#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <spdlog/spdlog.h>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "vr_shared.hpp"

// True-stereo D3D9 renderer.
//
// Simulation, input, timers, native FFB and OutRun's render queue are never
// replayed. For draws targeting the real game backbuffer, the normal draw is
// turned into the LEFT eye and the same D3D draw is repeated once into a
// matching full-size RIGHT-eye render target while all renderer state is alive.
//
// vr_renderer_probe.cpp remains the single head-tracking/recenter authority and
// latches one immutable head/FOV/IPD packet per successful game BeginScene. This
// file consumes that packet for every draw in the scene; it never re-samples the
// x64 host on a per-draw basis.
//
// World geometry is promoted to per-eye projection only when its live c64..c67
// values exactly correspond to a renderer-probe-verified OutRun WVP upload AND
// the current vertex-shader epoch is the same epoch that performed that upload.
// HUD/effect draws therefore fall back to identical zero-disparity duplication.
//
// At Present both full-size eye surfaces are resolved and drawn side-by-side to
// the real backbuffer. The x64 host crops that SBS transport and submits it as
// XrCompositionLayerProjection using the exact host pose sequence published by
// this renderer. Stereo metadata is published only after the real D3D9 Present
// succeeds, so Desktop Duplication can never pair a new pose with an older frame.

namespace Settings
{
	extern Setting<bool> VREnabled;
	extern Setting<bool> VRAutoEnableWhenHostPresent;
	extern Setting<bool> VRStereo;
	extern Setting<bool> VRDirectGpuOnly;
	extern Setting<float> VRWorldScale;
	extern Setting<float> VRStereoDepth;
	extern Setting<bool> VRTelemetry;
}

#ifndef OUTRUN_VR_BUILD_SHA
#define OUTRUN_VR_BUILD_SHA "unknown"
#endif

namespace OutRunVRStereo
{
	namespace
	{
		constexpr std::size_t ResetVtableIndex = 16;
		constexpr std::size_t PresentVtableIndex = 17;
		constexpr std::size_t SetRenderTargetVtableIndex = 37;
		constexpr std::size_t SetDepthStencilSurfaceVtableIndex = 39;
		constexpr std::size_t ClearVtableIndex = 43;
		constexpr std::size_t DrawPrimitiveVtableIndex = 81;
		constexpr std::size_t DrawIndexedPrimitiveVtableIndex = 82;
		constexpr std::size_t DrawPrimitiveUPVtableIndex = 83;
		constexpr std::size_t DrawIndexedPrimitiveUPVtableIndex = 84;
		constexpr std::size_t SetRenderStateVtableIndex = 57;
		constexpr std::size_t SetVertexShaderVtableIndex = 92;
		constexpr std::size_t CreateQueryVtableIndex = 118;
		constexpr std::size_t QueryIssueVtableIndex = 6;
		constexpr UINT OutRunWvpRegister = 64;
		constexpr UINT OutRunWvpRegisterCount = 4;
		constexpr std::uintptr_t OutRunProjectionRva = 0x0095D8A0u - 0x00400000u;
		constexpr float VerifiedWvpEpsilon = 1.0e-4f;

		SafetyHookInline ResetHook{};
		SafetyHookInline PresentHook{};
		SafetyHookInline SetRenderTargetHook{};
		SafetyHookInline SetDepthStencilSurfaceHook{};
		SafetyHookInline ClearHook{};
		SafetyHookInline DrawPrimitiveHook{};
		SafetyHookInline DrawIndexedPrimitiveHook{};
		SafetyHookInline DrawPrimitiveUPHook{};
		SafetyHookInline DrawIndexedPrimitiveUPHook{};
		SafetyHookInline SetRenderStateHook{};
		SafetyHookInline SetVertexShaderHook{};
		SafetyHookInline CreateQueryHook{};
		SafetyHookInline QueryIssueHook{};

		HANDLE SharedMapping = nullptr;
		OutRunVR::SharedPoseState* SharedState = nullptr;
		HANDLE RenderFrameMapping = nullptr;
		OutRunVR::SharedRenderFrameRing* RenderFrameRing = nullptr;
		std::uint32_t RenderFrameRunGeneration = 0;
		bool RenderFrameRingClaimed = false;

		IDirect3DSurface9* BackBuffer = nullptr;
		IDirect3DSurface9* TrackedRenderTarget = nullptr;
		IDirect3DSurface9* TrackedDepthStencil = nullptr;
		IDirect3DSurface9* RightEyeSurface = nullptr;
		IDirect3DSurface9* RightEyeDepth = nullptr;
		IDirect3DTexture9* LeftResolveTexture = nullptr;
		IDirect3DSurface9* LeftResolveSurface = nullptr;
		IDirect3DTexture9* RightResolveTexture = nullptr;
		IDirect3DSurface9* RightResolveSurface = nullptr;

		struct DirectTransportSlot
		{
			IDirect3DTexture9* leftTexture = nullptr;
			IDirect3DSurface9* leftSurface = nullptr;
			IDirect3DTexture9* rightTexture = nullptr;
			IDirect3DSurface9* rightSurface = nullptr;
			IDirect3DQuery9* fence = nullptr;
			HANDLE leftHandle = nullptr;
			HANDLE rightHandle = nullptr;
			std::uint32_t frameId = 0;
			bool producerPending = false;
			std::uint32_t pendingFrameId = 0;
			// True only after the producer fence completed and this slot was
			// advertised to the host. Unpublished timed-out slots need no ACK.
			bool published = false;
		};

		std::array<DirectTransportSlot, OutRunVR::RenderFrameRingSize> DirectTransportSlots{};
		IDirect3DTexture9* DirectInteropProbeTexture = nullptr;
		IDirect3DSurface9* DirectInteropProbeSurface = nullptr;
		IDirect3DQuery9* DirectInteropProbeFence = nullptr;
		HANDLE DirectInteropProbeHandle = nullptr;
		std::uint32_t DirectInteropProbeToken = 0;
		std::uint32_t ActiveDirectTransportSlot = 0;
		D3DFORMAT DirectTransportFormat = D3DFMT_UNKNOWN;
		std::uint32_t DirectTransportGeneration = 0;
		std::uint32_t DirectTransportWidth = 0;
		std::uint32_t DirectTransportHeight = 0;
		bool DirectTransportResourcesReady = false;
		bool DirectInteropVerified = false;

		D3DSURFACE_DESC BackBufferDesc{};
		bool StereoResourcesReady = false;
		thread_local bool InternalStereoPass = false;
		std::array<bool, 3> AuxRenderTargetActive{};

		std::atomic<std::uintptr_t> CurrentVertexShaderIdentity{ 0 };
		std::atomic<std::uint64_t> VertexShaderSerial{ 0 };
		std::array<DWORD, 256> TrackedRenderStateValue{};
		std::array<bool, 256> TrackedRenderStateValid{};
		std::array<std::uint8_t, 256> TrackedRenderStateReads{};
		std::uint64_t TrackedRenderStateCacheHits = 0;
		std::uint64_t TrackedRenderStateLiveValidations = 0;
		std::uint64_t TrackedRenderStateMismatches = 0;
		std::atomic<int> ActiveOcclusionQueries{ 0 };
		std::atomic<bool> OcclusionQueryTrackingUnavailable{ false };
		std::atomic<bool> StereoBaseCallbacksArmed{ false };

		D3DMATRIX CachedProjection{};
		D3DMATRIX CachedInverseProjection{};
		bool ProjectionInverseValid = false;

		bool FrameHadDuplicatedDraw = false;
		bool FrameHadWorldStereo = false;
		bool FrameRightDrawFailed = false;
		bool FrameStereoIncomplete = false;
		OutRunVR::StereoFailureReason FrameFailureReason = OutRunVR::StereoFailureNone;
		std::uint32_t FrameStereoPoseSequence = 0;
		OutRunVRRenderer::LatchedStereoFrame FrameStereoMetadata{};
		std::uint32_t StereoFrameCounter = 0;
		bool RightDepthSynchronized = true;
		bool RightStencilSynchronized = true;
		bool LastStereoWanted = false;
		bool FramePoseMismatchLogged = false;
		std::uint64_t PresentEpoch = 1;
		std::uint64_t LastMainDepthClearEpoch = 0;
		DWORD LastMainDepthClearFlags = 0;
		float LastMainDepthClearZ = 1.0f;
		DWORD LastMainDepthClearStencil = 0;
		D3DSURFACE_DESC LastMainDepthClearDesc{};

		ULONGLONG LastSummaryMs = 0;
		std::uint64_t DuplicatedDraws = 0;
		std::uint64_t WorldStereoDraws = 0;
		std::uint64_t NonWorldDuplicatedDraws = 0;
		std::uint64_t StereoComposeSuccess = 0;
		std::uint64_t StereoComposeFailure = 0;
		std::uint64_t MrtRejectedDraws = 0;
		std::uint64_t RestoreFailures = 0;
		std::uint64_t WorldClassificationFailures = 0;
		std::uint64_t OffscreenVerifiedWorldDraws = 0;
		std::uint64_t PoseSequenceMismatchFrames = 0;
		std::uint64_t MultiBeginScenePresents = 0;
		std::uint64_t MaxBeginScenesPerPresent = 0;
		std::uint64_t LastBeginSceneCountAtPresent = 0;
		std::uint64_t FixedFunctionNonWorldDraws = 0;
		std::uint64_t FixedFunctionWorldStereoDraws = 0;
		std::uint64_t FixedFunctionSkyRotationOnlyDraws = 0;
		bool FirstFixedFunctionWorldLogged = false;
		bool FirstFixedFunctionSkyRotationLogged = false;
		std::uint64_t OcclusionQueriesCreated = 0;
		std::uint64_t OcclusionQueryBegins = 0;
		std::uint64_t OcclusionQueryEnds = 0;
		std::uint64_t OcclusionStereoRejects = 0;
		std::uint64_t DirectTransportFrames = 0;
		std::uint64_t DirectTransportFallbacks = 0;
		std::uint64_t DirectTransportFenceTimeouts = 0;
		std::uint64_t DirectTransportRingBackpressure = 0;
		std::uint64_t DirectInteropProbeFailures = 0;
		std::uint64_t FullMainDepthClears = 0;
		std::uint64_t DepthBootstrapAttempts = 0;
		std::uint64_t DepthBootstrapSuccesses = 0;
		std::array<std::uint64_t, 32> FailureCounts{};
		bool FirstStereoActiveLogged = false;
		bool FirstWorldStereoLogged = false;
		bool FirstRestoreFailureLogged = false;
		bool FirstMrtRejectLogged = false;
		bool FirstOffscreenWorldLogged = false;
		bool FirstClassificationFailureLogged = false;
		bool FirstOcclusionRejectLogged = false;
		bool FirstOcclusionTrackingUnavailableLogged = false;
		bool FirstDirectTransportLogged = false;
		bool FirstDirectFallbackLogged = false;
		bool FirstDirectOnlyNoFrameLogged = false;
		bool FirstDirectDeferredLogged = false;
		bool FirstD3D9ExUnavailableLogged = false;
		bool FirstAdapterMismatchLogged = false;
		bool FirstInteropVerifiedLogged = false;
		bool FirstMainDepthReuseLogged = false;
		bool FirstMainClearLogged = false;
		bool FirstDepthBootstrapLogged = false;

		template <typename T>
		void ReleaseCom(T*& value)
		{
			if (value)
			{
				value->Release();
				value = nullptr;
			}
		}

		void ReplaceSurfaceRef(IDirect3DSurface9*& slot, IDirect3DSurface9* value)
		{
			if (value)
				value->AddRef();
			ReleaseCom(slot);
			slot = value;
		}

		struct InternalPassScope
		{
			bool previous = false;
			InternalPassScope() : previous(InternalStereoPass) { InternalStereoPass = true; }
			~InternalPassScope() { InternalStereoPass = previous; }
		};

		bool IsGameDevice(IDirect3DDevice9* device)
		{
			return device && Game::D3DDevice_ptr && *Game::D3DDevice_ptr == device;
		}

		bool FormatHasStencil(D3DFORMAT format);

		bool GameplayActive()
		{
			if (!Game::current_mode)
				return false;

			// Reuse the game's established "in-game" state definition instead of
			// treating only STATE_GAME as gameplay. GOAL/TIMEUP/TRYAGAIN,
			// OUTRUNMILES and the single-player pause state can keep rendering the
			// live 3D scene. Switching those frames to theater immediately clears
			// stereo metadata and can leave the HMD without a fresh projection
			// source while the monitor continues normally.
			if (Game::is_in_game())
				return true;

			// WARP/RESTART are short race-adjacent transition states. Preserve
			// gameplay presentation through them; the following menu/result states
			// still fall back to theater normally.
			switch (*Game::current_mode)
			{
			case GameState::STATE_WARP:
			case GameState::STATE_RESTART:
				return true;
			default:
				return false;
			}
		}

		bool HostRenderEligible()
		{
			if (!SharedState || SharedState->magic != OutRunVR::SharedMagic ||
				SharedState->protocolVersion != OutRunVR::SharedProtocolVersion ||
				SharedState->structSize != sizeof(OutRunVR::SharedPoseState))
				return false;
			const std::uint32_t flags = SharedState->flags;

			// Game-side stereo source generation is gated only by hard host
			// liveness/visibility. XrFrameState::shouldRender is transient compositor
			// advice to the x64 host; using it here used to zero the SBS frame metadata
			// and tear down the eye source for several Presents even though the host
			// heartbeat/session remained healthy. The host still decides independently
			// whether a given OpenXR frame submits a layer.
			return (flags & OutRunVR::HostAlive) != 0 &&
				(flags & OutRunVR::SessionVisible) != 0;
		}

		bool HostDirectTransportSupported()
		{
			return SharedState && (SharedState->flags & OutRunVR::HostDirectGpuTransport) != 0;
		}

		bool HostDirectTransportReady()
		{
			return SharedState && (SharedState->flags & OutRunVR::HostDirectGpuReady) != 0;
		}

		bool StereoWanted()
		{
			const bool hostEligible = HostRenderEligible();
			const bool wanted = Settings::VRStereo && GameplayActive() && hostEligible &&
				(Settings::VREnabled || Settings::VRAutoEnableWhenHostPresent);
			if (wanted && !LastStereoWanted && TrackedDepthStencil)
			{
				RightDepthSynchronized = false;
				D3DSURFACE_DESC d{};
				if (SUCCEEDED(TrackedDepthStencil->GetDesc(&d)) && FormatHasStencil(d.Format))
					RightStencilSynchronized = false;
			}
			LastStereoWanted = wanted;
			return wanted;
		}

		bool AnyAuxRenderTargetActive()
		{
			return AuxRenderTargetActive[0] || AuxRenderTargetActive[1] || AuxRenderTargetActive[2];
		}

		bool TargetIsBackBuffer()
		{
			return BackBuffer && TrackedRenderTarget && TrackedRenderTarget == BackBuffer;
		}

		bool IsReadableRange(const void* address, std::size_t size)
		{
			if (!address || !size)
				return false;
			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info))
				return false;
			if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) || (info.Protect & PAGE_NOACCESS))
				return false;
			const auto begin = reinterpret_cast<std::uintptr_t>(address);
			const auto regionBegin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
			const auto regionEnd = regionBegin + info.RegionSize;
			return begin >= regionBegin && begin + size >= begin && begin + size <= regionEnd;
		}

		bool ImageContainsRange(std::uintptr_t rva, std::size_t size)
		{
			const auto base = reinterpret_cast<std::uintptr_t>(Module::ExeHandle);
			if (!base || !IsReadableRange(reinterpret_cast<const void*>(base), sizeof(IMAGE_DOS_HEADER)))
				return false;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
				return false;
			const auto ntAddress = base + static_cast<std::uintptr_t>(dos->e_lfanew);
			if (!IsReadableRange(reinterpret_cast<const void*>(ntAddress), sizeof(IMAGE_NT_HEADERS)))
				return false;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(ntAddress);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return false;
			const std::size_t imageSize = nt->OptionalHeader.SizeOfImage;
			return rva <= imageSize && size <= imageSize - rva;
		}

		bool MatrixFinite(const D3DMATRIX& matrix)
		{
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					if (!std::isfinite(matrix.m[r][c]))
						return false;
			return true;
		}

		D3DMATRIX IdentityMatrix()
		{
			D3DMATRIX out{};
			out._11 = out._22 = out._33 = out._44 = 1.0f;
			return out;
		}

		D3DMATRIX MatrixFromQuaternionTranslation(const float qIn[4], const float position[3], float positionScale)
		{
			float x=qIn[0],y=qIn[1],z=qIn[2],w=qIn[3]; const float lenSq=x*x+y*y+z*z+w*w;
			if (!std::isfinite(lenSq)||lenSq<=1.0e-12f) return IdentityMatrix(); const float inv=1.0f/std::sqrt(lenSq); x*=inv;y*=inv;z*=inv;w*=inv;
			D3DMATRIX out=IdentityMatrix(); const float xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,xw=x*w,yw=y*w,zw=z*w;
			out._11=1-2*(yy+zz);out._12=2*(xy+zw);out._13=2*(xz-yw);out._21=2*(xy-zw);out._22=1-2*(xx+zz);out._23=2*(yz+xw);
			out._31=2*(xz+yw);out._32=2*(yz-xw);out._33=1-2*(xx+yy);out._41=position[0]*positionScale;out._42=position[1]*positionScale;out._43=position[2]*positionScale; return out;
		}
		D3DMATRIX InverseRigid(const D3DMATRIX& m)
		{
			D3DMATRIX out=IdentityMatrix(); out._11=m._11;out._12=m._21;out._13=m._31;out._21=m._12;out._22=m._22;out._23=m._32;out._31=m._13;out._32=m._23;out._33=m._33;
			out._41=-(m._41*out._11+m._42*out._21+m._43*out._31);out._42=-(m._41*out._12+m._42*out._22+m._43*out._32);out._43=-(m._41*out._13+m._42*out._23+m._43*out._33); return out;
		}

		D3DMATRIX MultiplyMatrix(const D3DMATRIX& a, const D3DMATRIX& b)
		{
			D3DMATRIX out{};
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					for (int k = 0; k < 4; ++k)
						out.m[r][c] += a.m[r][k] * b.m[k][c];
			return out;
		}

		D3DMATRIX TransposeMatrix(const D3DMATRIX& in)
		{
			D3DMATRIX out{};
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					out.m[r][c] = in.m[c][r];
			return out;
		}

		bool InvertMatrix(const D3DMATRIX& in, D3DMATRIX& out)
		{
			float a[4][8]{};
			for (int r = 0; r < 4; ++r)
			{
				for (int c = 0; c < 4; ++c)
					a[r][c] = in.m[r][c];
				a[r][4 + r] = 1.0f;
			}
			for (int col = 0; col < 4; ++col)
			{
				int pivot = col;
				float pivotAbs = std::fabs(a[pivot][col]);
				for (int r = col + 1; r < 4; ++r)
				{
					const float candidate = std::fabs(a[r][col]);
					if (candidate > pivotAbs)
					{
						pivot = r;
						pivotAbs = candidate;
					}
				}
				if (!std::isfinite(pivotAbs) || pivotAbs < 1.0e-8f)
					return false;
				if (pivot != col)
					for (int c = 0; c < 8; ++c)
						std::swap(a[pivot][c], a[col][c]);
				const float divisor = a[col][col];
				for (int c = 0; c < 8; ++c)
					a[col][c] /= divisor;
				for (int r = 0; r < 4; ++r)
				{
					if (r == col)
						continue;
					const float factor = a[r][col];
					for (int c = 0; c < 8; ++c)
						a[r][c] -= factor * a[col][c];
				}
			}
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					out.m[r][c] = a[r][c + 4];
			return MatrixFinite(out);
		}

		bool FloatArrayNear(const float* a, const float* b, std::size_t count, float epsilon)
		{
			if (!a || !b)
				return false;
			for (std::size_t i = 0; i < count; ++i)
				if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::fabs(a[i] - b[i]) > epsilon)
					return false;
			return true;
		}

		D3DMATRIX ProjectionFromFov(const D3DMATRIX& base, const OutRunVR::SharedFov& fov)
		{
			const float tanLeft = std::tan(fov.angleLeft);
			const float tanRight = std::tan(fov.angleRight);
			const float tanUp = std::tan(fov.angleUp);
			const float tanDown = std::tan(fov.angleDown);
			const float width = tanRight - tanLeft;
			const float height = tanUp - tanDown;
			D3DMATRIX out{};
			out._11 = 2.0f / width;
			out._22 = 2.0f / height;
			out._31 = (tanRight + tanLeft) / width;
			out._32 = (tanUp + tanDown) / height;
			out._33 = base._33;
			out._34 = base._34;
			out._43 = base._43;
			out._44 = base._44;
			return out;
		}

		bool ReadProjection(D3DMATRIX& projection)
		{
			float rendererProjection[16]{};
			if (OutRunVRRenderer::GetRendererBaseProjection(rendererProjection))
				std::memcpy(&projection, rendererProjection, sizeof(projection));
			else
			{
				if (!ImageContainsRange(OutRunProjectionRva, sizeof(D3DMATRIX))) return false;
				const auto* projectionPtr = Module::exe_ptr<D3DMATRIX>(OutRunProjectionRva);
				if (!IsReadableRange(projectionPtr, sizeof(D3DMATRIX))) return false;
				std::memcpy(&projection, projectionPtr, sizeof(projection));
			}
			return MatrixFinite(projection) && std::fabs(projection._34 + 1.0f) < 0.25f && std::fabs(projection._44) < 0.25f;
		}

		bool GetInverseProjection(const D3DMATRIX& projection, D3DMATRIX& inverse)
		{
			if (!ProjectionInverseValid || std::memcmp(&projection, &CachedProjection, sizeof(projection)) != 0)
			{
				D3DMATRIX candidate{};
				if (!InvertMatrix(projection, candidate)) return false;
				CachedProjection = projection;
				CachedInverseProjection = candidate;
				ProjectionInverseValid = true;
			}
			inverse = CachedInverseProjection;
			return true;
		}

		bool EnsureSharedState()
		{
			if (SharedState)
				return SharedState->magic == OutRunVR::SharedMagic && SharedState->protocolVersion == OutRunVR::SharedProtocolVersion && SharedState->structSize == sizeof(OutRunVR::SharedPoseState);
			SharedMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(OutRunVR::SharedPoseState)), OutRunVR::SharedMemoryName);
			if (!SharedMapping) return false;
			const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
			SharedState = static_cast<OutRunVR::SharedPoseState*>(MapViewOfFile(SharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OutRunVR::SharedPoseState)));
			if (!SharedState) { CloseHandle(SharedMapping); SharedMapping = nullptr; return false; }
			if (!existed)
			{
				std::memset(SharedState, 0, sizeof(*SharedState));
				SharedState->protocolVersion = OutRunVR::SharedProtocolVersion;
				SharedState->structSize = sizeof(*SharedState);
				MemoryBarrier();
				SharedState->magic = OutRunVR::SharedMagic;
			}
			return true;
		}

		std::uint32_t MakeRenderFrameRunGeneration() noexcept
		{
			LARGE_INTEGER qpc{};
			QueryPerformanceCounter(&qpc);
			std::uint32_t generation =
				static_cast<std::uint32_t>(qpc.LowPart) ^
				static_cast<std::uint32_t>(qpc.HighPart) ^
				GetCurrentProcessId() ^
				GetTickCount() ^
				0x52554E47u; // 'RUNG'
			if (!generation)
				generation = 1;
			return generation;
		}

		bool ClaimRenderFrameRingForCurrentRun() noexcept
		{
			if (!RenderFrameRing)
				return false;
			const bool headerValid =
				RenderFrameRing->magic == OutRunVR::RenderFrameMagic &&
				RenderFrameRing->protocolVersion == OutRunVR::RenderFrameProtocolVersion &&
				RenderFrameRing->structSize == sizeof(OutRunVR::SharedRenderFrameRing) &&
				RenderFrameRing->slotCount == OutRunVR::RenderFrameRingSize;
			if (!headerValid)
				return false;

			const std::uint32_t self = GetCurrentProcessId();
			if (!RenderFrameRunGeneration)
				RenderFrameRunGeneration = MakeRenderFrameRunGeneration();

			if (RenderFrameRingClaimed)
				return RenderFrameRing->clientPid == self &&
					RenderFrameRing->reserved0 == RenderFrameRunGeneration;

			// The host may keep Frame.v2 mapped across a fast game restart.
			// Claim a new run generation under the ring seqlock and invalidate
			// every old slot before publishing any frame from this process.
			LONG ringSeq = InterlockedIncrement(
				reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));
			if ((ringSeq & 1) == 0)
				ringSeq = InterlockedIncrement(
					reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));
			MemoryBarrier();

			RenderFrameRing->clientPid = self;
			RenderFrameRing->reserved0 = RenderFrameRunGeneration;
			RenderFrameRing->latestSlot = 0;
			for (auto& slot : RenderFrameRing->slots)
			{
				std::memset(&slot, 0, sizeof(slot));
				slot.protocolVersion = OutRunVR::RenderFrameProtocolVersion;
				slot.structSize = sizeof(slot);
				slot.clientPid = self;
				slot.reserved[OutRunVR::RenderFrameRunGenerationIndex] =
					RenderFrameRunGeneration;
				MemoryBarrier();
				slot.magic = OutRunVR::RenderFrameMagic;
			}

			MemoryBarrier();
			ringSeq = InterlockedIncrement(
				reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));
			if (ringSeq & 1)
				InterlockedIncrement(
					reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));

			RenderFrameRingClaimed = true;
			spdlog::info(
				"VR Frame.v2: claimed run identity pid={} generation=0x{:08x}; stale slots from previous game runs invalidated",
				self, RenderFrameRunGeneration);
			return true;
		}

		bool EnsureRenderFrameState()
		{
			if (RenderFrameRing)
				return ClaimRenderFrameRingForCurrentRun();

			RenderFrameMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(OutRunVR::SharedRenderFrameRing)), OutRunVR::RenderFrameMemoryName);
			if (!RenderFrameMapping) return false;
			const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
			RenderFrameRing = static_cast<OutRunVR::SharedRenderFrameRing*>(MapViewOfFile(RenderFrameMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OutRunVR::SharedRenderFrameRing)));
			if (!RenderFrameRing) return false;
			if (!existed)
			{
				std::memset(RenderFrameRing, 0, sizeof(*RenderFrameRing));
				RenderFrameRing->protocolVersion = OutRunVR::RenderFrameProtocolVersion;
				RenderFrameRing->structSize = sizeof(*RenderFrameRing);
				RenderFrameRing->slotCount = OutRunVR::RenderFrameRingSize;
				for (auto& slot : RenderFrameRing->slots)
				{
					slot.protocolVersion = OutRunVR::RenderFrameProtocolVersion;
					slot.structSize = sizeof(slot);
					slot.magic = OutRunVR::RenderFrameMagic;
				}
				MemoryBarrier();
				RenderFrameRing->magic = OutRunVR::RenderFrameMagic;
			}
			return ClaimRenderFrameRingForCurrentRun();
		}

		void PoisonFrame(OutRunVR::StereoFailureReason reason)
		{
			FrameStereoIncomplete = true;
			if (FrameFailureReason == OutRunVR::StereoFailureNone)
			{
				FrameFailureReason = reason;
				const auto index = static_cast<std::size_t>(reason);
				if (index < FailureCounts.size()) ++FailureCounts[index];
			}
		}

		bool ReadTrackedRenderState(IDirect3DDevice9* device,
			D3DRENDERSTATETYPE state, DWORD& out) noexcept;

		enum class EyeBuildResult { NonWorld, WorldStereo, FatalError };
		struct DrawStereoState
		{
			float originalConstants[16]{};
			float eyeConstants[2][16]{};
			D3DMATRIX fixedOriginalView{};
			D3DMATRIX fixedOriginalProjection{};
			D3DMATRIX fixedEyeView[2]{};
			D3DMATRIX fixedEyeProjection[2]{};
			bool worldStereo = false;
			bool fixedFunctionWorld = false;
			bool fixedFunctionRotationOnly = false;
			std::uint32_t poseSequence = 0;
			OutRunVRRenderer::LatchedStereoFrame stereoFrame{};
		};

		EyeBuildResult BuildEyeConstants(IDirect3DDevice9* device, const OutRunVRRenderer::LatchedStereoFrame& stereo, DrawStereoState& state)
		{
			const std::uintptr_t trackedShader =
				CurrentVertexShaderIdentity.load(std::memory_order_acquire);
			if (trackedShader == 0)
			{
				// R13/R29 intentionally masks the tracked identity to zero for
				// shader-based zero-disparity effects. Confirm the real D3D9
				// binding before treating this as a genuine fixed-function draw.
				IDirect3DVertexShader9* boundShader = nullptr;
				const HRESULT boundHr = device->GetVertexShader(&boundShader);
				if (FAILED(boundHr))
				{
					++FixedFunctionNonWorldDraws;
					return EyeBuildResult::NonWorld;
				}
				if (boundShader)
				{
					boundShader->Release();
					return EyeBuildResult::NonWorld;
				}

				DWORD fvf = 0;
				if (FAILED(device->GetFVF(&fvf)) || fvf == 0)
				{
					++FixedFunctionNonWorldDraws;
					return EyeBuildResult::NonWorld;
				}

				const DWORD positionType = fvf & D3DFVF_POSITION_MASK;
				switch (positionType)
				{
				case D3DFVF_XYZ:
				case D3DFVF_XYZB1:
				case D3DFVF_XYZB2:
				case D3DFVF_XYZB3:
				case D3DFVF_XYZB4:
				case D3DFVF_XYZB5:
					break;
				default:
					// XYZRHW and unknown/clip-space formats are screen-space
					// or otherwise already transformed; keep zero disparity.
					++FixedFunctionNonWorldDraws;
					return EyeBuildResult::NonWorld;
				}

				if (FAILED(device->GetTransform(
						D3DTS_VIEW, &state.fixedOriginalView)) ||
					FAILED(device->GetTransform(
						D3DTS_PROJECTION, &state.fixedOriginalProjection)))
				{
					++FixedFunctionNonWorldDraws;
					return EyeBuildResult::NonWorld;
				}

				const auto projectionClass =
					OutRunVR::PassPolicy::ClassifyProjectionSignature(
						state.fixedOriginalProjection._34,
						state.fixedOriginalProjection._44);
				if (projectionClass !=
					OutRunVR::PassPolicy::ProjectionClass::Perspective3D)
				{
					++FixedFunctionNonWorldDraws;
					return EyeBuildResult::NonWorld;
				}

				// Car-select and other theater/menu previews also use
				// fixed-function perspective XYZ. They are not VR world geometry
				// and must not receive the headset inverse/eye transforms.
				DWORD zEnable = D3DZB_TRUE;
				if (!ReadTrackedRenderState(device, D3DRS_ZENABLE, zEnable))
				{
					++FixedFunctionNonWorldDraws;
					return EyeBuildResult::NonWorld;
				}
				const auto fixedPolicy =
					OutRunVR::PassPolicy::ClassifyFixedFunctionStereo(
						projectionClass, GameplayActive(),
						zEnable != D3DZB_FALSE);
				if (fixedPolicy ==
					OutRunVR::PassPolicy::FixedFunctionStereoPolicy::NonWorld)
				{
					++FixedFunctionNonWorldDraws;
					return EyeBuildResult::NonWorld;
				}
				const bool rotationOnly =
					fixedPolicy ==
					OutRunVR::PassPolicy::FixedFunctionStereoPolicy::SkyRotationOnly;

				if (stereo.poseSequence == 0 ||
					!MatrixFinite(state.fixedOriginalView) ||
					!MatrixFinite(state.fixedOriginalProjection))
					return EyeBuildResult::FatalError;

				float headInverseRaw[16]{};
				std::uint32_t headPoseSequence = 0;
				D3DMATRIX headInverse{};
				if (!OutRunVRRenderer::GetLatchedHeadInverse(
						headInverseRaw, headPoseSequence) ||
					headPoseSequence != stereo.poseSequence)
					return EyeBuildResult::FatalError;
				std::memcpy(&headInverse, headInverseRaw,
					sizeof(headInverse));
				if (!MatrixFinite(headInverse))
					return EyeBuildResult::FatalError;

				D3DMATRIX appliedHeadInverse = headInverse;
				if (rotationOnly)
				{
					appliedHeadInverse._41 = 0.0f;
					appliedHeadInverse._42 = 0.0f;
					appliedHeadInverse._43 = 0.0f;
				}

				for (int eye = 0; eye < 2; ++eye)
				{
					float eyeOffset[3]{
						stereo.eyeOffset[eye][0],
						stereo.eyeOffset[eye][1],
						stereo.eyeOffset[eye][2]
					};
					if (rotationOnly)
						eyeOffset[0] = eyeOffset[1] = eyeOffset[2] = 0.0f;
					const D3DMATRIX eyePose =
						MatrixFromQuaternionTranslation(
							stereo.eyeOrientation[eye], eyeOffset,
							Settings::VRWorldScale * Settings::VRStereoDepth);
					const D3DMATRIX eyeInverse = InverseRigid(eyePose);
					state.fixedEyeView[eye] =
						MultiplyMatrix(
							MultiplyMatrix(
								state.fixedOriginalView, appliedHeadInverse),
							eyeInverse);
					state.fixedEyeProjection[eye] =
						ProjectionFromFov(
							state.fixedOriginalProjection,
							stereo.eyeFov[eye]);
					if (!MatrixFinite(state.fixedEyeView[eye]) ||
						!MatrixFinite(state.fixedEyeProjection[eye]))
						return EyeBuildResult::FatalError;
				}

				state.fixedFunctionWorld = true;
				state.fixedFunctionRotationOnly = rotationOnly;
				state.worldStereo = true;
				state.poseSequence = stereo.poseSequence;
				if (rotationOnly)
				{
					++FixedFunctionSkyRotationOnlyDraws;
					if (!FirstFixedFunctionSkyRotationLogged)
					{
						FirstFixedFunctionSkyRotationLogged = true;
						spdlog::info(
							"VR SKY ROTATION: depth-off fixed-function perspective draw keeps headset rotation/FOV but removes head/IPD translation");
					}
				}
				else
				{
					++FixedFunctionWorldStereoDraws;
					if (!FirstFixedFunctionWorldLogged)
					{
						FirstFixedFunctionWorldLogged = true;
						spdlog::info(
							"VR FIXEDFN WORLD: depth-tested perspective XYZ receives latched head inverse + eye-local IPD/FOV");
					}
				}
				return EyeBuildResult::WorldStereo;
			}

			float verifiedConstants[16]{};
			std::uint32_t verifiedGeneration = 0, verifiedPoseSequence = 0;
			std::uintptr_t verifiedShaderIdentity = 0;
			std::uint64_t verifiedShaderSerial = 0;
			if (!OutRunVRRenderer::GetLastVerifiedWvp(verifiedConstants, verifiedGeneration, verifiedPoseSequence, verifiedShaderIdentity, verifiedShaderSerial)) return EyeBuildResult::NonWorld;
			std::uintptr_t shaderIdentity = 0; std::uint64_t shaderSerial = 0;
			if (!GetCurrentShaderEpoch(shaderIdentity, shaderSerial)) return EyeBuildResult::FatalError;
			if (verifiedShaderIdentity != shaderIdentity || verifiedShaderSerial != shaderSerial) return EyeBuildResult::NonWorld;
			if (verifiedGeneration == 0 || verifiedPoseSequence != stereo.poseSequence) return EyeBuildResult::FatalError;
			if (FAILED(device->GetVertexShaderConstantF(OutRunWvpRegister, state.originalConstants, OutRunWvpRegisterCount))) return EyeBuildResult::FatalError;
			if (!FloatArrayNear(state.originalConstants, verifiedConstants, 16, VerifiedWvpEpsilon)) return EyeBuildResult::FatalError;
			D3DMATRIX projection{}, invProjection{};
			if (!ReadProjection(projection) || !GetInverseProjection(projection, invProjection)) return EyeBuildResult::FatalError;
			D3DMATRIX uploadedT{}; std::memcpy(&uploadedT, state.originalConstants, sizeof(uploadedT));
			const D3DMATRIX currentWvp = TransposeMatrix(uploadedT);
			const D3DMATRIX correctedWorldView = MultiplyMatrix(currentWvp, invProjection);
			if (!MatrixFinite(correctedWorldView)) return EyeBuildResult::FatalError;
			for (int eye = 0; eye < 2; ++eye)
			{
				const D3DMATRIX eyePose = MatrixFromQuaternionTranslation(stereo.eyeOrientation[eye], stereo.eyeOffset[eye], Settings::VRWorldScale * Settings::VRStereoDepth);
				const D3DMATRIX eyeInverse = InverseRigid(eyePose);
				const D3DMATRIX eyeProjection = ProjectionFromFov(projection, stereo.eyeFov[eye]);
				const D3DMATRIX eyeWvp = MultiplyMatrix(MultiplyMatrix(correctedWorldView, eyeInverse), eyeProjection);
				if (!MatrixFinite(eyeWvp)) return EyeBuildResult::FatalError;
				const D3DMATRIX eyeWvpT = TransposeMatrix(eyeWvp); std::memcpy(state.eyeConstants[eye], &eyeWvpT, sizeof(eyeWvpT));
			}
			state.worldStereo = true; state.poseSequence = stereo.poseSequence; return EyeBuildResult::WorldStereo;
		}

		void InvalidateTrackedRenderStates() noexcept
		{
			TrackedRenderStateValid.fill(false);
			TrackedRenderStateReads.fill(0);
		}

		bool ReadTrackedRenderState(IDirect3DDevice9* device,
			D3DRENDERSTATETYPE state, DWORD& out) noexcept
		{
			const auto index = static_cast<std::size_t>(state);
			if (!device || index >= TrackedRenderStateValue.size())
				return false;
			if (TrackedRenderStateValid[index])
			{
				auto& reads = TrackedRenderStateReads[index];
				if (++reads < 16)
				{
					++TrackedRenderStateCacheHits;
					out = TrackedRenderStateValue[index];
					return true;
				}
				reads = 0;
			}
			DWORD live = 0;
			if (FAILED(device->GetRenderState(state, &live)))
			{
				if (!TrackedRenderStateValid[index])
					return false;
				out = TrackedRenderStateValue[index];
				return true;
			}
			++TrackedRenderStateLiveValidations;
			if (TrackedRenderStateValid[index] &&
				TrackedRenderStateValue[index] != live)
				++TrackedRenderStateMismatches;
			TrackedRenderStateValue[index] = live;
			TrackedRenderStateValid[index] = true;
			out = live;
			return true;
		}

		void PrimeTrackedRenderStates(IDirect3DDevice9* device) noexcept
		{
			InvalidateTrackedRenderStates();
			if (!device) return;
			constexpr D3DRENDERSTATETYPE states[] = {
				D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
				D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
				D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE,
				D3DRS_STENCILENABLE, D3DRS_STENCILWRITEMASK,
				D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
				D3DRS_STENCILPASS, D3DRS_TWOSIDEDSTENCILMODE,
				D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
				D3DRS_CCW_STENCILPASS
			};
			for (const auto state : states)
			{
				const auto index = static_cast<std::size_t>(state);
				DWORD value = 0;
				if (index < TrackedRenderStateValue.size() &&
					SUCCEEDED(device->GetRenderState(state, &value)))
				{
					TrackedRenderStateValue[index] = value;
					TrackedRenderStateValid[index] = true;
				}
			}
		}

		HRESULT __stdcall SetRenderStateDest(IDirect3DDevice9* device,
			D3DRENDERSTATETYPE state, DWORD value)
		{
			const HRESULT hr = SetRenderStateHook.stdcall<HRESULT>(
				device, state, value);
			const auto index = static_cast<std::size_t>(state);
			if (SUCCEEDED(hr) && !InternalStereoPass && IsGameDevice(device) &&
				index < TrackedRenderStateValue.size())
			{
				TrackedRenderStateValue[index] = value;
				TrackedRenderStateValid[index] = true;
				TrackedRenderStateReads[index] = 0;
			}
			return hr;
		}

		bool CurrentDrawMatchesVerifiedWorld(IDirect3DDevice9* device)
		{
			(void)device;
			float verified[16]{}, current[16]{};
			std::uint32_t generation = 0, poseSequence = 0;
			std::uintptr_t verifiedShader = 0, currentShader = 0, writeShader = 0;
			std::uint64_t verifiedSerial = 0, currentSerial = 0;
			std::uint64_t writeSerial = 0, writeDrawSerial = 0, writeShaderSerial = 0;
			if (!OutRunVRRenderer::GetLastVerifiedWvp(
					verified, generation, poseSequence,
					verifiedShader, verifiedSerial) ||
				generation == 0 || poseSequence == 0)
				return false;
			if (!GetCurrentShaderEpoch(currentShader, currentSerial) ||
				currentShader != verifiedShader || currentSerial != verifiedSerial)
				return false;
			if (!OutRunVRRenderer::GetLastGameWvpWrite(
					current, writeSerial, writeDrawSerial,
					writeShader, writeShaderSerial) ||
				writeShader != currentShader || writeShaderSerial != currentSerial)
				return false;
			return FloatArrayNear(current, verified, 16, VerifiedWvpEpsilon);
		}

		bool SetWvpOneRegisterAtATime(IDirect3DDevice9* device, const float* constants)
		{
			for (UINT reg = 0; reg < OutRunWvpRegisterCount; ++reg)
				if (FAILED(device->SetVertexShaderConstantF(OutRunWvpRegister + reg, constants + reg * 4, 1))) return false;
			return true;
		}

		bool SetFixedFunctionEyeTransforms(IDirect3DDevice9* device,
			const DrawStereoState& draw, int eye)
		{
			return device && eye >= 0 && eye < 2 &&
				SUCCEEDED(device->SetTransform(
					D3DTS_VIEW, &draw.fixedEyeView[eye])) &&
				SUCCEEDED(device->SetTransform(
					D3DTS_PROJECTION, &draw.fixedEyeProjection[eye]));
		}

		bool RestoreFixedFunctionTransforms(IDirect3DDevice9* device,
			const DrawStereoState& draw)
		{
			return device &&
				SUCCEEDED(device->SetTransform(
					D3DTS_VIEW, &draw.fixedOriginalView)) &&
				SUCCEEDED(device->SetTransform(
					D3DTS_PROJECTION, &draw.fixedOriginalProjection));
		}

		bool FormatHasStencil(D3DFORMAT format)
		{
			switch (format) { case D3DFMT_D15S1: case D3DFMT_D24S8: case D3DFMT_D24X4S4: case D3DFMT_D24FS8: return true; default: return false; }
		}
		bool SurfaceHasStencil(IDirect3DSurface9* surface) { if (!surface) return false; D3DSURFACE_DESC desc{}; return SUCCEEDED(surface->GetDesc(&desc)) && FormatHasStencil(desc.Format); }
		bool DepthTestActive(IDirect3DDevice9* device) { if (!device || !TrackedDepthStencil) return false; DWORD enabled=D3DZB_TRUE; if (!ReadTrackedRenderState(device,D3DRS_ZENABLE,enabled)) return true; return enabled!=D3DZB_FALSE; }
		bool StencilTestActive(IDirect3DDevice9* device) { if (!device || !SurfaceHasStencil(TrackedDepthStencil)) return false; DWORD enabled=FALSE; if (!ReadTrackedRenderState(device,D3DRS_STENCILENABLE,enabled)) return true; return enabled!=FALSE; }
		bool LeftDrawMayWriteDepth(IDirect3DDevice9* device) { if(!device||!TrackedDepthStencil)return false;DWORD zEnable=D3DZB_TRUE,zWrite=TRUE;if(!ReadTrackedRenderState(device,D3DRS_ZENABLE,zEnable)||!ReadTrackedRenderState(device,D3DRS_ZWRITEENABLE,zWrite))return true;return zEnable!=D3DZB_FALSE&&zWrite!=FALSE; }
		bool LeftDrawMayWriteStencil(IDirect3DDevice9* device)
		{
			if(!device||!SurfaceHasStencil(TrackedDepthStencil))return false;DWORD enabled=FALSE,writeMask=0;if(!ReadTrackedRenderState(device,D3DRS_STENCILENABLE,enabled)||!ReadTrackedRenderState(device,D3DRS_STENCILWRITEMASK,writeMask))return true;if(!enabled||writeMask==0)return false;DWORD fail=D3DSTENCILOP_KEEP,zfail=D3DSTENCILOP_KEEP,pass=D3DSTENCILOP_KEEP;if(!ReadTrackedRenderState(device,D3DRS_STENCILFAIL,fail)||!ReadTrackedRenderState(device,D3DRS_STENCILZFAIL,zfail)||!ReadTrackedRenderState(device,D3DRS_STENCILPASS,pass))return true;if(fail!=D3DSTENCILOP_KEEP||zfail!=D3DSTENCILOP_KEEP||pass!=D3DSTENCILOP_KEEP)return true;DWORD twoSided=FALSE;if(!ReadTrackedRenderState(device,D3DRS_TWOSIDEDSTENCILMODE,twoSided))return true;if(!twoSided)return false;DWORD cFail=D3DSTENCILOP_KEEP,cZFail=D3DSTENCILOP_KEEP,cPass=D3DSTENCILOP_KEEP;if(!ReadTrackedRenderState(device,D3DRS_CCW_STENCILFAIL,cFail)||!ReadTrackedRenderState(device,D3DRS_CCW_STENCILZFAIL,cZFail)||!ReadTrackedRenderState(device,D3DRS_CCW_STENCILPASS,cPass))return true;return cFail!=D3DSTENCILOP_KEEP||cZFail!=D3DSTENCILOP_KEEP||cPass!=D3DSTENCILOP_KEEP;
		}
		void InvalidateRightDepthStencilIfLeftMayWrite(IDirect3DDevice9* device) { if(RightDepthSynchronized&&LeftDrawMayWriteDepth(device))RightDepthSynchronized=false;if(RightStencilSynchronized&&LeftDrawMayWriteStencil(device))RightStencilSynchronized=false; }
		bool ViewportCoversStereoBackbuffer(IDirect3DDevice9* device) { if(!device||!BackBufferDesc.Width||!BackBufferDesc.Height)return false;D3DVIEWPORT9 viewport{};if(FAILED(device->GetViewport(&viewport)))return false;return viewport.X==0&&viewport.Y==0&&viewport.Width==BackBufferDesc.Width&&viewport.Height==BackBufferDesc.Height; }
		bool ClearCoversStereoBackbuffer(IDirect3DDevice9* device,DWORD count,const D3DRECT* rects)
		{
			if(!ViewportCoversStereoBackbuffer(device))return false;
			if(count==0)return true;
			if(!rects)return false;
			for(DWORD i=0;i<count;++i)
			{
				const auto&r=rects[i];
				if(r.x1<=0&&r.y1<=0&&r.x2>=static_cast<LONG>(BackBufferDesc.Width)&&r.y2>=static_cast<LONG>(BackBufferDesc.Height))return true;
			}
			return false;
		}
		bool DepthDescMatches(const D3DSURFACE_DESC&a,const D3DSURFACE_DESC&b)
		{
			return a.Width==b.Width&&a.Height==b.Height&&a.Format==b.Format&&a.MultiSampleType==b.MultiSampleType&&a.MultiSampleQuality==b.MultiSampleQuality;
		}
		void RecordFullMainDepthClear(IDirect3DDevice9*device,DWORD count,const D3DRECT*rects,DWORD flags,float z,DWORD stencil)
		{
			const DWORD depthFlags=flags&(D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL);
			if(!depthFlags||!TargetIsBackBuffer()||!TrackedDepthStencil||!ClearCoversStereoBackbuffer(device,count,rects))return;
			D3DSURFACE_DESC desc{};if(FAILED(TrackedDepthStencil->GetDesc(&desc)))return;
			LastMainDepthClearEpoch=PresentEpoch;LastMainDepthClearFlags=depthFlags;LastMainDepthClearZ=z;LastMainDepthClearStencil=stencil;LastMainDepthClearDesc=desc;++FullMainDepthClears;
			if(!FirstMainClearLogged){FirstMainClearLogged=true;spdlog::info("VR stereo: full main depth clear observed count={} flags=0x{:x} z={} stencil={} epoch={}",count,static_cast<unsigned>(depthFlags),z,stencil,PresentEpoch);}
		}

		void ReleaseStereoResources()
		{
			StereoResourcesReady=false;DirectTransportResourcesReady=false;
			for(auto&slot:DirectTransportSlots){ReleaseCom(slot.fence);ReleaseCom(slot.leftSurface);ReleaseCom(slot.leftTexture);ReleaseCom(slot.rightSurface);ReleaseCom(slot.rightTexture);slot={};}
			ReleaseCom(DirectInteropProbeFence);ReleaseCom(DirectInteropProbeSurface);ReleaseCom(DirectInteropProbeTexture);DirectInteropProbeHandle=nullptr;DirectInteropProbeToken=0;DirectInteropVerified=false;
			if(SharedState&&SharedState->magic==OutRunVR::SharedMagic){InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientInteropProbeHandle),0);InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientInteropProbeToken),0);}
			DirectTransportFormat=D3DFMT_UNKNOWN;ProjectionInverseValid=false;ReleaseCom(RightEyeSurface);ReleaseCom(RightEyeDepth);ReleaseCom(LeftResolveSurface);ReleaseCom(LeftResolveTexture);ReleaseCom(RightResolveSurface);ReleaseCom(RightResolveTexture);ReleaseCom(BackBuffer);ReleaseCom(TrackedRenderTarget);ReleaseCom(TrackedDepthStencil);RightDepthSynchronized=true;RightStencilSynchronized=true;BackBufferDesc={};
		}
		bool CreateRightDepthForTracked(IDirect3DDevice9* device)
		{
			ReleaseCom(RightEyeDepth);RightDepthSynchronized=TrackedDepthStencil==nullptr;RightStencilSynchronized=true;if(!TrackedDepthStencil)return true;D3DSURFACE_DESC depthDesc{};if(FAILED(TrackedDepthStencil->GetDesc(&depthDesc)))return false;RightStencilSynchronized=!FormatHasStencil(depthDesc.Format);return SUCCEEDED(device->CreateDepthStencilSurface(BackBufferDesc.Width,BackBufferDesc.Height,depthDesc.Format,depthDesc.MultiSampleType,depthDesc.MultiSampleQuality,FALSE,&RightEyeDepth,nullptr));
		}
		bool InitializeAuxRenderTargetState(IDirect3DDevice9* device){for(DWORD index=1;index<=3;++index){IDirect3DSurface9*surface=nullptr;const HRESULT hr=device->GetRenderTarget(index,&surface);AuxRenderTargetActive[index-1]=SUCCEEDED(hr)&&surface!=nullptr;if(surface)surface->Release();}return true;}
		D3DFORMAT ChooseDirectTransportFormat(D3DFORMAT source){switch(source){case D3DFMT_A2B10G10R10:return D3DFMT_A2B10G10R10;case D3DFMT_A16B16G16R16F:return D3DFMT_A16B16G16R16F;case D3DFMT_A8R8G8B8:return D3DFMT_A8R8G8B8;default:return D3DFMT_A8R8G8B8;}}
		bool CanConvertForDirectTransport(IDirect3DDevice9* device,D3DFORMAT source,D3DFORMAT target){if(!device)return false;if(source==target)return true;D3DDEVICE_CREATION_PARAMETERS cp{};if(FAILED(device->GetCreationParameters(&cp)))return false;IDirect3D9*d3d=nullptr;if(FAILED(device->GetDirect3D(&d3d))||!d3d)return false;const HRESULT hr=d3d->CheckDeviceFormatConversion(cp.AdapterOrdinal,cp.DeviceType,source,target);d3d->Release();return SUCCEEDED(hr);}
		bool FrameIdAtOrAfter(std::uint32_t candidate,std::uint32_t reference){return reference==0||static_cast<std::int32_t>(candidate-reference)>=0;}
		bool WaitForEventQuery(IDirect3DQuery9* query,ULONGLONG timeoutMs){if(!query)return false;const ULONGLONG deadline=GetTickCount64()+timeoutMs;for(;;){const HRESULT ready=query->GetData(nullptr,0,D3DGETDATA_FLUSH);if(ready==S_OK)return true;if(ready!=S_FALSE)return false;if(GetTickCount64()>=deadline)return false;Sleep(0);}}

		bool QueryGameAdapterLuid(IDirect3DDevice9* device,LUID& luid)
		{
			IDirect3DDevice9Ex*deviceEx=nullptr;if(!device||FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),reinterpret_cast<void**>(&deviceEx)))||!deviceEx){if(!FirstD3D9ExUnavailableLogged){FirstD3D9ExUnavailableLogged=true;spdlog::warn("VR direct transport: game device is not D3D9Ex; zero-copy disabled, SBS/Desktop Duplication remains active");}return false;}D3DDEVICE_CREATION_PARAMETERS cp{};IDirect3D9*base=nullptr;IDirect3D9Ex*d3dEx=nullptr;const bool ok=SUCCEEDED(deviceEx->GetCreationParameters(&cp))&&SUCCEEDED(deviceEx->GetDirect3D(&base))&&base&&SUCCEEDED(base->QueryInterface(__uuidof(IDirect3D9Ex),reinterpret_cast<void**>(&d3dEx)))&&d3dEx&&SUCCEEDED(d3dEx->GetAdapterLUID(cp.AdapterOrdinal,&luid));ReleaseCom(d3dEx);ReleaseCom(base);ReleaseCom(deviceEx);return ok;
		}
		bool EnsureDirectInteropProbe(IDirect3DDevice9* device)
		{
			if(!HostDirectTransportSupported()||!EnsureSharedState()||(SharedState->flags&OutRunVR::HostAdapterLuidValid)==0)return false;LUID gameLuid{};if(!QueryGameAdapterLuid(device,gameLuid))return false;InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientAdapterLuidLow),static_cast<LONG>(gameLuid.LowPart));InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientAdapterLuidHigh),static_cast<LONG>(gameLuid.HighPart));const bool adapterMatches=SharedState->hostAdapterLuidLow==gameLuid.LowPart&&SharedState->hostAdapterLuidHigh==static_cast<std::uint32_t>(gameLuid.HighPart);if(!adapterMatches){if(!FirstAdapterMismatchLogged){FirstAdapterMismatchLogged=true;spdlog::warn("VR direct transport: OpenXR D3D11 adapter LUID does not match the game D3D9Ex adapter; forcing SBS fallback");}return false;}if(DirectInteropProbeToken&&SharedState->hostInteropProbeAckToken==DirectInteropProbeToken){DirectInteropVerified=true;if(!FirstInteropVerifiedLogged){FirstInteropVerifiedLogged=true;spdlog::info("VR direct transport: D3D9Ex -> D3D11 shared-resource probe verified on the OpenXR adapter");}return true;}if(!DirectInteropProbeTexture){HANDLE handle=nullptr;if(FAILED(device->CreateTexture(1,1,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&DirectInteropProbeTexture,&handle))||!DirectInteropProbeTexture||!handle||FAILED(DirectInteropProbeTexture->GetSurfaceLevel(0,&DirectInteropProbeSurface))||FAILED(device->ColorFill(DirectInteropProbeSurface,nullptr,D3DCOLOR_ARGB(0xFF,0x7B,0x7B,0x7B)))||FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT,&DirectInteropProbeFence))||!DirectInteropProbeFence||FAILED(DirectInteropProbeFence->Issue(D3DISSUE_END))||!WaitForEventQuery(DirectInteropProbeFence,50)){++DirectInteropProbeFailures;ReleaseCom(DirectInteropProbeFence);ReleaseCom(DirectInteropProbeSurface);ReleaseCom(DirectInteropProbeTexture);DirectInteropProbeHandle=nullptr;return false;}DirectInteropProbeHandle=handle;DirectInteropProbeToken=static_cast<std::uint32_t>(GetTickCount())^GetCurrentProcessId()^0x4F525652u;if(!DirectInteropProbeToken)DirectInteropProbeToken=1;InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->hostInteropProbeAckToken),0);InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientInteropProbeHandle),static_cast<LONG>(reinterpret_cast<std::uintptr_t>(DirectInteropProbeHandle)));InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientInteropProbeToken),static_cast<LONG>(DirectInteropProbeToken));}return false;
		}
		void ReleaseDirectTransportSlots(){DirectTransportResourcesReady=false;DirectTransportWidth=0;DirectTransportHeight=0;for(auto&slot:DirectTransportSlots){ReleaseCom(slot.fence);ReleaseCom(slot.leftSurface);ReleaseCom(slot.leftTexture);ReleaseCom(slot.rightSurface);ReleaseCom(slot.rightTexture);slot={};}}
		void DirectTransportTargetSize(IDirect3DDevice9* device,UINT& width,UINT& height)
		{
			// Once a DirectGPU eye size is active, keep it as the fallback for an
			// unstable seqlock read. Falling back to the desktop backbuffer for one
			// read used to recreate the whole 4-slot ring 2124x2284 <-> 3440x1440.
			width=DirectTransportWidth?DirectTransportWidth:BackBufferDesc.Width;
			height=DirectTransportHeight?DirectTransportHeight:BackBufferDesc.Height;
			if(SharedState&&SharedState->magic==OutRunVR::SharedMagic&&SharedState->protocolVersion==OutRunVR::SharedProtocolVersion&&SharedState->structSize==sizeof(OutRunVR::SharedPoseState))
			{
				for(int attempt=0;attempt<4;++attempt)
				{
					const std::uint32_t before=SharedState->sequence;if(before&1u)continue;MemoryBarrier();
					const UINT w0=SharedState->recommendedWidth[0],w1=SharedState->recommendedWidth[1];
					const UINT h0=SharedState->recommendedHeight[0],h1=SharedState->recommendedHeight[1];MemoryBarrier();
					const std::uint32_t after=SharedState->sequence;if(before!=after||(after&1u))continue;
					const UINT recommendedW=w0>w1?w0:w1;const UINT recommendedH=h0>h1?h0:h1;
					if(recommendedW&&recommendedH){width=recommendedW;height=recommendedH;}
					break;
				}
			}
			D3DCAPS9 caps{};if(device&&SUCCEEDED(device->GetDeviceCaps(&caps))){if(caps.MaxTextureWidth&&width>caps.MaxTextureWidth)width=caps.MaxTextureWidth;if(caps.MaxTextureHeight&&height>caps.MaxTextureHeight)height=caps.MaxTextureHeight;}
			if(!width)width=BackBufferDesc.Width?BackBufferDesc.Width:1u;if(!height)height=BackBufferDesc.Height?BackBufferDesc.Height:1u;
		}
		HRESULT StretchDirectEye(IDirect3DDevice9* device,IDirect3DSurface9* source,IDirect3DSurface9* target)
		{
			if(!device||!source||!target)return D3DERR_INVALIDCALL;HRESULT hr=device->StretchRect(source,nullptr,target,nullptr,D3DTEXF_LINEAR);if(FAILED(hr))hr=device->StretchRect(source,nullptr,target,nullptr,D3DTEXF_NONE);return hr;
		}
		bool EnsureDirectTransportResources(IDirect3DDevice9* device)
		{
			if(!device||!BackBufferDesc.Width||!BackBufferDesc.Height||!EnsureRenderFrameState()||!EnsureDirectInteropProbe(device))return false;UINT targetWidth=0,targetHeight=0;DirectTransportTargetSize(device,targetWidth,targetHeight);if(DirectTransportResourcesReady&&DirectTransportWidth==targetWidth&&DirectTransportHeight==targetHeight)return true;if(DirectTransportResourcesReady)ReleaseDirectTransportSlots();DirectTransportFormat=ChooseDirectTransportFormat(BackBufferDesc.Format);if(!CanConvertForDirectTransport(device,BackBufferDesc.Format,DirectTransportFormat))return false;for(auto&slot:DirectTransportSlots){HANDLE leftHandle=nullptr,rightHandle=nullptr;const HRESULT leftHr=device->CreateTexture(targetWidth,targetHeight,1,D3DUSAGE_RENDERTARGET,DirectTransportFormat,D3DPOOL_DEFAULT,&slot.leftTexture,&leftHandle);const HRESULT rightHr=SUCCEEDED(leftHr)?device->CreateTexture(targetWidth,targetHeight,1,D3DUSAGE_RENDERTARGET,DirectTransportFormat,D3DPOOL_DEFAULT,&slot.rightTexture,&rightHandle):E_FAIL;if(FAILED(leftHr)||FAILED(rightHr)||!leftHandle||!rightHandle||FAILED(slot.leftTexture->GetSurfaceLevel(0,&slot.leftSurface))||FAILED(slot.rightTexture->GetSurfaceLevel(0,&slot.rightSurface))||FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT,&slot.fence))||!slot.fence){ReleaseDirectTransportSlots();return false;}slot.leftHandle=leftHandle;slot.rightHandle=rightHandle;}DirectTransportWidth=targetWidth;DirectTransportHeight=targetHeight;if(!DirectTransportGeneration)DirectTransportGeneration=RenderFrameRunGeneration;if(++DirectTransportGeneration==0)++DirectTransportGeneration;DirectTransportResourcesReady=true;spdlog::info("VR stereo: verified 4-slot D3D9Ex shared eye ring ready {}x{} from desktop {}x{} (VDXR/OpenXR recommended) format={} generation={}",DirectTransportWidth,DirectTransportHeight,BackBufferDesc.Width,BackBufferDesc.Height,static_cast<int>(DirectTransportFormat),DirectTransportGeneration);return true;
		}
		bool ResolveDirectTransport(IDirect3DDevice9* device,std::uint32_t frameId)
		{
			if(!frameId||!EnsureDirectTransportResources(device)||!BackBuffer||!RightEyeSurface)return false;
			const std::uint32_t consumed=SharedState?SharedState->hostDirectConsumedFrameId:0;
			const std::uint32_t preferred=(frameId-1u)%OutRunVR::RenderFrameRingSize;
			std::uint32_t selected=OutRunVR::RenderFrameRingSize;

			// Search every ring slot. A slow query must not stall the game thread
			// or block an otherwise free slot.
			for(std::uint32_t offset=0;offset<OutRunVR::RenderFrameRingSize;++offset)
			{
				const std::uint32_t index=(preferred+offset)%OutRunVR::RenderFrameRingSize;
				auto&slot=DirectTransportSlots[index];

				if(slot.producerPending)
				{
					const HRESULT ready=slot.fence?slot.fence->GetData(nullptr,0,0):E_FAIL;
					if(ready==S_OK)
					{
						slot.producerPending=false;
						slot.pendingFrameId=0;
						// A frame whose fence missed its publication window was
						// never visible to the host, so reclaim it without ACK.
						if(!slot.published)slot.frameId=0;
					}
					else if(ready==S_FALSE)
					{
						continue;
					}
					else
					{
						slot.producerPending=false;
						slot.pendingFrameId=0;
						slot.frameId=0;
						slot.published=false;
					}
				}

				// Published images are immutable until the host ACKs their frame.
				if(slot.published&&slot.frameId&&!FrameIdAtOrAfter(consumed,slot.frameId))
					continue;
				if(slot.published&&slot.frameId&&FrameIdAtOrAfter(consumed,slot.frameId))
				{
					slot.frameId=0;
					slot.published=false;
				}
				selected=index;
				break;
			}

			if(selected>=OutRunVR::RenderFrameRingSize)
			{
				++DirectTransportRingBackpressure;
				return false;
			}

			auto&slot=DirectTransportSlots[selected];
			{
				InternalPassScope guard;
				if(FAILED(StretchDirectEye(device,BackBuffer,slot.leftSurface))||
					FAILED(StretchDirectEye(device,RightEyeSurface,slot.rightSurface))||
					FAILED(slot.fence->Issue(D3DISSUE_END)))return false;
			}
			slot.producerPending=true;
			slot.pendingFrameId=frameId;
			slot.frameId=frameId;
			slot.published=false;
			ActiveDirectTransportSlot=selected;
			return true;
		}

		bool DirectTransportFrameReadyAfterPresent(std::uint32_t frameId)
		{
			if(!frameId||ActiveDirectTransportSlot>=DirectTransportSlots.size())return false;
			auto&slot=DirectTransportSlots[ActiveDirectTransportSlot];
			if(slot.frameId!=frameId||slot.pendingFrameId!=frameId||!slot.fence)return false;

			// Present has already flushed the frame. Allow at most 1 ms of bounded
			// completion grace here; the old 4 ms pre-Present wait consumed a large
			// part of a 90/120 Hz frame budget and was visible as VD hitching.
			if(WaitForEventQuery(slot.fence,1))
			{
				slot.producerPending=false;
				slot.pendingFrameId=0;
				slot.published=true;
				return true;
			}

			++DirectTransportFenceTimeouts;
			const HRESULT status=slot.fence->GetData(nullptr,0,0);
			if(status!=S_FALSE)
			{
				slot.producerPending=false;
				slot.pendingFrameId=0;
				slot.frameId=0;
				slot.published=false;
			}
			// S_FALSE stays quarantined. A later slot scan reclaims it as soon as
			// the query completes; it is never published and therefore needs no ACK.
			return false;
		}

		bool EnsureStereoResources(IDirect3DDevice9* device)
		{
			if(!device)return false;if(StereoResourcesReady&&BackBuffer&&RightEyeSurface&&LeftResolveSurface&&RightResolveSurface)return true;IDirect3DSurface9*newBackBuffer=nullptr;if(FAILED(device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&newBackBuffer))||!newBackBuffer)return false;D3DSURFACE_DESC desc{};if(FAILED(newBackBuffer->GetDesc(&desc))){newBackBuffer->Release();return false;}ReleaseStereoResources();BackBuffer=newBackBuffer;BackBufferDesc=desc;IDirect3DSurface9*currentRt=nullptr;if(SUCCEEDED(device->GetRenderTarget(0,&currentRt))&&currentRt)TrackedRenderTarget=currentRt;else ReplaceSurfaceRef(TrackedRenderTarget,BackBuffer);IDirect3DSurface9*currentDepth=nullptr;const HRESULT depthStateHr=device->GetDepthStencilSurface(&currentDepth);if(SUCCEEDED(depthStateHr)&&currentDepth)TrackedDepthStencil=currentDepth;else if(depthStateHr==D3DERR_NOTFOUND)TrackedDepthStencil=nullptr;InitializeAuxRenderTargetState(device);if(FAILED(device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,desc.MultiSampleType,desc.MultiSampleQuality,FALSE,&RightEyeSurface,nullptr))||!CreateRightDepthForTracked(device)){ReleaseStereoResources();return false;}if(FAILED(device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,desc.Format,D3DPOOL_DEFAULT,&LeftResolveTexture,nullptr))||FAILED(LeftResolveTexture->GetSurfaceLevel(0,&LeftResolveSurface))||FAILED(device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,desc.Format,D3DPOOL_DEFAULT,&RightResolveTexture,nullptr))||FAILED(RightResolveTexture->GetSurfaceLevel(0,&RightResolveSurface))){ReleaseStereoResources();return false;}D3DVIEWPORT9 savedViewport{};if(FAILED(device->GetViewport(&savedViewport))){ReleaseStereoResources();return false;}bool initOk=true;{InternalPassScope guard;if(FAILED(SetRenderTargetHook.stdcall<HRESULT>(device,0u,RightEyeSurface)))initOk=false;if(initOk){const HRESULT depthHr=SetDepthStencilSurfaceHook?SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,RightEyeDepth):device->SetDepthStencilSurface(RightEyeDepth);if(FAILED(depthHr))initOk=false;}if(initOk&&FAILED(device->SetViewport(&savedViewport)))initOk=false;DWORD clearFlags=D3DCLEAR_TARGET;if(RightEyeDepth){D3DSURFACE_DESC depthDesc{};if(FAILED(RightEyeDepth->GetDesc(&depthDesc)))initOk=false;else{clearFlags|=D3DCLEAR_ZBUFFER;if(FormatHasStencil(depthDesc.Format))clearFlags|=D3DCLEAR_STENCIL;}}if(initOk&&FAILED(ClearHook.stdcall<HRESULT>(device,0u,static_cast<const D3DRECT*>(nullptr),clearFlags,D3DCOLOR_XRGB(0,0,0),1.0f,0u)))initOk=false;if(TrackedRenderTarget&&FAILED(SetRenderTargetHook.stdcall<HRESULT>(device,0u,TrackedRenderTarget)))initOk=false;const HRESULT restoreDepthHr=SetDepthStencilSurfaceHook?SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,TrackedDepthStencil):device->SetDepthStencilSurface(TrackedDepthStencil);if(FAILED(restoreDepthHr))initOk=false;if(FAILED(device->SetViewport(&savedViewport)))initOk=false;}if(!initOk){ReleaseStereoResources();return false;}StereoResourcesReady=true;spdlog::info("VR stereo: full-size eye surfaces ready {}x{} format={} msaa={} quality={}",desc.Width,desc.Height,static_cast<int>(desc.Format),static_cast<int>(desc.MultiSampleType),desc.MultiSampleQuality);return true;
		}

		void NoteRestoreFailure(const char* what){FrameRightDrawFailed=true;PoisonFrame(OutRunVR::StereoFailureRestoreFailed);++RestoreFailures;if(!FirstRestoreFailureLogged){FirstRestoreFailureLogged=true;spdlog::warn("VR stereo: renderer state restore failed at {}; frame forced to mono fallback",what);}}
		bool RestoreRightPassState(IDirect3DDevice9* device,IDirect3DSurface9* savedRt,IDirect3DSurface9* savedDepth,const D3DVIEWPORT9& savedViewport,const float* originalConstants,bool restoreWvp){bool ok=true;if(savedRt&&FAILED(SetRenderTargetHook.stdcall<HRESULT>(device,0u,savedRt)))ok=false;const HRESULT depthHr=SetDepthStencilSurfaceHook?SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,savedDepth):device->SetDepthStencilSurface(savedDepth);if(FAILED(depthHr))ok=false;if(FAILED(device->SetViewport(&savedViewport)))ok=false;if(restoreWvp&&!SetWvpOneRegisterAtATime(device,originalConstants))ok=false;return ok;}
		bool TryBootstrapRightDepthFromRecentClear(IDirect3DDevice9*device)
		{
			++DepthBootstrapAttempts;
			if(!device||!RightEyeSurface||!RightEyeDepth||!TrackedDepthStencil||LastMainDepthClearEpoch!=PresentEpoch)return false;
			D3DSURFACE_DESC leftDesc{},rightDesc{};if(FAILED(TrackedDepthStencil->GetDesc(&leftDesc))||FAILED(RightEyeDepth->GetDesc(&rightDesc)))return false;
			if(!DepthDescMatches(leftDesc,rightDesc)||!DepthDescMatches(leftDesc,LastMainDepthClearDesc))return false;
			DWORD replayFlags=0;if(!RightDepthSynchronized&&(LastMainDepthClearFlags&D3DCLEAR_ZBUFFER))replayFlags|=D3DCLEAR_ZBUFFER;if(!RightStencilSynchronized&&(LastMainDepthClearFlags&D3DCLEAR_STENCIL)&&FormatHasStencil(leftDesc.Format))replayFlags|=D3DCLEAR_STENCIL;if(!replayFlags)return false;
			D3DVIEWPORT9 savedViewport{};if(FAILED(device->GetViewport(&savedViewport)))return false;IDirect3DSurface9*savedRt=TrackedRenderTarget,*savedDepth=TrackedDepthStencil;HRESULT clearHr=E_FAIL;bool restoreOk=true;{InternalPassScope guard;if(SUCCEEDED(SetRenderTargetHook.stdcall<HRESULT>(device,0u,RightEyeSurface))&&SUCCEEDED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,RightEyeDepth))&&SUCCEEDED(device->SetViewport(&savedViewport)))clearHr=ClearHook.stdcall<HRESULT>(device,0u,static_cast<const D3DRECT*>(nullptr),replayFlags,0,LastMainDepthClearZ,LastMainDepthClearStencil);restoreOk=RestoreRightPassState(device,savedRt,savedDepth,savedViewport,nullptr,false);}if(FAILED(clearHr)||!restoreOk)return false;
			if(replayFlags&D3DCLEAR_ZBUFFER)RightDepthSynchronized=true;if(replayFlags&D3DCLEAR_STENCIL)RightStencilSynchronized=true;++DepthBootstrapSuccesses;if(!FirstDepthBootstrapLogged){FirstDepthBootstrapLogged=true;spdlog::info("VR stereo: bootstrapped right-eye depth from same-Present full main clear flags=0x{:x} epoch={}",static_cast<unsigned>(replayFlags),PresentEpoch);}return true;
		}
		std::uint32_t NextStereoFrameId(){if(++StereoFrameCounter==0)++StereoFrameCounter;return StereoFrameCounter;}

		void PublishStereoState(std::uint32_t state,bool worldStereo,std::uint32_t poseSequence,std::uint32_t frameId)
		{
			if(!EnsureSharedState())return;InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientStereoFrameIndex]),0);const LONG stereoBits=static_cast<LONG>(OutRunVR::ClientStereoActive|OutRunVR::ClientStereoWorldDraw|OutRunVR::ClientStereoDrawDuplicated);InterlockedAnd(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientFlagsIndex]),~stereoBits);InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientStereoBackbufferWidthIndex]),static_cast<LONG>(BackBufferDesc.Width));InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientStereoBackbufferHeightIndex]),static_cast<LONG>(BackBufferDesc.Height));if(state!=OutRunVR::StereoSbsActive||poseSequence==0||frameId==0){InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientStereoPoseSequence),0);InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientStereoStateIndex]),static_cast<LONG>(state));return;}InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientStereoPoseSequence),static_cast<LONG>(poseSequence));LONG bits=static_cast<LONG>(OutRunVR::ClientStereoActive|OutRunVR::ClientStereoDrawDuplicated);if(worldStereo)bits|=static_cast<LONG>(OutRunVR::ClientStereoWorldDraw);InterlockedOr(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientFlagsIndex]),bits);InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientStereoStateIndex]),static_cast<LONG>(OutRunVR::StereoSbsActive));MemoryBarrier();InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[OutRunVR::ClientStereoFrameIndex]),static_cast<LONG>(frameId));
		}

		void WriteGameBuildTag(OutRunVR::SharedRenderFrameState& frame)
		{
			char tag[12]{};
			const char* source = OUTRUN_VR_BUILD_SHA;
			for (std::size_t i = 0; i < sizeof(tag) && source[i]; ++i)
				tag[i] = source[i];
			std::memcpy(
				&frame.reserved[OutRunVR::RenderFrameGameBuildTag0Index],
				tag, sizeof(tag));
		}

		void PublishRenderFrame(std::uint32_t state,std::uint32_t frameId,std::uint32_t sourcePoseSequence,std::int64_t presentQpc,OutRunVR::StereoFailureReason failureReason,const OutRunVRRenderer::LatchedStereoFrame* stereo,bool presentInFlight=false,bool directTransport=false)
		{
			if(!EnsureRenderFrameState())return;const std::uint32_t slotIndex=frameId?(frameId-1u)%OutRunVR::RenderFrameRingSize:(RenderFrameRing->latestSlot+1u)%OutRunVR::RenderFrameRingSize;auto&frame=RenderFrameRing->slots[slotIndex];LONG seq=InterlockedIncrement(reinterpret_cast<volatile LONG*>(&frame.sequence));if((seq&1)==0)InterlockedIncrement(reinterpret_cast<volatile LONG*>(&frame.sequence));MemoryBarrier();frame.magic=OutRunVR::RenderFrameMagic;frame.protocolVersion=OutRunVR::RenderFrameProtocolVersion;frame.structSize=sizeof(frame);frame.clientPid=GetCurrentProcessId();frame.state=state;frame.frameId=frameId;frame.sourcePoseSequence=sourcePoseSequence;frame.presentationMode=GameplayActive()?OutRunVR::PresentationGameplay:OutRunVR::PresentationTheater;frame.flags=presentInFlight?OutRunVR::RenderFramePresentInFlight:0;frame.failureReason=static_cast<std::uint32_t>(failureReason);const bool directSizedFrame=directTransport&&DirectTransportResourcesReady&&DirectTransportWidth&&DirectTransportHeight;frame.backbufferWidth=directSizedFrame?DirectTransportWidth:BackBufferDesc.Width;frame.backbufferHeight=directSizedFrame?DirectTransportHeight:BackBufferDesc.Height;frame.presentQpc=presentQpc;std::memset(frame.eye,0,sizeof(frame.eye));std::memset(frame.reserved,0,sizeof(frame.reserved));frame.reserved[OutRunVR::RenderFrameRunGenerationIndex]=RenderFrameRunGeneration;WriteGameBuildTag(frame);frame.reserved[OutRunVR::RenderFrameCadenceRequestIndex]=OutRunVRRenderer::GetActiveCadenceRequestId();if(directTransport&&DirectTransportResourcesReady&&ActiveDirectTransportSlot<DirectTransportSlots.size()){const auto&transport=DirectTransportSlots[ActiveDirectTransportSlot];if(transport.leftHandle&&transport.rightHandle&&transport.frameId==frameId){frame.flags|=OutRunVR::RenderFrameDirectGpuTransport;frame.reserved[OutRunVR::RenderFrameDirectLeftHandleIndex]=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(transport.leftHandle));frame.reserved[OutRunVR::RenderFrameDirectRightHandleIndex]=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(transport.rightHandle));frame.reserved[OutRunVR::RenderFrameDirectWidthIndex]=DirectTransportWidth;frame.reserved[OutRunVR::RenderFrameDirectHeightIndex]=DirectTransportHeight;frame.reserved[OutRunVR::RenderFrameDirectFormatIndex]=static_cast<std::uint32_t>(DirectTransportFormat);frame.reserved[OutRunVR::RenderFrameDirectGenerationIndex]=DirectTransportGeneration;frame.reserved[OutRunVR::RenderFrameDirectSlotIndex]=ActiveDirectTransportSlot;}}if(!presentInFlight&&state==OutRunVR::StereoSbsActive&&stereo&&stereo->valid&&frameId){const std::uint32_t transportFlag=frame.flags&OutRunVR::RenderFrameDirectGpuTransport;frame.flags=transportFlag|OutRunVR::RenderFrameStereoComplete|OutRunVR::RenderFrameWorldStereo|OutRunVR::RenderFrameDrawDuplicated|OutRunVR::RenderFrameEffectivePoseValid;for(int eye=0;eye<2;++eye){std::memcpy(frame.eye[eye].orientation,stereo->effectiveEyeOrientation[eye],sizeof(frame.eye[eye].orientation));std::memcpy(frame.eye[eye].position,stereo->effectiveEyePosition[eye],sizeof(frame.eye[eye].position));frame.eye[eye].fov=stereo->eyeFov[eye];}}MemoryBarrier();seq=InterlockedIncrement(reinterpret_cast<volatile LONG*>(&frame.sequence));if(seq&1)InterlockedIncrement(reinterpret_cast<volatile LONG*>(&frame.sequence));LONG ringSeq=InterlockedIncrement(reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));if((ringSeq&1)==0)InterlockedIncrement(reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));MemoryBarrier();RenderFrameRing->clientPid=GetCurrentProcessId();RenderFrameRing->latestSlot=slotIndex;MemoryBarrier();ringSeq=InterlockedIncrement(reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));if(ringSeq&1)InterlockedIncrement(reinterpret_cast<volatile LONG*>(&RenderFrameRing->publishSequence));
		}

		struct ScreenVertex{float x,y,z,rhw;float u,v;};
		bool DrawTextureHalf(IDirect3DDevice9* device,IDirect3DTexture9* texture,bool right){if(!texture||!BackBufferDesc.Width||!BackBufferDesc.Height)return false;const float x0=right?static_cast<float>(BackBufferDesc.Width/2):0.0f;const float x1=right?static_cast<float>(BackBufferDesc.Width):static_cast<float>(BackBufferDesc.Width/2);const float y0=0.0f,y1=static_cast<float>(BackBufferDesc.Height);const ScreenVertex vertices[4]={{x0-0.5f,y0-0.5f,0.0f,1.0f,0.0f,0.0f},{x1-0.5f,y0-0.5f,0.0f,1.0f,1.0f,0.0f},{x0-0.5f,y1-0.5f,0.0f,1.0f,0.0f,1.0f},{x1-0.5f,y1-0.5f,0.0f,1.0f,1.0f,1.0f}};if(FAILED(device->SetTexture(0,texture)))return false;return SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,static_cast<UINT>(sizeof(ScreenVertex))));}
		bool ComposeSbs(IDirect3DDevice9* device)
		{
			if(!StereoResourcesReady||!BackBuffer||!RightEyeSurface||!LeftResolveSurface||!RightResolveSurface)return false;
			if(FAILED(device->StretchRect(BackBuffer,nullptr,LeftResolveSurface,nullptr,D3DTEXF_NONE))||FAILED(device->StretchRect(RightEyeSurface,nullptr,RightResolveSurface,nullptr,D3DTEXF_NONE)))return false;
			IDirect3DStateBlock9*stateBlock=nullptr;
			if(FAILED(device->CreateStateBlock(D3DSBT_ALL,&stateBlock))||!stateBlock)return false;
			IDirect3DSurface9*savedRt=nullptr,*savedDepth=nullptr;D3DVIEWPORT9 savedViewport{};
			const HRESULT rtHr=device->GetRenderTarget(0,&savedRt),depthHr=device->GetDepthStencilSurface(&savedDepth),viewportHr=device->GetViewport(&savedViewport);
			const bool depthOk=SUCCEEDED(depthHr)||depthHr==D3DERR_NOTFOUND;
			if(FAILED(rtHr)||!savedRt||!depthOk||FAILED(viewportHr)){if(savedRt)savedRt->Release();if(savedDepth)savedDepth->Release();stateBlock->Release();return false;}
			bool success=false,beganScene=false;
			{InternalPassScope guard;if(SUCCEEDED(SetRenderTargetHook.stdcall<HRESULT>(device,0u,BackBuffer))&&SUCCEEDED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,static_cast<IDirect3DSurface9*>(nullptr)))&&SUCCEEDED(device->BeginScene())){beganScene=true;device->SetVertexShader(nullptr);device->SetPixelShader(nullptr);device->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);device->SetRenderState(D3DRS_ZENABLE,FALSE);device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);device->SetRenderState(D3DRS_LIGHTING,FALSE);device->SetRenderState(D3DRS_FOGENABLE,FALSE);device->SetRenderState(D3DRS_STENCILENABLE,FALSE);device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0);device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);D3DVIEWPORT9 full{0,0,BackBufferDesc.Width,BackBufferDesc.Height,0.0f,1.0f};const bool stateOk=SUCCEEDED(device->SetViewport(&full))&&SUCCEEDED(device->Clear(0,nullptr,D3DCLEAR_TARGET,D3DCOLOR_XRGB(0,0,0),1.0f,0));const bool leftOk=stateOk&&DrawTextureHalf(device,LeftResolveTexture,false);const bool rightOk=leftOk&&DrawTextureHalf(device,RightResolveTexture,true);device->SetTexture(0,nullptr);const HRESULT endHr=device->EndScene();beganScene=false;success=leftOk&&rightOk&&SUCCEEDED(endHr);}if(beganScene)device->EndScene();}
			const HRESULT applyHr=stateBlock->Apply();stateBlock->Release();bool restoreOk=SUCCEEDED(applyHr);
			if(FAILED(SetRenderTargetHook.stdcall<HRESULT>(device,0u,savedRt)))restoreOk=false;
			if(FAILED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,savedDepth)))restoreOk=false;
			if(FAILED(device->SetViewport(&savedViewport)))restoreOk=false;
			savedRt->Release();if(savedDepth)savedDepth->Release();
			if(!restoreOk)NoteRestoreFailure("SBS composition state restore");
			return success&&restoreOk;
		}

		bool PrepareDuplicatedDraw(IDirect3DDevice9* device,DrawStereoState& draw)
		{
			if(InternalStereoPass||!StereoWanted()||!TargetIsBackBuffer())return false;if(OcclusionQueryTrackingUnavailable.load(std::memory_order_acquire)){PoisonFrame(OutRunVR::StereoFailureOcclusionQueryActive);++OcclusionStereoRejects;if(!FirstOcclusionTrackingUnavailableLogged){FirstOcclusionTrackingUnavailableLogged=true;spdlog::error("VR stereo: occlusion-query Issue tracking unavailable; stereo duplication fails closed to single-execution mono until tracking recovers");}return false;}if(AnyAuxRenderTargetActive()){PoisonFrame(OutRunVR::StereoFailureMrtActive);++MrtRejectedDraws;if(!FirstMrtRejectLogged){FirstMrtRejectLogged=true;spdlog::info("VR stereo: auxiliary MRT active; current Present is marked incomplete");}return false;}if(ActiveOcclusionQueries.load(std::memory_order_acquire)>0){++OcclusionStereoRejects;if(!FirstOcclusionRejectLogged){FirstOcclusionRejectLogged=true;spdlog::info("VR stereo: active D3D9 occlusion query kept single-eye; query result is shared without invalidating the stereo Present");}return false;}if(!EnsureStereoResources(device)){PoisonFrame(OutRunVR::StereoFailureResourceUnavailable);return false;}if(TrackedDepthStencil&&(!RightDepthSynchronized||!RightStencilSynchronized))TryBootstrapRightDepthFromRecentClear(device);if(TrackedDepthStencil&&!RightDepthSynchronized&&DepthTestActive(device)){PoisonFrame(OutRunVR::StereoFailureDepthUnsynchronized);return false;}if(TrackedDepthStencil&&!RightStencilSynchronized&&StencilTestActive(device)){PoisonFrame(OutRunVR::StereoFailureStencilUnsynchronized);return false;}OutRunVRRenderer::LatchedStereoFrame stereo{};if(!OutRunVRRenderer::GetLatchedStereoFrame(stereo)){PoisonFrame(OutRunVR::StereoFailureMissingLatchedPose);return false;}draw.stereoFrame=stereo;const EyeBuildResult classification=BuildEyeConstants(device,stereo,draw);if(classification==EyeBuildResult::FatalError){++WorldClassificationFailures;PoisonFrame(OutRunVR::StereoFailureWorldClassificationFailed);if(!FirstClassificationFailureLogged){FirstClassificationFailureLogged=true;spdlog::warn("VR stereo: verified world draw could not be classified/transformed safely; frame forced to mono fallback");}return false;}return true;
		}

		template <typename DrawCall>
		HRESULT ExecuteStereoDraw(IDirect3DDevice9* device,DrawCall&& drawCall)
		{
			if(!StereoBaseCallbacksArmed.load(std::memory_order_acquire)||!IsGameDevice(device))return drawCall();
			if(StereoWanted()&&!TargetIsBackBuffer())
			{
				if(CurrentDrawMatchesVerifiedWorld(device))
				{
					++OffscreenVerifiedWorldDraws;
					if(!FirstOffscreenWorldLogged)
					{
						FirstOffscreenWorldLogged=true;D3DSURFACE_DESC rtDesc{},depthDesc{};const bool rtOk=TrackedRenderTarget&&SUCCEEDED(TrackedRenderTarget->GetDesc(&rtDesc));const bool depthOk=TrackedDepthStencil&&SUCCEEDED(TrackedDepthStencil->GetDesc(&depthDesc));
						spdlog::info("VR stereo: verified auxiliary/offscreen world pass kept mono; stereo backbuffer remains eligible. rt={}x{} fmt={} msaa={} depthFmt={} depthMsaa={}",rtOk?rtDesc.Width:0u,rtOk?rtDesc.Height:0u,rtOk?static_cast<int>(rtDesc.Format):-1,rtOk?static_cast<int>(rtDesc.MultiSampleType):-1,depthOk?static_cast<int>(depthDesc.Format):-1,depthOk?static_cast<int>(depthDesc.MultiSampleType):-1);
					}
				}
				return drawCall();
			}

			const bool occlusionActive=ActiveOcclusionQueries.load(std::memory_order_acquire)>0||
				OcclusionQueryTrackingUnavailable.load(std::memory_order_acquire);
			DrawStereoState draw{};
			if(!PrepareDuplicatedDraw(device,draw))
			{
				if(StereoWanted()&&TargetIsBackBuffer()&&!occlusionActive)InvalidateRightDepthStencilIfLeftMayWrite(device);
				return drawCall();
			}
			D3DVIEWPORT9 savedViewport{};if(FAILED(device->GetViewport(&savedViewport))){InvalidateRightDepthStencilIfLeftMayWrite(device);PoisonFrame(OutRunVR::StereoFailureViewportUnavailable);return drawCall();}
			if(draw.worldStereo){bool leftEyeOk=false;{InternalPassScope guard;leftEyeOk=draw.fixedFunctionWorld?SetFixedFunctionEyeTransforms(device,draw,0):SetWvpOneRegisterAtATime(device,draw.eyeConstants[0]);}if(!leftEyeOk){bool rolledBack=false;{InternalPassScope guard;rolledBack=draw.fixedFunctionWorld?RestoreFixedFunctionTransforms(device,draw):SetWvpOneRegisterAtATime(device,draw.originalConstants);}InvalidateRightDepthStencilIfLeftMayWrite(device);PoisonFrame(OutRunVR::StereoFailureLeftWvpUploadFailed);if(!rolledBack)NoteRestoreFailure(draw.fixedFunctionWorld?"left-eye fixed-function rollback":"left-eye c64 rollback");return drawCall();}}
			const HRESULT leftHr=drawCall();if(FAILED(leftHr)){InvalidateRightDepthStencilIfLeftMayWrite(device);PoisonFrame(OutRunVR::StereoFailureLeftDrawFailed);if(draw.worldStereo){bool restored=false;{InternalPassScope guard;restored=draw.fixedFunctionWorld?RestoreFixedFunctionTransforms(device,draw):SetWvpOneRegisterAtATime(device,draw.originalConstants);}if(!restored)NoteRestoreFailure(draw.fixedFunctionWorld?"left draw fixed-function transforms":"left draw c64");}return leftHr;}
			IDirect3DSurface9*savedRt=TrackedRenderTarget,*savedDepth=TrackedDepthStencil;HRESULT rightHr=D3D_OK;OutRunVR::StereoFailureReason rightFailure=OutRunVR::StereoFailureRightStateFailed;bool restoreOk=true;{InternalPassScope guard;rightHr=SetRenderTargetHook.stdcall<HRESULT>(device,0u,RightEyeSurface);if(SUCCEEDED(rightHr))rightHr=SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,RightEyeDepth);if(SUCCEEDED(rightHr))rightHr=device->SetViewport(&savedViewport);if(SUCCEEDED(rightHr)&&draw.worldStereo){const bool eyeOk=draw.fixedFunctionWorld?SetFixedFunctionEyeTransforms(device,draw,1):SetWvpOneRegisterAtATime(device,draw.eyeConstants[1]);if(!eyeOk){rightFailure=OutRunVR::StereoFailureRightWvpUploadFailed;rightHr=E_FAIL;}}if(SUCCEEDED(rightHr)){rightFailure=OutRunVR::StereoFailureRightDrawFailed;rightHr=drawCall();}restoreOk=RestoreRightPassState(device,savedRt,savedDepth,savedViewport,draw.originalConstants,draw.worldStereo&&!draw.fixedFunctionWorld);if(draw.fixedFunctionWorld&&!RestoreFixedFunctionTransforms(device,draw))restoreOk=false;}
			FrameHadDuplicatedDraw=true;++DuplicatedDraws;if(draw.worldStereo){if(FrameStereoPoseSequence==0){FrameStereoPoseSequence=draw.poseSequence;FrameStereoMetadata=draw.stereoFrame;}else if(FrameStereoPoseSequence!=draw.poseSequence){FrameRightDrawFailed=true;PoisonFrame(OutRunVR::StereoFailurePoseSequenceMismatch);if(!FramePoseMismatchLogged){FramePoseMismatchLogged=true;++PoseSequenceMismatchFrames;spdlog::warn("VR stereo: multiple host pose sequences reached one Present; frame forced to mono fallback");}}FrameHadWorldStereo=true;++WorldStereoDraws;if(!FirstWorldStereoLogged){FirstWorldStereoLogged=true;spdlog::info("VR stereo: TRUE GEOMETRY STEREO active; per-eye c64 + OpenXR asymmetric FOV confirmed");}}else ++NonWorldDuplicatedDraws;if(FAILED(rightHr)){FrameRightDrawFailed=true;InvalidateRightDepthStencilIfLeftMayWrite(device);PoisonFrame(rightFailure);}if(!restoreOk){InvalidateRightDepthStencilIfLeftMayWrite(device);NoteRestoreFailure("right-eye draw");}return leftHr;
		}

		HRESULT __stdcall DrawPrimitiveDest(IDirect3DDevice9*device,D3DPRIMITIVETYPE type,UINT startVertex,UINT primitiveCount){auto call=[&](){return DrawPrimitiveHook.stdcall<HRESULT>(device,type,startVertex,primitiveCount);};return ExecuteStereoDraw(device,call);}
		HRESULT __stdcall DrawIndexedPrimitiveDest(IDirect3DDevice9*device,D3DPRIMITIVETYPE type,INT baseVertexIndex,UINT minVertexIndex,UINT numVertices,UINT startIndex,UINT primitiveCount){auto call=[&](){return DrawIndexedPrimitiveHook.stdcall<HRESULT>(device,type,baseVertexIndex,minVertexIndex,numVertices,startIndex,primitiveCount);};return ExecuteStereoDraw(device,call);}
		HRESULT __stdcall DrawPrimitiveUPDest(IDirect3DDevice9*device,D3DPRIMITIVETYPE type,UINT primitiveCount,const void*data,UINT stride){if(InternalStereoPass)return DrawPrimitiveUPHook.stdcall<HRESULT>(device,type,primitiveCount,data,stride);auto call=[&](){return DrawPrimitiveUPHook.stdcall<HRESULT>(device,type,primitiveCount,data,stride);};return ExecuteStereoDraw(device,call);}
		HRESULT __stdcall DrawIndexedPrimitiveUPDest(IDirect3DDevice9*device,D3DPRIMITIVETYPE type,UINT minVertexIndex,UINT numVertices,UINT primitiveCount,const void*indexData,D3DFORMAT indexFormat,const void*vertexData,UINT stride){auto call=[&](){return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(device,type,minVertexIndex,numVertices,primitiveCount,indexData,indexFormat,vertexData,stride);};return ExecuteStereoDraw(device,call);}

		HRESULT __stdcall ClearDest(IDirect3DDevice9*device,DWORD count,const D3DRECT*rects,DWORD flags,D3DCOLOR color,float z,DWORD stencil)
		{
			if(!StereoBaseCallbacksArmed.load(std::memory_order_acquire)||!IsGameDevice(device)||InternalStereoPass)return ClearHook.stdcall<HRESULT>(device,count,rects,flags,color,z,stencil);if(StereoWanted()&&!TargetIsBackBuffer()){const HRESULT hr=ClearHook.stdcall<HRESULT>(device,count,rects,flags,color,z,stencil);return hr;}const bool candidate=StereoWanted()&&TargetIsBackBuffer();bool duplicate=candidate;if(duplicate&&AnyAuxRenderTargetActive()){PoisonFrame(OutRunVR::StereoFailureMrtActive);duplicate=false;}if(duplicate&&!EnsureStereoResources(device)){PoisonFrame(OutRunVR::StereoFailureResourceUnavailable);duplicate=false;}const HRESULT leftHr=ClearHook.stdcall<HRESULT>(device,count,rects,flags,color,z,stencil);if(SUCCEEDED(leftHr))RecordFullMainDepthClear(device,count,rects,flags,z,stencil);if(!duplicate||FAILED(leftHr)){if(candidate&&FAILED(leftHr))PoisonFrame(OutRunVR::StereoFailureClearFailed);if(SUCCEEDED(leftHr)&&candidate){if((flags&D3DCLEAR_ZBUFFER)!=0)RightDepthSynchronized=false;if((flags&D3DCLEAR_STENCIL)!=0&&SurfaceHasStencil(TrackedDepthStencil))RightStencilSynchronized=false;}return leftHr;}D3DVIEWPORT9 savedViewport{};if(FAILED(device->GetViewport(&savedViewport))){if((flags&D3DCLEAR_ZBUFFER)!=0)RightDepthSynchronized=false;if((flags&D3DCLEAR_STENCIL)!=0&&SurfaceHasStencil(TrackedDepthStencil))RightStencilSynchronized=false;PoisonFrame(OutRunVR::StereoFailureViewportUnavailable);return leftHr;}IDirect3DSurface9*savedRt=TrackedRenderTarget,*savedDepth=TrackedDepthStencil;HRESULT rightHr=D3D_OK;bool restoreOk=true;{InternalPassScope guard;rightHr=SetRenderTargetHook.stdcall<HRESULT>(device,0u,RightEyeSurface);if(SUCCEEDED(rightHr))rightHr=SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,RightEyeDepth);if(SUCCEEDED(rightHr))rightHr=device->SetViewport(&savedViewport);if(SUCCEEDED(rightHr))rightHr=ClearHook.stdcall<HRESULT>(device,count,rects,flags,color,z,stencil);restoreOk=RestoreRightPassState(device,savedRt,savedDepth,savedViewport,nullptr,false);}if(FAILED(rightHr)){FrameRightDrawFailed=true;if((flags&D3DCLEAR_ZBUFFER)!=0)RightDepthSynchronized=false;if((flags&D3DCLEAR_STENCIL)!=0&&SurfaceHasStencil(TrackedDepthStencil))RightStencilSynchronized=false;PoisonFrame(OutRunVR::StereoFailureClearFailed);}else if(ClearCoversStereoBackbuffer(device,count,rects)){if((flags&D3DCLEAR_ZBUFFER)!=0)RightDepthSynchronized=true;if((flags&D3DCLEAR_STENCIL)!=0&&SurfaceHasStencil(TrackedDepthStencil))RightStencilSynchronized=true;}if(!restoreOk){if((flags&D3DCLEAR_ZBUFFER)!=0)RightDepthSynchronized=false;if((flags&D3DCLEAR_STENCIL)!=0&&SurfaceHasStencil(TrackedDepthStencil))RightStencilSynchronized=false;NoteRestoreFailure("right-eye clear");}return leftHr;
		}

		HRESULT __stdcall SetRenderTargetDest(IDirect3DDevice9*device,DWORD index,IDirect3DSurface9*surface){const HRESULT hr=SetRenderTargetHook.stdcall<HRESULT>(device,index,surface);if(!InternalStereoPass&&IsGameDevice(device)&&SUCCEEDED(hr)){if(index==0)ReplaceSurfaceRef(TrackedRenderTarget,surface);else if(index>=1&&index<=3)AuxRenderTargetActive[index-1]=surface!=nullptr;}return hr;}
		HRESULT __stdcall SetDepthStencilSurfaceDest(IDirect3DDevice9*device,IDirect3DSurface9*surface)
		{
			const HRESULT hr=SetDepthStencilSurfaceHook.stdcall<HRESULT>(device,surface);
			if(!InternalStereoPass&&IsGameDevice(device)&&SUCCEEDED(hr))
			{
				const bool changed=TrackedDepthStencil!=surface;
				ReplaceSurfaceRef(TrackedDepthStencil,surface);
				if(changed&&StereoResourcesReady&&TargetIsBackBuffer())
				{
					if(!surface)
					{
						ReleaseCom(RightEyeDepth);
						RightDepthSynchronized=true;
						RightStencilSynchronized=true;
					}
					else
					{
						D3DSURFACE_DESC leftDesc{},rightDesc{};
						const bool compatible=RightEyeDepth&&
							SUCCEEDED(surface->GetDesc(&leftDesc))&&
							SUCCEEDED(RightEyeDepth->GetDesc(&rightDesc))&&
							leftDesc.Width==BackBufferDesc.Width&&leftDesc.Height==BackBufferDesc.Height&&
							rightDesc.Width==BackBufferDesc.Width&&rightDesc.Height==BackBufferDesc.Height&&
							leftDesc.Format==rightDesc.Format&&
							leftDesc.MultiSampleType==rightDesc.MultiSampleType&&
							leftDesc.MultiSampleQuality==rightDesc.MultiSampleQuality;
						if(compatible)
						{
							if(!FirstMainDepthReuseLogged)
							{
								FirstMainDepthReuseLogged=true;
								spdlog::info("VR stereo: compatible main depth restored after auxiliary pass; reusing right-eye depth without desynchronizing");
							}
						}
						else
						{
							if(FrameHadDuplicatedDraw||FrameHadWorldStereo)
								PoisonFrame(OutRunVR::StereoFailureDepthStateChanged);
							if(!CreateRightDepthForTracked(device))
								StereoResourcesReady=false;
						}
					}
				}
			}
			return hr;
		}

		HRESULT __stdcall QueryIssueDest(IDirect3DQuery9*query,DWORD issueFlags){const HRESULT hr=QueryIssueHook.stdcall<HRESULT>(query,issueFlags);if(FAILED(hr)||!query||query->GetType()!=D3DQUERYTYPE_OCCLUSION)return hr;IDirect3DDevice9*queryDevice=nullptr;if(FAILED(query->GetDevice(&queryDevice))||!queryDevice)return hr;const bool gameQuery=IsGameDevice(queryDevice);queryDevice->Release();if(!gameQuery)return hr;if((issueFlags&D3DISSUE_BEGIN)!=0){ActiveOcclusionQueries.fetch_add(1,std::memory_order_acq_rel);++OcclusionQueryBegins;}if((issueFlags&D3DISSUE_END)!=0){int current=ActiveOcclusionQueries.load(std::memory_order_acquire);while(current>0&&!ActiveOcclusionQueries.compare_exchange_weak(current,current-1,std::memory_order_acq_rel,std::memory_order_acquire)){}++OcclusionQueryEnds;}return hr;}
		HRESULT __stdcall CreateQueryDest(IDirect3DDevice9*device,D3DQUERYTYPE type,IDirect3DQuery9**query){const HRESULT hr=CreateQueryHook.stdcall<HRESULT>(device,type,query);if(!IsGameDevice(device)||FAILED(hr)||type!=D3DQUERYTYPE_OCCLUSION||!query||!*query)return hr;++OcclusionQueriesCreated;if(!QueryIssueHook){void**queryVtable=*reinterpret_cast<void***>(*query);if(queryVtable)QueryIssueHook=safetyhook::create_inline(queryVtable[QueryIssueVtableIndex],QueryIssueDest);if(QueryIssueHook){OcclusionQueryTrackingUnavailable.store(false,std::memory_order_release);spdlog::info("VR stereo: occlusion-query Issue hook armed; active query draws remain single-eye without invalidating stereo frames");}else{OcclusionQueryTrackingUnavailable.store(true,std::memory_order_release);spdlog::error("VR stereo: failed to hook IDirect3DQuery9::Issue; stereo duplication disabled fail-closed for query safety");}}return hr;}
		HRESULT __stdcall SetVertexShaderDest(IDirect3DDevice9*device,IDirect3DVertexShader9*shader){const HRESULT hr=SetVertexShaderHook.stdcall<HRESULT>(device,shader);if(!InternalStereoPass&&IsGameDevice(device)&&SUCCEEDED(hr)){const std::uintptr_t next=reinterpret_cast<std::uintptr_t>(shader),previous=CurrentVertexShaderIdentity.load(std::memory_order_acquire);if(next!=previous){CurrentVertexShaderIdentity.store(next,std::memory_order_release);std::uint64_t serial=VertexShaderSerial.fetch_add(1,std::memory_order_acq_rel)+1;if(serial==0)VertexShaderSerial.fetch_add(1,std::memory_order_acq_rel);}}return hr;}

		void MaybeLogSummary()
		{
			if(!Settings::VRTelemetry)return;const ULONGLONG now=GetTickCount64();if(now-LastSummaryMs<5000)return;LastSummaryMs=now;const DWORD gamePid=GetCurrentProcessId();const DWORD hostPid=SharedState?SharedState->hostPid:0;spdlog::info("VR identity: gameBuild={} gamePid={} hostPid={} runKey={}-{}",OUTRUN_VR_BUILD_SHA,gamePid,hostPid,hostPid,gamePid);spdlog::info("VR stereo: draws={} world={} ui/effect={} fixedFn={} fixedFnWorld={} offscreenWorld={} classifyFail={} composeOk={} composeFail={} rightFail={} mrtReject={} restoreFail={} poseMismatchFrames={} multiBeginPresent={} maxBeginPerPresent={} poseSeq={} direct[frames={},fallbacks={},fenceTimeout={}] occ[created={},begin={},end={},active={},reject={}] depthSync[fullClear={},bootstrapTry={},bootstrapOk={}] failures[pose={},res={},mrt={},viewport={},classify={},depth={},stencil={},clear={},rightState={},rightWvp={},rightDraw={},restore={},compose={},present={},depthState={},poseMismatch={},offscreen={},occlusion={}]",DuplicatedDraws,WorldStereoDraws,NonWorldDuplicatedDraws,FixedFunctionNonWorldDraws,FixedFunctionWorldStereoDraws,OffscreenVerifiedWorldDraws,WorldClassificationFailures,StereoComposeSuccess,StereoComposeFailure,FrameRightDrawFailed?1:0,MrtRejectedDraws,RestoreFailures,PoseSequenceMismatchFrames,MultiBeginScenePresents,MaxBeginScenesPerPresent,FrameStereoPoseSequence,DirectTransportFrames,DirectTransportFallbacks,DirectTransportFenceTimeouts,OcclusionQueriesCreated,OcclusionQueryBegins,OcclusionQueryEnds,ActiveOcclusionQueries.load(std::memory_order_acquire),OcclusionStereoRejects,FullMainDepthClears,DepthBootstrapAttempts,DepthBootstrapSuccesses,FailureCounts[OutRunVR::StereoFailureMissingLatchedPose],FailureCounts[OutRunVR::StereoFailureResourceUnavailable],FailureCounts[OutRunVR::StereoFailureMrtActive],FailureCounts[OutRunVR::StereoFailureViewportUnavailable],FailureCounts[OutRunVR::StereoFailureWorldClassificationFailed],FailureCounts[OutRunVR::StereoFailureDepthUnsynchronized],FailureCounts[OutRunVR::StereoFailureStencilUnsynchronized],FailureCounts[OutRunVR::StereoFailureClearFailed],FailureCounts[OutRunVR::StereoFailureRightStateFailed],FailureCounts[OutRunVR::StereoFailureRightWvpUploadFailed],FailureCounts[OutRunVR::StereoFailureRightDrawFailed],FailureCounts[OutRunVR::StereoFailureRestoreFailed],FailureCounts[OutRunVR::StereoFailureComposeFailed],FailureCounts[OutRunVR::StereoFailurePresentFailed],FailureCounts[OutRunVR::StereoFailureDepthStateChanged],FailureCounts[OutRunVR::StereoFailurePoseSequenceMismatch],FailureCounts[OutRunVR::StereoFailureOffscreenWorld],FailureCounts[OutRunVR::StereoFailureOcclusionQueryActive]);
		}

		HRESULT __stdcall PresentDest(IDirect3DDevice9*device,const RECT*sourceRect,const RECT*destRect,HWND destWindowOverride,const RGNDATA*dirtyRegion)
		{
			if(!StereoBaseCallbacksArmed.load(std::memory_order_acquire)||!IsGameDevice(device))return PresentHook.stdcall<HRESULT>(device,sourceRect,destRect,destWindowOverride,dirtyRegion);
			const std::uint64_t beginNow=OutRunVRRenderer::GetBeginSceneCallCount(),beginDelta=beginNow>=LastBeginSceneCountAtPresent?beginNow-LastBeginSceneCountAtPresent:0;LastBeginSceneCountAtPresent=beginNow;if(GameplayActive()){if(beginDelta>1)++MultiBeginScenePresents;MaxBeginScenesPerPresent=std::max(MaxBeginScenesPerPresent,beginDelta);}

			const bool stereoRequested=StereoWanted();
			const std::uint32_t pendingFrameId=stereoRequested?NextStereoFrameId():0;
			const bool stereoFrameEligible=
				stereoRequested&&FrameHadWorldStereo&&FrameHadDuplicatedDraw&&
				!FrameRightDrawFailed&&!FrameStereoIncomplete&&
				FrameStereoPoseSequence&&FrameStereoMetadata.valid&&
				EnsureStereoResources(device);

			bool composedStereo=false;
			bool directTransport=false;
			bool directOnlyHold=false;
			std::uint32_t pendingPoseSequence=0;

			auto markDirectOnlyHold=[&]()
			{
				if(directOnlyHold)return;
				directOnlyHold=true;
				++DirectTransportFallbacks;
				if(!FirstDirectDeferredLogged)
				{
					FirstDirectDeferredLogged=true;
					spdlog::warn("VR DIRECT-ONLY: shared-eye frame deferred without blocking; incomplete handles are never published and OpenXR reuses the last safe projection");
				}
			};

			if(stereoFrameEligible)
			{
				if(HostDirectTransportReady())
					directTransport=ResolveDirectTransport(device,pendingFrameId);

				if(directTransport)
				{
					composedStereo=true;
					pendingPoseSequence=FrameStereoPoseSequence;
					if(!FirstDirectTransportLogged)
					{
						FirstDirectTransportLogged=true;
						spdlog::info("VR stereo: 4-slot direct GPU transport uses bounded post-Present completion; source eye={}x{}",BackBufferDesc.Width,BackBufferDesc.Height);
					}
				}
				else if(Settings::VRDirectGpuOnly)
				{
					markDirectOnlyHold();
				}
				else if(ComposeSbs(device))
				{
					++StereoComposeSuccess;
					composedStereo=true;
					pendingPoseSequence=FrameStereoPoseSequence;
					++DirectTransportFallbacks;
					if(!FirstDirectFallbackLogged)
					{
						FirstDirectFallbackLogged=true;
						spdlog::warn("VR stereo: direct transport not ready; keeping SBS/Desktop Duplication fallback");
					}
				}
				else
				{
					++StereoComposeFailure;
					PoisonFrame(OutRunVR::StereoFailureComposeFailed);
				}
			}

			MaybeLogSummary();
			LARGE_INTEGER presentStart{};
			QueryPerformanceCounter(&presentStart);

			// Never advertise DirectGPU handles before their event query completes.
			// The classic path keeps its existing pre-Present correlation marker.
			if(stereoRequested&&!directTransport)
				PublishRenderFrame(OutRunVR::StereoSbsFallbackMono,pendingFrameId,pendingPoseSequence,presentStart.QuadPart,FrameFailureReason,nullptr,true,false);

			const HRESULT hr=PresentHook.stdcall<HRESULT>(device,sourceRect,destRect,destWindowOverride,dirtyRegion);

			if(directTransport)
			{
				if(SUCCEEDED(hr)&&DirectTransportFrameReadyAfterPresent(pendingFrameId))
				{
					++DirectTransportFrames;
				}
				else
				{
					directTransport=false;
					composedStereo=false;
					markDirectOnlyHold();
				}
			}

			if(composedStereo&&SUCCEEDED(hr)&&!FrameStereoIncomplete)
			{
				PublishStereoState(OutRunVR::StereoSbsActive,true,pendingPoseSequence,pendingFrameId);
				PublishRenderFrame(OutRunVR::StereoSbsActive,pendingFrameId,pendingPoseSequence,presentStart.QuadPart,OutRunVR::StereoFailureNone,&FrameStereoMetadata,false,directTransport);
				if(!FirstStereoActiveLogged)
				{
					FirstStereoActiveLogged=true;
					spdlog::info("VR stereo: TRUE STEREO active; transport={} sourceEye={}x{} PC={}",directTransport?"verified D3D9Ex->D3D11 ring":"SBS Desktop Duplication",BackBufferDesc.Width,BackBufferDesc.Height,(directTransport&&HostDirectTransportReady())?"left-eye mirror":"SBS fallback");
				}
			}
			else if(!(directOnlyHold&&stereoFrameEligible&&SUCCEEDED(hr)))
			{
				if(FAILED(hr)&&FrameFailureReason==OutRunVR::StereoFailureNone)PoisonFrame(OutRunVR::StereoFailurePresentFailed);
				const std::uint32_t fallback=stereoRequested?OutRunVR::StereoSbsFallbackMono:OutRunVR::StereoDisabled;
				PublishStereoState(fallback,false,0,0);
				PublishRenderFrame(fallback,0,0,presentStart.QuadPart,FrameFailureReason,nullptr);
			}
			// A transient DirectGPU miss in direct-only mode deliberately leaves the
			// previous valid stereo metadata intact. The host already owns/reuses the
			// last released OpenXR projection, avoiding a black/corrupt mode flip.

			OutRunVRRenderer::NotifyGamePresent();
			FrameHadDuplicatedDraw=false;FrameHadWorldStereo=false;FrameRightDrawFailed=false;FrameStereoIncomplete=false;FramePoseMismatchLogged=false;FrameFailureReason=OutRunVR::StereoFailureNone;FrameStereoPoseSequence=0;FrameStereoMetadata={};if(++PresentEpoch==0)PresentEpoch=1;return hr;
		}

		HRESULT __stdcall ResetDest(IDirect3DDevice9*device,D3DPRESENT_PARAMETERS*params){if(!StereoBaseCallbacksArmed.load(std::memory_order_acquire)||!IsGameDevice(device))return ResetHook.stdcall<HRESULT>(device,params);InvalidateTrackedRenderStates();OutRunVRRenderer::NotifyGameReset();ReleaseStereoResources();AuxRenderTargetActive={};ActiveOcclusionQueries.store(0,std::memory_order_release);CurrentVertexShaderIdentity.store(0,std::memory_order_release);VertexShaderSerial.store(0,std::memory_order_release);LastStereoWanted=false;RightStencilSynchronized=true;FramePoseMismatchLogged=false;FirstMainDepthReuseLogged=false;FirstMainClearLogged=false;FirstDepthBootstrapLogged=false;PresentEpoch=1;LastMainDepthClearEpoch=0;LastMainDepthClearFlags=0;LastMainDepthClearZ=1.0f;LastMainDepthClearStencil=0;LastMainDepthClearDesc={};LastBeginSceneCountAtPresent=OutRunVRRenderer::GetBeginSceneCallCount();FrameStereoIncomplete=false;FrameFailureReason=OutRunVR::StereoFailureNone;FrameStereoMetadata={};PublishStereoState(OutRunVR::StereoDisabled,false,0,0);PublishRenderFrame(OutRunVR::StereoDisabled,0,0,0,OutRunVR::StereoFailureNone,nullptr);HRESULT hr=D3DERR_INVALIDCALL;if(OutRunVRD3D9ExUpgradeR13::IsCompatDevice(device)){if(!OutRunVRD3D9ExUpgradeR13::ResetCompatDevice(device,params,hr)){spdlog::error("VR stereo: promoted D3D9Ex device lost ResetEx fallback ownership");return D3DERR_INVALIDCALL;}}else hr=ResetHook.stdcall<HRESULT>(device,params);if(SUCCEEDED(hr)){PrimeTrackedRenderStates(device);EnsureStereoResources(device);}return hr;}

		constexpr std::uint32_t StereoInstallPending = 0;
		constexpr std::uint32_t StereoInstallReady = 1;
		constexpr std::uint32_t StereoInstallFailed = 2;
		std::atomic<std::uint32_t> StereoInstallState{StereoInstallPending};
		std::atomic<IDirect3DDevice9*> StereoInstalledDevice{nullptr};
		std::mutex StereoInstallMutex;

		void RollbackStereoHooks() noexcept
		{
			StereoBaseCallbacksArmed.store(false, std::memory_order_release);
			QueryIssueHook = {};
			CreateQueryHook = {};
			SetVertexShaderHook = {};
			SetRenderStateHook = {};
			DrawIndexedPrimitiveUPHook = {};
			DrawPrimitiveUPHook = {};
			DrawIndexedPrimitiveHook = {};
			DrawPrimitiveHook = {};
			ClearHook = {};
			SetDepthStencilSurfaceHook = {};
			SetRenderTargetHook = {};
			PresentHook = {};
			ResetHook = {};
			StereoInstalledDevice.store(nullptr, std::memory_order_release);
		}

		bool InstallStereoHooks(IDirect3DDevice9*device)
		{
			std::lock_guard<std::mutex> installLock(StereoInstallMutex);
			if(StereoInstallState.load(std::memory_order_acquire)==StereoInstallReady)
				return StereoInstalledDevice.load(std::memory_order_acquire)==device;
			if(!device)
			{
				StereoInstallState.store(StereoInstallFailed,std::memory_order_release);
				return false;
			}
			void**vtable=*reinterpret_cast<void***>(device);
			if(!vtable)
			{
				StereoInstallState.store(StereoInstallFailed,std::memory_order_release);
				return false;
			}
			StereoInstallState.store(StereoInstallPending,std::memory_order_release);
			StereoBaseCallbacksArmed.store(false,std::memory_order_release);
			const auto disabled=safetyhook::InlineHook::StartDisabled;
			ResetHook=safetyhook::create_inline(vtable[ResetVtableIndex],ResetDest,disabled);
			PresentHook=safetyhook::create_inline(vtable[PresentVtableIndex],PresentDest,disabled);
			SetRenderTargetHook=safetyhook::create_inline(vtable[SetRenderTargetVtableIndex],SetRenderTargetDest,disabled);
			SetDepthStencilSurfaceHook=safetyhook::create_inline(vtable[SetDepthStencilSurfaceVtableIndex],SetDepthStencilSurfaceDest,disabled);
			ClearHook=safetyhook::create_inline(vtable[ClearVtableIndex],ClearDest,disabled);
			DrawPrimitiveHook=safetyhook::create_inline(vtable[DrawPrimitiveVtableIndex],DrawPrimitiveDest,disabled);
			DrawIndexedPrimitiveHook=safetyhook::create_inline(vtable[DrawIndexedPrimitiveVtableIndex],DrawIndexedPrimitiveDest,disabled);
			DrawPrimitiveUPHook=safetyhook::create_inline(vtable[DrawPrimitiveUPVtableIndex],DrawPrimitiveUPDest,disabled);
			DrawIndexedPrimitiveUPHook=safetyhook::create_inline(vtable[DrawIndexedPrimitiveUPVtableIndex],DrawIndexedPrimitiveUPDest,disabled);
			SetRenderStateHook=safetyhook::create_inline(vtable[SetRenderStateVtableIndex],SetRenderStateDest,disabled);
			SetVertexShaderHook=safetyhook::create_inline(vtable[SetVertexShaderVtableIndex],SetVertexShaderDest,disabled);
			CreateQueryHook=safetyhook::create_inline(vtable[CreateQueryVtableIndex],CreateQueryDest,disabled);
			if(!ResetHook||!PresentHook||!SetRenderTargetHook||!SetDepthStencilSurfaceHook||!ClearHook||!DrawPrimitiveHook||!DrawIndexedPrimitiveHook||!DrawPrimitiveUPHook||!DrawIndexedPrimitiveUPHook||!SetRenderStateHook||!SetVertexShaderHook||!CreateQueryHook)
			{
				RollbackStereoHooks();
				StereoInstallState.store(StereoInstallFailed,std::memory_order_release);
				spdlog::error("VR stereo: D3D9 hook transaction was partial; all base hooks rolled back immediately");
				return false;
			}
			SafetyHookInline*baseHooks[]{&SetRenderTargetHook,&SetDepthStencilSurfaceHook,&SetRenderStateHook,&SetVertexShaderHook,&CreateQueryHook,&ClearHook,&DrawPrimitiveHook,&DrawIndexedPrimitiveHook,&DrawPrimitiveUPHook,&DrawIndexedPrimitiveUPHook,&PresentHook,&ResetHook};
			for(auto*hook:baseHooks)
			{
				if(!hook->enable().has_value())
				{
					RollbackStereoHooks();
					StereoInstallState.store(StereoInstallFailed,std::memory_order_release);
					spdlog::error("VR stereo: disabled-first D3D9 hook transaction could not be enabled completely; callbacks remain disarmed");
					return false;
				}
			}
			IDirect3DVertexShader9*shader=nullptr;
			if(SUCCEEDED(device->GetVertexShader(&shader))&&shader)
			{
				CurrentVertexShaderIdentity.store(reinterpret_cast<std::uintptr_t>(shader),std::memory_order_release);
				VertexShaderSerial.store(1,std::memory_order_release);
				shader->Release();
			}
			PrimeTrackedRenderStates(device);
			IDirect3DQuery9*queryProbe=nullptr;
			if(SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_OCCLUSION,&queryProbe))&&queryProbe)queryProbe->Release();
			InitializeAuxRenderTargetState(device);
			LastBeginSceneCountAtPresent=OutRunVRRenderer::GetBeginSceneCallCount();
			EnsureSharedState();
			EnsureRenderFrameState();
			EnsureStereoResources(device);
			StereoInstalledDevice.store(device,std::memory_order_release);
			StereoBaseCallbacksArmed.store(true,std::memory_order_release);
			StereoInstallState.store(StereoInstallReady,std::memory_order_release);
			spdlog::info("VR stereo: D3D9 full-eye renderer installed (Reset/Present/RT/Depth/Clear/Draw*/RenderState/VS/CreateQuery); disabled-first callbacks armed atomically at the behavior boundary; R42 render-state shadow validates every 16 reads and c64 world checks use game upload shadow");
			return true;
		}

		DWORD WINAPI StereoInstallThread(void*)
		{
			// D3D9Ex devices are installed synchronously on their CreateDevice
			// caller thread. This worker remains only for the classic D3D9 path and
			// exits before touching a synchronously installed promoted device.
			for(int attempt=0;attempt<1200;++attempt)
			{
				if(StereoInstallState.load(std::memory_order_acquire)==StereoInstallReady)
					return 0;
				if(Game::D3DDevice_ptr&&*Game::D3DDevice_ptr)
				{
					if(!InstallStereoHooks(*Game::D3DDevice_ptr))
						spdlog::error("VR stereo: classic-path renderer hook installation failed closed");
					return 0;
				}
				Sleep(100);
			}
			StereoInstallState.store(StereoInstallFailed,std::memory_order_release);
			spdlog::warn("VR stereo: D3D9 device did not appear; stereo hooks not installed");
			return 0;
		}

	}

	bool InstallStereoHooksSynchronously(IDirect3DDevice9* device) noexcept
	{
		const bool installed=InstallStereoHooks(device);
		if(installed)
			spdlog::info("VR stereo: synchronous CreateDevice-thread base hook handoff READY; promoted D3D9Ex device exposed only after Reset ownership transferred");
		return installed;
	}

	bool IsInternalStereoPassActive(){return InternalStereoPass;}
	bool GetCurrentShaderEpoch(std::uintptr_t&shaderIdentity,std::uint64_t&serial){for(int attempt=0;attempt<3;++attempt){const std::uint64_t before=VertexShaderSerial.load(std::memory_order_acquire);const std::uintptr_t identity=CurrentVertexShaderIdentity.load(std::memory_order_acquire);const std::uint64_t after=VertexShaderSerial.load(std::memory_order_acquire);if(before==after){shaderIdentity=identity;serial=after;return identity!=0&&after!=0;}}shaderIdentity=0;serial=0;return false;}
	class VRStereoHook:public Hook
	{
	public:
		std::string_view description() override{return "OpenXRVRStereo";}
		bool validate() override{return true;}
		bool apply() override{HANDLE thread=CreateThread(nullptr,0,StereoInstallThread,nullptr,0,nullptr);if(!thread){StereoInstallState.store(StereoInstallFailed,std::memory_order_release);spdlog::error("VR stereo: failed to create installer thread: {}",GetLastError());return false;}CloseHandle(thread);return true;}
		static VRStereoHook instance;
	};
	VRStereoHook VRStereoHook::instance;
}
