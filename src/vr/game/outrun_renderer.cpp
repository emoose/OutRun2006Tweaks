#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <spdlog/spdlog.h>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "input_manager.hpp"
#include "vr_shared.hpp"
#include "vr/ipc/host_pose_v3.hpp"
#include "vr/ipc/cadence_v1.hpp"

// Authoritative renderer-side OpenXR head-pose injector for OutRun 2006.
//
// Kim2091/Remix-Wrappers independently reverse-engineered the relevant PC
// renderer path:
//   0x0095DB20  WorldView = World * View
//   0x0095D8A0  Projection
//   VS c64..c67 = Transpose(WorldView * Projection)
// with the live View at 0x0095D860. The game camera is right-handed and uses
// D3DXMatrixLookAtRH / D3DXMatrixPerspectiveFovRH, matching OpenXR's RH basis.
//
// The final shader upload remains the authoritative visual transform. A small
// render-phase camera-state sync mirrors the same pose into cam_pos/look only
// between BeginScene and EndScene so culling/billboards/flares that consult the
// live camera can follow head motion without feeding VR values back into the
// physics tick. The sync is restored before EndScene calls the original D3D9
// function. This intentionally does not claim to fix culling that happens before
// BeginScene; that boundary still needs runtime visibility testing.
//
// Stereo eye FOV/IPD are latched from the SAME immutable host snapshot as the
// head pose once per successful game BeginScene. Protocol v3 is the primary
// source; the proven v2 bridge remains a same-frame fallback until transport v3
// fully replaces the legacy frame ring.

namespace Settings
{
	extern Setting<bool> VREnabled;
	extern Setting<bool> VRAutoEnableWhenHostPresent;
	extern Setting<bool> VRHeadTracking;
	extern Setting<bool> VRPositionalTracking;
	extern Setting<bool> VRCullingCameraSync;
	extern Setting<bool> VRCullingUnionFov;
	extern Setting<float> VRWorldScale;
	extern Setting<float> VRRotationScale;
	extern Setting<int> VRMatrixOrder;
	extern Setting<bool> VRTelemetry;
	extern Setting<bool> UseNewInput;
	extern Setting<int> VRFrameCadenceMode;
	extern Setting<float> VRFrameCadenceTimeoutMs;
}

namespace OutRunVRRenderer
{
	namespace
	{
		struct Vec3 { float x, y, z; };
		struct Quat { float x, y, z, w; };
		struct PoseSample
		{
			Quat orientation{ 0.0f, 0.0f, 0.0f, 1.0f };
			Vec3 position{ 0.0f, 0.0f, 0.0f };
			bool positionValid = false;
			std::uint32_t hostPid = 0;
			std::uint32_t sequence = 0;
			std::uint32_t referenceSpaceGeneration = 0;
			bool stereoValid = false;
			SharedFov eyeFov[2]{};
			float eyeOffset[2][3]{};
			Quat eyeOrientation[2]{
				{ 0.0f, 0.0f, 0.0f, 1.0f },
				{ 0.0f, 0.0f, 0.0f, 1.0f }
			};
		};

		constexpr std::size_t BeginSceneVtableIndex = 41;
		constexpr std::size_t EndSceneVtableIndex = 42;
		constexpr std::size_t SetVertexShaderConstantFVtableIndex = 94;
		constexpr UINT OutRunWvpRegister = 64;
		constexpr UINT OutRunWvpRegisterCount = 4;

		// Stock 0x00400000 image-base addresses expressed as RVAs for ASLR safety.
		constexpr std::uintptr_t OutRunViewRva = 0x0095D860u - 0x00400000u;
		constexpr std::uintptr_t OutRunProjectionRva = 0x0095D8A0u - 0x00400000u;
		constexpr std::uintptr_t OutRunWorldViewRva = 0x0095DB20u - 0x00400000u;

		constexpr float Pi = 3.14159265358979323846f;
		constexpr float WvpVerifyAbsoluteEpsilon = 0.05f;
		constexpr LONGLONG HostPoseStaleMs = 250;

		SafetyHookInline BeginSceneHook{};
		SafetyHookInline EndSceneHook{};
		SafetyHookInline SetVertexShaderConstantFHook{};

		constexpr std::uint32_t RendererInstallPending = 0;
		constexpr std::uint32_t RendererInstallReady = 1;
		constexpr std::uint32_t RendererInstallFailed = 2;
		std::atomic<std::uint32_t> RendererInstallState{RendererInstallPending};
		std::atomic<bool> RendererInjectionAllowed{true};

		HANDLE SharedMapping = nullptr;
		SharedPoseState* SharedState = nullptr;
		LARGE_INTEGER QpcFrequency{};
		OutRunVR::IpcV3::HostPoseV3Source V3PoseSource{};

		HANDLE CadenceHostMapping = nullptr;
		const OutRunVR::CadenceV1::HostState* CadenceHostState = nullptr;
		HANDLE CadenceClientMapping = nullptr;
		OutRunVR::CadenceV1::ClientState* CadenceClientState = nullptr;
		HANDLE CadenceRequestEvent = nullptr;
		HANDLE CadencePresentedEvent = nullptr;
		std::atomic<std::uint32_t> ActiveCadenceRequestId{0};
		std::atomic<bool> CadencePacingActive{false};
		std::uint32_t CadenceAcceptedRequestId = 0;
		std::uint32_t CadencePresentedRequestId = 0;
		std::uint32_t CadenceTimedOutRequestId = 0;
		std::uint32_t CadenceTimeoutCount = 0;
		std::uint32_t CadenceLastWaitUs = 0;
		std::int64_t CadenceAcceptedQpc = 0;
		std::int64_t CadencePresentedQpc = 0;
		bool FirstCadenceAcceptedLogged = false;
		bool FirstCadenceTimeoutLogged = false;

		const D3DMATRIX* RendererView = nullptr;
		const D3DMATRIX* RendererProjection = nullptr;
		const D3DMATRIX* RendererWorldView = nullptr;
		bool RendererGlobalsChecked = false;
		bool RendererGlobalsValid = false;

		Quat CenterOrientation{ 0.0f, 0.0f, 0.0f, 1.0f };
		Vec3 CenterPosition{ 0.0f, 0.0f, 0.0f };
		bool CenterValid = false;
		bool CenterPositionValid = false;
		std::uint32_t CenterHostPid = 0;
		std::uint32_t CenterReferenceSpaceGeneration = 0;
		bool RecenterWasDown = false;
		bool AutoEnableLogged = false;

		D3DMATRIX LatchedHeadInverse{};
		bool LatchedHeadInverseValid = false;
		LatchedStereoFrame LatchedStereo{};
		std::uint32_t FrameTelemetryFlags = ClientHookAlive;
		float LatchedRelativeAngleDeg = 0.0f;
		std::uint32_t LatchedPoseSequence = 0;
		bool PresentPoseLocked = false;
		bool LastPoseSourceV3 = false;

		float LastVerifiedWvp[16]{};
		bool LastVerifiedWvpValid = false;
		std::uint32_t LastVerifiedWvpGeneration = 0;
		std::uint32_t LastVerifiedWvpPoseSequence = 0;
		std::uintptr_t LastVerifiedShaderIdentity = 0;
		std::uint64_t LastVerifiedShaderSerial = 0;

		float LastGameWvpWrite[16]{};
		float LastRawGameWvpWrite[16]{};
		bool LastGameWvpWriteValid = false;
		std::uint64_t LastGameWvpWriteSerial = 0;
		std::uint64_t LastGameWvpTopLevelDrawSerial = 0;
		std::uintptr_t LastGameWvpShaderIdentity = 0;
		std::uint64_t LastGameWvpShaderSerial = 0;

		D3DVECTOR CullingCameraSavedPos{};
		D3DVECTOR CullingCameraSavedLook{};
		EvWorkCamera* CullingCameraObject = nullptr;
		bool CullingCameraOverridden = false;
		D3DMATRIX CullingProjectionSaved{};
		bool CullingProjectionOverridden = false;

		ULONGLONG LastSummaryMs = 0;
		std::uint64_t BeginSceneCalls = 0;
		std::uint64_t WvpCandidateCalls = 0;
		std::uint64_t WvpVerifiedCalls = 0;
		std::uint64_t WvpPreparedCalls = 0;
		std::uint64_t WvpUploadSucceededCalls = 0;
		std::uint64_t WvpUploadFailedCalls = 0;
		std::uint64_t WvpRejectedCalls = 0;
		std::uint64_t UnsafeAddressRejects = 0;
		std::uint64_t ReusedPoseSceneCalls = 0;
		std::uint64_t V3PoseReads = 0;
		std::uint64_t V2PoseFallbacks = 0;
		bool FirstVerifiedLogged = false;
		bool FirstInjectedLogged = false;
		bool FirstRejectedLogged = false;
		bool FirstUnsafeAddressLogged = false;
		bool FirstUploadFailedLogged = false;
		bool CullingUnionFovDeferredLogged = false;
		bool FirstV3PoseLogged = false;
		bool FirstV2FallbackLogged = false;

		bool IsGameDevice(IDirect3DDevice9* device)
		{
			return device && Game::D3DDevice_ptr && *Game::D3DDevice_ptr == device;
		}

		Quat Normalize(Quat q)
		{
			const float lengthSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
			if (!std::isfinite(lengthSq) || lengthSq <= 1.0e-12f)
				return { 0.0f, 0.0f, 0.0f, 1.0f };
			const float invLength = 1.0f / std::sqrt(lengthSq);
			return { q.x * invLength, q.y * invLength, q.z * invLength, q.w * invLength };
		}

		bool QuaternionIsSane(const Quat& q)
		{
			if (!std::isfinite(q.x) || !std::isfinite(q.y) ||
				!std::isfinite(q.z) || !std::isfinite(q.w))
				return false;
			const float lengthSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
			return std::isfinite(lengthSq) && lengthSq > 0.25f && lengthSq < 4.0f;
		}

		bool VectorIsFinite(const Vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

		bool FovValid(const SharedFov& fov)
		{
			constexpr float limit = 1.56f;
			return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
				std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown) &&
				fov.angleLeft > -limit && fov.angleRight < limit &&
				fov.angleDown > -limit && fov.angleUp < limit &&
				fov.angleRight > fov.angleLeft + 0.05f &&
				fov.angleUp > fov.angleDown + 0.05f;
		}

		float FloatFromBits(std::uint32_t bits)
		{
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}

		bool DecodePackedEyeOrientations(const SharedPoseState& snapshot, Quat out[2])
		{
			std::int16_t packed[8]{};
			static_assert(sizeof(packed) == OutRunVR::PackedEyeOrientationBytes);
			std::memcpy(packed,
				snapshot.runtimeName + OutRunVR::PackedEyeOrientationOffset,
				sizeof(packed));
			for (int eye = 0; eye < 2; ++eye)
			{
				Quat q{
					static_cast<float>(packed[eye * 4 + 0]) / 32767.0f,
					static_cast<float>(packed[eye * 4 + 1]) / 32767.0f,
					static_cast<float>(packed[eye * 4 + 2]) / 32767.0f,
					static_cast<float>(packed[eye * 4 + 3]) / 32767.0f
				};
				if (!QuaternionIsSane(q))
					return false;
				out[eye] = Normalize(q);
			}
			return true;
		}

		Quat Conjugate(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }

		Quat MultiplyRaw(const Quat& a, const Quat& b)
		{
			return {
				a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
				a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
				a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
				a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
			};
		}

		Quat Multiply(const Quat& a, const Quat& b)
		{
			return Normalize(MultiplyRaw(a, b));
		}

		Quat AxisAngle(float x, float y, float z, float radians)
		{
			const float half = radians * 0.5f;
			const float s = std::sin(half);
			return Normalize({ x * s, y * s, z * s, std::cos(half) });
		}

		Quat ScaleRotation(Quat q, float scale)
		{
			q = Normalize(q);
			if (q.w < 0.0f)
				q = { -q.x, -q.y, -q.z, -q.w };

			const float w = std::clamp(q.w, -1.0f, 1.0f);
			const float angle = 2.0f * std::acos(w);
			const float sinHalf = std::sqrt(std::max(0.0f, 1.0f - w * w));
			if (sinHalf < 1.0e-6f || angle < 1.0e-6f)
				return { 0.0f, 0.0f, 0.0f, 1.0f };
			return AxisAngle(q.x / sinHalf, q.y / sinHalf, q.z / sinHalf, angle * scale);
		}

		Vec3 RotateVector(const Quat& qIn, const Vec3& v)
		{
			const Quat q = Normalize(qIn);
			const Quat p{ v.x, v.y, v.z, 0.0f };
			const Quat r = MultiplyRaw(MultiplyRaw(q, p), Conjugate(q));
			return { r.x, r.y, r.z };
		}

		Quat YawOnly(const Quat& orientation)
		{
			const Vec3 forward = RotateVector(orientation, { 0.0f, 0.0f, -1.0f });
			if (!VectorIsFinite(forward))
				return { 0.0f, 0.0f, 0.0f, 1.0f };
			const float planarSq = forward.x * forward.x + forward.z * forward.z;
			if (planarSq <= 1.0e-8f)
				return { 0.0f, 0.0f, 0.0f, 1.0f };
			const float yaw = std::atan2(-forward.x, -forward.z);
			return AxisAngle(0.0f, 1.0f, 0.0f, yaw);
		}

		D3DMATRIX IdentityMatrix()
		{
			D3DMATRIX out{};
			out._11 = out._22 = out._33 = out._44 = 1.0f;
			return out;
		}

		D3DMATRIX MatrixFromPose(const Quat& qIn, const Vec3& position)
		{
			const Quat q = Normalize(qIn);
			const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
			const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
			const float xw = q.x * q.w, yw = q.y * q.w, zw = q.z * q.w;

			D3DMATRIX out = IdentityMatrix();
			out._11 = 1.0f - 2.0f * (yy + zz);
			out._12 = 2.0f * (xy + zw);
			out._13 = 2.0f * (xz - yw);
			out._21 = 2.0f * (xy - zw);
			out._22 = 1.0f - 2.0f * (xx + zz);
			out._23 = 2.0f * (yz + xw);
			out._31 = 2.0f * (xz + yw);
			out._32 = 2.0f * (yz - xw);
			out._33 = 1.0f - 2.0f * (xx + yy);
			out._41 = position.x;
			out._42 = position.y;
			out._43 = position.z;
			return out;
		}

		D3DMATRIX MultiplyMatrix(const D3DMATRIX& a, const D3DMATRIX& b)
		{
			D3DMATRIX out{};
			for (int row = 0; row < 4; ++row)
				for (int col = 0; col < 4; ++col)
					for (int k = 0; k < 4; ++k)
						out.m[row][col] += a.m[row][k] * b.m[k][col];
			return out;
		}

		D3DMATRIX TransposeMatrix(const D3DMATRIX& m)
		{
			D3DMATRIX out{};
			for (int row = 0; row < 4; ++row)
				for (int col = 0; col < 4; ++col)
					out.m[row][col] = m.m[col][row];
			return out;
		}

		D3DMATRIX InverseRigid(const D3DMATRIX& m)
		{
			D3DMATRIX out = IdentityMatrix();
			out._11 = m._11; out._12 = m._21; out._13 = m._31;
			out._21 = m._12; out._22 = m._22; out._23 = m._32;
			out._31 = m._13; out._32 = m._23; out._33 = m._33;
			out._41 = -(m._41 * out._11 + m._42 * out._21 + m._43 * out._31);
			out._42 = -(m._41 * out._12 + m._42 * out._22 + m._43 * out._32);
			out._43 = -(m._41 * out._13 + m._42 * out._23 + m._43 * out._33);
			return out;
		}

		bool MatrixFinite(const D3DMATRIX& matrix)
		{
			for (int row = 0; row < 4; ++row)
				for (int col = 0; col < 4; ++col)
					if (!std::isfinite(matrix.m[row][col]))
						return false;
			return true;
		}

		D3DMATRIX ProjectionFromFov(const D3DMATRIX& base, const SharedFov& fov)
		{
			const float tanLeft = std::tan(fov.angleLeft), tanRight = std::tan(fov.angleRight);
			const float tanUp = std::tan(fov.angleUp), tanDown = std::tan(fov.angleDown);
			const float width = tanRight - tanLeft, height = tanUp - tanDown;
			D3DMATRIX out{};
			out._11 = 2.0f / width; out._22 = 2.0f / height;
			out._31 = (tanRight + tanLeft) / width; out._32 = (tanUp + tanDown) / height;
			out._33 = base._33; out._34 = base._34; out._43 = base._43; out._44 = base._44;
			return out;
		}

		bool MatrixNear(const float* candidate, const D3DMATRIX& matrix, bool transposed, float epsilon)
		{
			for (int row = 0; row < 4; ++row)
			{
				for (int col = 0; col < 4; ++col)
				{
					const float a = candidate[row * 4 + col];
					const float b = transposed ? matrix.m[col][row] : matrix.m[row][col];
					if (!std::isfinite(a) || !std::isfinite(b) || std::fabs(a - b) > epsilon)
						return false;
				}
			}
			return true;
		}

		bool IsReadableRange(const void* address, std::size_t size)
		{
			if (!address || size == 0)
				return false;

			const auto begin = reinterpret_cast<std::uintptr_t>(address);
			if (begin + size < begin)
				return false;

			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info))
				return false;
			if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD) ||
				(info.Protect & PAGE_NOACCESS))
				return false;

			const auto regionBegin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
			const auto regionEnd = regionBegin + info.RegionSize;
			return begin >= regionBegin && begin + size <= regionEnd;
		}

		bool IsWritableRange(void* address, std::size_t size)
		{
			if (!address || size == 0) return false;
			MEMORY_BASIC_INFORMATION info{};
			if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
				(info.Protect & PAGE_GUARD) || (info.Protect & PAGE_NOACCESS)) return false;
			const DWORD protect = info.Protect & 0xFFu;
			const bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
				protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
			if (!writable) return false;
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

		bool ValidateRendererGlobals()
		{
			if (RendererGlobalsChecked)
				return RendererGlobalsValid;
			RendererGlobalsChecked = true;

			if (!ImageContainsRange(OutRunViewRva, sizeof(D3DMATRIX)) ||
				!ImageContainsRange(OutRunProjectionRva, sizeof(D3DMATRIX)) ||
				!ImageContainsRange(OutRunWorldViewRva, sizeof(D3DMATRIX)))
				return false;

			RendererView = Module::exe_ptr<D3DMATRIX>(OutRunViewRva);
			RendererProjection = Module::exe_ptr<D3DMATRIX>(OutRunProjectionRva);
			RendererWorldView = Module::exe_ptr<D3DMATRIX>(OutRunWorldViewRva);
			if (!IsReadableRange(RendererView, sizeof(D3DMATRIX)) ||
				!IsReadableRange(RendererProjection, sizeof(D3DMATRIX)) ||
				!IsReadableRange(RendererWorldView, sizeof(D3DMATRIX)))
			{
				RendererView = nullptr;
				RendererProjection = nullptr;
				RendererWorldView = nullptr;
				return false;
			}

			RendererGlobalsValid = true;
			return true;
		}

		bool ReadRendererMatrices(D3DMATRIX& view, D3DMATRIX& projection, D3DMATRIX& worldView)
		{
			if (!ValidateRendererGlobals())
				return false;
			std::memcpy(&view, RendererView, sizeof(view));
			if (CullingProjectionOverridden) projection = CullingProjectionSaved;
			else std::memcpy(&projection, RendererProjection, sizeof(projection));
			std::memcpy(&worldView, RendererWorldView, sizeof(worldView));
			return MatrixFinite(view) && MatrixFinite(projection) && MatrixFinite(worldView);
		}

		bool SharedHeaderValid()
		{
			return SharedState && SharedState->magic == SharedMagic &&
				SharedState->protocolVersion == SharedProtocolVersion &&
				SharedState->structSize == sizeof(SharedPoseState);
		}

		bool EnsureSharedState()
		{
			if (SharedState)
			{
				if (!SharedHeaderValid())
					return false;
				if (QpcFrequency.QuadPart <= 0)
					QueryPerformanceFrequency(&QpcFrequency);
				InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientPid),
					static_cast<LONG>(GetCurrentProcessId()));
				return true;
			}

			SharedMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
				0, static_cast<DWORD>(sizeof(SharedPoseState)), SharedMemoryName);
			if (!SharedMapping)
				return false;
			const bool mappingAlreadyExisted = GetLastError() == ERROR_ALREADY_EXISTS;

			SharedState = static_cast<SharedPoseState*>(MapViewOfFile(
				SharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedPoseState)));
			if (!SharedState)
			{
				CloseHandle(SharedMapping);
				SharedMapping = nullptr;
				return false;
			}

			// Creator-only initialization. magic is published last, so an attaching
			// process never treats a partially initialized header as ready.
			if (!mappingAlreadyExisted)
			{
				std::memset(SharedState, 0, sizeof(SharedPoseState));
				SharedState->protocolVersion = SharedProtocolVersion;
				SharedState->structSize = sizeof(SharedPoseState);
				MemoryBarrier();
				SharedState->magic = SharedMagic;
			}
			else if (!SharedHeaderValid())
			{
				// The host may still be publishing a newly-created mapping. Never
				// memset somebody else's mapping; simply retry on a later frame.
				return false;
			}

			QueryPerformanceFrequency(&QpcFrequency);
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientPid),
				static_cast<LONG>(GetCurrentProcessId()));
			spdlog::info("VR renderer: shared pose bridge ready (protocol {}, client pid={})",
				SharedProtocolVersion, GetCurrentProcessId());
			return true;
		}

		ClientPresentationMode CurrentPresentationMode()
		{
			// R45: presentation ownership follows the actual OutRun state instead
			// of Game::is_in_game() plus a 1.5 s sticky hold. The broad helper
			// includes selector/menu-adjacent 3D scenes on this executable, which
			// caused the vehicle-select preview car to receive gameplay stereo.
			// Keep only states that are genuinely rendered on the race camera in
			// stereo. Result/continue/try-again/ranking/selector states are mono
			// theater UI and must switch immediately, with no stale stereo hold.
			if (!Game::current_mode)
				return PresentationUnknown;

			const GameState state =
				static_cast<GameState>(*Game::current_mode);
			switch (state)
			{
			case STATE_START:
			case STATE_WARP:
			case STATE_RESTART:
			case STATE_GAME:
			case STATE_GIVEUP:
			case STATE_SMPAUSEMENU:
			case STATE_GOAL:
			case STATE_TIMEUP:
			case STATE_LINK_TIMEUP:
				return PresentationGameplay;
			default:
				return PresentationTheater;
			}
		}

		void PublishClientTelemetry(std::uint32_t flags, float relativeAngleDeg)
		{
			if (!EnsureSharedState())
				return;

			std::uint32_t angleBits = 0;
			static_assert(sizeof(angleBits) == sizeof(relativeAngleDeg));
			std::memcpy(&angleBits, &relativeAngleDeg, sizeof(angleBits));
			const std::uint32_t presentation = static_cast<std::uint32_t>(CurrentPresentationMode());
			const std::uint32_t gameState = Game::current_mode
				? static_cast<std::uint32_t>(*Game::current_mode) : 0xFFFFFFFFu;

			InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->clientPid),
				static_cast<LONG>(GetCurrentProcessId()));
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(&SharedState->reserved[ClientHeartbeatIndex]));
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[ClientFlagsIndex]),
				static_cast<LONG>(flags));
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[ClientLastAngleBitsIndex]),
				static_cast<LONG>(angleBits));
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[ClientPresentationModeIndex]),
				static_cast<LONG>(presentation));
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&SharedState->reserved[ClientGameStateIndex]),
				static_cast<LONG>(gameState));
		}

		bool ReadHostPose(PoseSample& pose)
		{
			OutRunVR::IpcV3::HostPoseSnapshot v3{};
			if (V3PoseSource.Read(v3))
			{
				const std::uint32_t legacySequence = static_cast<std::uint32_t>(v3.poseId & 0xFFFFFFFFu);
				if (legacySequence != 0)
				{
					pose = {};
					pose.sequence = legacySequence;
					pose.hostPid = v3.hostPid;
					pose.referenceSpaceGeneration = v3.referenceSpaceGeneration;
					pose.orientation = {
						v3.headOrientation[0], v3.headOrientation[1],
						v3.headOrientation[2], v3.headOrientation[3]
					};
					pose.position = {
						v3.headPositionMeters[0], v3.headPositionMeters[1], v3.headPositionMeters[2]
					};
					pose.positionValid = v3.positionValid;
					pose.stereoValid = v3.stereoValid;
					if (pose.stereoValid)
					{
						for (int eye = 0; eye < 2; ++eye)
						{
							pose.eyeFov[eye] = {
								v3.eyes[eye].fov.angleLeft,
								v3.eyes[eye].fov.angleRight,
								v3.eyes[eye].fov.angleUp,
								v3.eyes[eye].fov.angleDown
							};
							for (int axis = 0; axis < 3; ++axis)
								pose.eyeOffset[eye][axis] = v3.eyes[eye].positionMeters[axis];
							pose.eyeOrientation[eye] = {
								v3.eyes[eye].orientation[0], v3.eyes[eye].orientation[1],
								v3.eyes[eye].orientation[2], v3.eyes[eye].orientation[3]
							};
						}
					}
					LastPoseSourceV3 = true;
					++V3PoseReads;
					if (!FirstV3PoseLogged)
					{
						FirstV3PoseLogged = true;
						spdlog::info("VR renderer: protocol v3 pose is PRIMARY; legacy v2 remains automatic fallback");
					}
					return true;
				}
			}

			LastPoseSourceV3 = false;
			++V2PoseFallbacks;
			if (!FirstV2FallbackLogged && V3PoseReads != 0)
			{
				FirstV2FallbackLogged = true;
				spdlog::warn("VR renderer: protocol v3 pose unavailable/stale; falling back to legacy v2 pose without dropping the frame");
			}

			if (!EnsureSharedState())
				return false;

			SharedPoseState snapshot{};
			bool stable = false;
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				const std::uint32_t seqBefore = SharedState->sequence;
				if (seqBefore & 1u)
					continue;
				MemoryBarrier();
				std::memcpy(&snapshot, SharedState, sizeof(snapshot));
				MemoryBarrier();
				const std::uint32_t seqAfter = SharedState->sequence;
				if (seqBefore == seqAfter && !(seqAfter & 1u))
				{
					stable = true;
					pose.sequence = seqAfter;
					break;
				}
			}

			if (!stable || snapshot.magic != SharedMagic ||
				snapshot.protocolVersion != SharedProtocolVersion ||
				snapshot.structSize != sizeof(SharedPoseState))
				return false;
			if ((snapshot.flags & HostAlive) == 0 ||
				(snapshot.flags & OrientationValid) == 0 ||
				(snapshot.flags & SessionVisible) == 0 ||
				(snapshot.flags & HostShouldRender) == 0 || snapshot.hostPid == 0)
				return false;

			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			if (QpcFrequency.QuadPart <= 0 || snapshot.sampleQpc <= 0)
				return false;
			const LONGLONG ageTicks = now.QuadPart - snapshot.sampleQpc;
			const LONGLONG maxAgeTicks = (QpcFrequency.QuadPart * HostPoseStaleMs) / 1000;
			if (ageTicks < 0 || ageTicks > maxAgeTicks)
				return false;

			const Quat rawOrientation{
				snapshot.orientation[0], snapshot.orientation[1],
				snapshot.orientation[2], snapshot.orientation[3]
			};
			if (!QuaternionIsSane(rawOrientation))
				return false;

			pose.orientation = Normalize(rawOrientation);
			pose.position = { snapshot.position[0], snapshot.position[1], snapshot.position[2] };
			pose.positionValid = (snapshot.flags & PositionValid) != 0 && VectorIsFinite(pose.position);
			pose.hostPid = snapshot.hostPid;
			pose.referenceSpaceGeneration = snapshot.reserved[HostReferenceSpaceGenerationIndex];

			pose.stereoValid = (snapshot.flags & StereoViewsValid) != 0 &&
				(snapshot.flags & StereoEyeOrientationValid) != 0 &&
				FovValid(snapshot.eyeFov[0]) && FovValid(snapshot.eyeFov[1]);
			if (pose.stereoValid)
			{
				pose.eyeFov[0] = snapshot.eyeFov[0];
				pose.eyeFov[1] = snapshot.eyeFov[1];
				Quat eyeOrientation[2]{};
				if (!DecodePackedEyeOrientations(snapshot, eyeOrientation))
					pose.stereoValid = false;
				const std::uint32_t indexes[2][3] = {
					{ HostEyeOffsetLeftXIndex, HostEyeOffsetLeftYIndex, HostEyeOffsetLeftZIndex },
					{ HostEyeOffsetRightXIndex, HostEyeOffsetRightYIndex, HostEyeOffsetRightZIndex }
				};
				for (int eye = 0; eye < 2 && pose.stereoValid; ++eye)
				{
					for (int axis = 0; axis < 3; ++axis)
					{
						const float value = FloatFromBits(snapshot.reserved[indexes[eye][axis]]);
						if (!std::isfinite(value) || std::fabs(value) > 0.25f)
						{
							pose.stereoValid = false;
							break;
						}
						pose.eyeOffset[eye][axis] = value;
					}
					pose.eyeOrientation[eye] = eyeOrientation[eye];
				}
			}
			return true;
		}

		bool RendererRecenterActionDown()
		{
			if (Settings::UseNewInput)
				return InputManager_ModActionHeld(ModAction::VRRecenter);
			return (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
		}

		bool RendererRecenterPressed()
		{
			const bool isDown = RendererRecenterActionDown();
			const bool pressed = isDown && !RecenterWasDown;
			RecenterWasDown = isDown;
			return pressed;
		}

		bool GameRendererIsActive()
		{
			return CurrentPresentationMode() == PresentationGameplay;
		}


        bool CadenceHostHeaderValid(const OutRunVR::CadenceV1::HostState& state) noexcept
        {
            return state.magic == OutRunVR::CadenceV1::HostMagic &&
                state.version == OutRunVR::CadenceV1::ProtocolVersion &&
                state.structSize == sizeof(OutRunVR::CadenceV1::HostState);
        }

        bool EnsureCadenceChannel() noexcept
        {
            if (!CadenceHostState)
            {
                CadenceHostMapping = OpenFileMappingW(
                    FILE_MAP_READ, FALSE, OutRunVR::CadenceV1::HostStateName);
                if (!CadenceHostMapping)
                    return false;
                CadenceHostState =
                    static_cast<const OutRunVR::CadenceV1::HostState*>(
                        MapViewOfFile(CadenceHostMapping, FILE_MAP_READ, 0, 0,
                            sizeof(OutRunVR::CadenceV1::HostState)));
                if (!CadenceHostState)
                {
                    CloseHandle(CadenceHostMapping);
                    CadenceHostMapping = nullptr;
                    return false;
                }
            }

            if (!CadenceClientState)
            {
                CadenceClientMapping = CreateFileMappingW(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                    static_cast<DWORD>(sizeof(OutRunVR::CadenceV1::ClientState)),
                    OutRunVR::CadenceV1::ClientStateName);
                if (!CadenceClientMapping)
                    return false;
                CadenceClientState =
                    static_cast<OutRunVR::CadenceV1::ClientState*>(
                        MapViewOfFile(CadenceClientMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                            sizeof(OutRunVR::CadenceV1::ClientState)));
                if (!CadenceClientState)
                {
                    CloseHandle(CadenceClientMapping);
                    CadenceClientMapping = nullptr;
                    return false;
                }
                std::memset(CadenceClientState, 0, sizeof(*CadenceClientState));
                CadenceClientState->version = OutRunVR::CadenceV1::ProtocolVersion;
                CadenceClientState->structSize = sizeof(*CadenceClientState);
                CadenceClientState->clientPid = GetCurrentProcessId();
                MemoryBarrier();
                CadenceClientState->magic = OutRunVR::CadenceV1::ClientMagic;
            }

            if (!CadenceRequestEvent)
                CadenceRequestEvent = CreateEventW(
                    nullptr, FALSE, FALSE, OutRunVR::CadenceV1::RequestEventName);
            if (!CadencePresentedEvent)
                CadencePresentedEvent = CreateEventW(
                    nullptr, FALSE, FALSE, OutRunVR::CadenceV1::PresentedEventName);

            return CadenceHostState && CadenceClientState &&
                CadenceRequestEvent && CadencePresentedEvent;
        }

        bool ReadCadenceHost(OutRunVR::CadenceV1::HostState& out) noexcept
        {
            if (!EnsureCadenceChannel())
                return false;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const std::uint32_t before = CadenceHostState->sequence;
                if (before & 1u)
                    continue;
                MemoryBarrier();
                std::memcpy(&out, CadenceHostState, sizeof(out));
                MemoryBarrier();
                const std::uint32_t after = CadenceHostState->sequence;
                if (before == after && !(after & 1u) &&
                    CadenceHostHeaderValid(out))
                    return true;
            }
            return false;
        }

        void PublishCadenceClient(std::uint32_t flags) noexcept
        {
            if (!CadenceClientState)
                return;
            LONG seq = InterlockedIncrement(
                reinterpret_cast<volatile LONG*>(&CadenceClientState->sequence));
            if ((seq & 1) == 0)
                InterlockedIncrement(
                    reinterpret_cast<volatile LONG*>(&CadenceClientState->sequence));
            MemoryBarrier();
            CadenceClientState->magic = OutRunVR::CadenceV1::ClientMagic;
            CadenceClientState->version = OutRunVR::CadenceV1::ProtocolVersion;
            CadenceClientState->structSize = sizeof(*CadenceClientState);
            CadenceClientState->clientPid = GetCurrentProcessId();
            CadenceClientState->flags = flags;
            CadenceClientState->acceptedRequestId = CadenceAcceptedRequestId;
            CadenceClientState->presentedRequestId = CadencePresentedRequestId;
            CadenceClientState->timeoutCount = CadenceTimeoutCount;
            CadenceClientState->lastWaitUs = CadenceLastWaitUs;
            CadenceClientState->acceptedQpc = CadenceAcceptedQpc;
            CadenceClientState->presentedQpc = CadencePresentedQpc;
            MemoryBarrier();
            seq = InterlockedIncrement(
                reinterpret_cast<volatile LONG*>(&CadenceClientState->sequence));
            if (seq & 1)
                InterlockedIncrement(
                    reinterpret_cast<volatile LONG*>(&CadenceClientState->sequence));
        }

        void AcceptCadenceRequest(std::uint32_t requestId,
            std::uint32_t waitUs) noexcept
        {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            ActiveCadenceRequestId.store(requestId, std::memory_order_release);
            CadenceAcceptedRequestId = requestId;
            CadenceAcceptedQpc = now.QuadPart;
            CadenceLastWaitUs = waitUs;
            PublishCadenceClient(OutRunVR::CadenceV1::ClientEnabled);
            if (!FirstCadenceAcceptedLogged)
            {
                FirstCadenceAcceptedLogged = true;
                if (Settings::VRFrameCadenceMode >= 2)
                    spdlog::info(
                        "VR R35 CADENCE: SerializedProbe active; request={} bounded game/host synchronization enabled",
                        requestId);
                else
                    spdlog::info(
                        "VR R40 CADENCE: runtime-gated PhaseLock active; request={} game Present waits for the next xrWaitFrame token while the host never waits for game Present",
                        requestId);
            }
        }

        void MarkCadencePresented() noexcept
        {
            if (Settings::VRFrameCadenceMode <= 0 || !EnsureCadenceChannel())
                return;
            const std::uint32_t active =
                ActiveCadenceRequestId.load(std::memory_order_acquire);
            if (!active || active == CadencePresentedRequestId)
                return;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            CadencePresentedRequestId = active;
            CadencePresentedQpc = now.QuadPart;
            PublishCadenceClient(OutRunVR::CadenceV1::ClientEnabled);
            SetEvent(CadencePresentedEvent);
        }

        void WaitForNextCadenceRequest() noexcept
        {
            if (Settings::VRFrameCadenceMode <= 0 || !GameRendererIsActive())
            {
                CadencePacingActive.store(false, std::memory_order_release);
                return;
            }

            OutRunVR::CadenceV1::HostState host{};
            if (!ReadCadenceHost(host))
            {
                CadencePacingActive.store(false, std::memory_order_release);
                return;
            }
            const std::uint32_t required =
                OutRunVR::CadenceV1::HostEnabled |
                OutRunVR::CadenceV1::HostRunning;
            if ((host.flags & required) != required || !host.hostPid)
            {
                CadencePacingActive.store(false, std::memory_order_release);
                return;
            }

            HANDLE hostProcess = OpenProcess(SYNCHRONIZE, FALSE, host.hostPid);
            const bool hostAlive = hostProcess &&
                WaitForSingleObject(hostProcess, 0) == WAIT_TIMEOUT;
            if (hostProcess) CloseHandle(hostProcess);

            LARGE_INTEGER cadenceNow{}, cadenceFreq{};
            QueryPerformanceCounter(&cadenceNow);
            QueryPerformanceFrequency(&cadenceFreq);
            const double requestAgeMs =
                host.requestQpc > 0 && cadenceFreq.QuadPart > 0 &&
                cadenceNow.QuadPart >= host.requestQpc
                ? static_cast<double>(cadenceNow.QuadPart - host.requestQpc) *
                    1000.0 / static_cast<double>(cadenceFreq.QuadPart)
                : 0.0;
            const double staleLimitMs = std::max(
                100.0, static_cast<double>(std::clamp(
                    Settings::VRFrameCadenceTimeoutMs.get(),
                    5.0f, 100.0f)) * 4.0);
            if (!hostAlive ||
                (host.requestQpc > 0 && requestAgeMs > staleLimitMs))
            {
                CadencePacingActive.store(false, std::memory_order_release);
                ActiveCadenceRequestId.store(0, std::memory_order_release);
                return;
            }
            CadencePacingActive.store(true, std::memory_order_release);

            const std::uint32_t current =
                ActiveCadenceRequestId.load(std::memory_order_acquire);
            if (CadenceTimedOutRequestId != 0 &&
                host.requestId == CadenceTimedOutRequestId)
            {
                CadencePacingActive.store(false, std::memory_order_release);
                return;
            }
            LARGE_INTEGER start{}, end{}, frequency{};
            QueryPerformanceCounter(&start);
            QueryPerformanceFrequency(&frequency);

            if (host.requestId && host.requestId != current)
            {
                CadenceTimedOutRequestId = 0;
                AcceptCadenceRequest(host.requestId, 0);
                return;
            }

            // R40: PhaseLock is host-clock pacing, not merely telemetry.
            // The game Present thread waits for the NEXT request token, which
            // is published immediately after the host's xrBeginFrame. Crucially
            // mode 1 never makes the host wait for game Present, so there is no
            // circular wait: xrWaitFrame -> publish token -> game renders once.
            // This prevents the old mode-1 failure where the Tweaks limiter was
            // disabled while the game still ran uncapped at 100-126 FPS against
            // a 90 Hz runtime. SerializedProbe (mode 2) keeps the same game-side
            // gate and additionally enables its bounded host-side diagnostic ACK.
            PublishCadenceClient(
                OutRunVR::CadenceV1::ClientEnabled |
                OutRunVR::CadenceV1::ClientWaiting);
            const double runtimePeriodMs =
                host.predictedDisplayPeriod > 0
                ? static_cast<double>(host.predictedDisplayPeriod) / 1000000.0
                : 0.0;
            const double configuredTimeoutMs =
                static_cast<double>(std::clamp(
                    Settings::VRFrameCadenceTimeoutMs.get(),
                    5.0f, 100.0f));
            // Mode 1 must survive a temporarily late compositor long enough to
            // remove GPU pressure and recover. Four runtime periods covers the
            // observed ~39 ms VDXR interval at 90 Hz while still failing open.
            const double waitBudgetMs =
                Settings::VRFrameCadenceMode < 2
                ? std::max(configuredTimeoutMs,
                    runtimePeriodMs > 0.0 ? runtimePeriodMs * 4.0
                                          : configuredTimeoutMs)
                : (runtimePeriodMs > 0.0
                    ? runtimePeriodMs * 1.25
                    : configuredTimeoutMs);
            const DWORD timeoutMs = static_cast<DWORD>(std::clamp(
                waitBudgetMs, 5.0, 100.0));
            const DWORD wait = WaitForSingleObject(CadenceRequestEvent, timeoutMs);
            QueryPerformanceCounter(&end);
            const std::uint32_t waitUs =
                frequency.QuadPart > 0 && end.QuadPart >= start.QuadPart
                ? static_cast<std::uint32_t>(std::min<LONGLONG>(
                    0xFFFFFFFFll,
                    ((end.QuadPart - start.QuadPart) * 1000000ll) /
                        frequency.QuadPart))
                : 0u;

            OutRunVR::CadenceV1::HostState after{};
            if (ReadCadenceHost(after) && after.requestId &&
                after.requestId != current &&
                (after.flags & required) == required)
            {
                CadenceTimedOutRequestId = 0;
                AcceptCadenceRequest(after.requestId, waitUs);
                return;
            }

            CadenceLastWaitUs = waitUs;
            if (wait == WAIT_TIMEOUT)
            {
                ++CadenceTimeoutCount;
                CadenceTimedOutRequestId =
                    current ? current : host.requestId;
                CadencePacingActive.store(false, std::memory_order_release);
                PublishCadenceClient(
                    OutRunVR::CadenceV1::ClientEnabled |
                    OutRunVR::CadenceV1::ClientLastWaitTimedOut);
                if (!FirstCadenceTimeoutLogged)
                {
                    FirstCadenceTimeoutLogged = true;
                    spdlog::warn(
                        "VR R35 CADENCE: host request timeout after {} ms; failing open so OutRun cannot hang",
                        timeoutMs);
                }
            }
            else
            {
                PublishCadenceClient(OutRunVR::CadenceV1::ClientEnabled);
            }
        }

		void ResetFrameState()
		{
			LatchedHeadInverseValid = false;
			LatchedStereo = {};
			LastVerifiedWvpValid = false;
			LastVerifiedWvpPoseSequence = 0;
			LastVerifiedShaderIdentity = 0;
			LastVerifiedShaderSerial = 0;
			FrameTelemetryFlags = ClientHookAlive;
			LatchedRelativeAngleDeg = 0.0f;
			LatchedPoseSequence = 0;
		}

		void ResetCenter()
		{
			CenterValid = false;
			CenterPositionValid = false;
			CenterHostPid = 0;
			CenterReferenceSpaceGeneration = 0;
		}

		void RestoreCullingCamera()
		{
			if (CullingCameraOverridden && CullingCameraObject)
			{
				CullingCameraObject->cam_pos_F8 = CullingCameraSavedPos;
				CullingCameraObject->look_pos_104 = CullingCameraSavedLook;
			}
			CullingCameraObject = nullptr;
			CullingCameraOverridden = false;
			if (CullingProjectionOverridden && RendererProjection)
			{
				auto* projection = const_cast<D3DMATRIX*>(RendererProjection);
				if (IsWritableRange(projection, sizeof(D3DMATRIX))) std::memcpy(projection, &CullingProjectionSaved, sizeof(D3DMATRIX));
			}
			CullingProjectionOverridden = false;
		}

		void ApplyCullingCameraSync()
		{
			if (!Settings::VRCullingCameraSync || !LatchedHeadInverseValid ||
				Settings::VRMatrixOrder != 0 || !ValidateRendererGlobals())
				return;

			EvWorkCamera* cam = Game::camera();
			if (!cam || !RendererView)
				return;

			D3DMATRIX baseView{};
			std::memcpy(&baseView, RendererView, sizeof(baseView));
			if (!MatrixFinite(baseView))
				return;

			const D3DMATRIX correctedView = MultiplyMatrix(baseView, LatchedHeadInverse);
			if (!MatrixFinite(correctedView))
				return;
			const D3DMATRIX cameraWorld = InverseRigid(correctedView);

			const float dx = cam->look_pos_104.x - cam->cam_pos_F8.x;
			const float dy = cam->look_pos_104.y - cam->cam_pos_F8.y;
			const float dz = cam->look_pos_104.z - cam->cam_pos_F8.z;
			float lookDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (!std::isfinite(lookDistance) || lookDistance < 0.01f)
				lookDistance = 1.0f;

			Vec3 forward{ -cameraWorld._31, -cameraWorld._32, -cameraWorld._33 };
			const float forwardLength = std::sqrt(
				forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
			if (!std::isfinite(forwardLength) || forwardLength < 1.0e-5f)
				return;
			forward.x /= forwardLength;
			forward.y /= forwardLength;
			forward.z /= forwardLength;

			RestoreCullingCamera();
			CullingCameraObject = cam;
			CullingCameraSavedPos = cam->cam_pos_F8;
			CullingCameraSavedLook = cam->look_pos_104;
			cam->cam_pos_F8 = { cameraWorld._41, cameraWorld._42, cameraWorld._43 };
			cam->look_pos_104 = {
				cameraWorld._41 + forward.x * lookDistance,
				cameraWorld._42 + forward.y * lookDistance,
				cameraWorld._43 + forward.z * lookDistance
			};
			CullingCameraOverridden = true;
			if (Settings::VRCullingUnionFov && !CullingUnionFovDeferredLogged)
			{
				CullingUnionFovDeferredLogged = true;
				spdlog::warn("VR renderer: CullingUnionFov is deferred until a culling-only frustum boundary is verified; live projection remains untouched");
			}
			FrameTelemetryFlags |= ClientCullingCameraSynced;
		}

		void LatchFramePose()
		{
			ResetFrameState();
			RestoreCullingCamera();

			if (!GameRendererIsActive())
				return;

			PoseSample sample{};
			if (!ReadHostPose(sample))
			{
				// A torn seqlock read or one stale sample must not redefine forward.
				// Keep the existing center and simply render this scene without a VR
				// transform. Centers change only on F10, host replacement, or an
				// OpenXR reference-space generation change.
				return;
			}

			FrameTelemetryFlags |= ClientHostPoseValid;
			const bool autoEnabled = Settings::VRAutoEnableWhenHostPresent;
			const bool trackingEnabled = Settings::VRHeadTracking || autoEnabled;
			const bool enabled = (Settings::VREnabled || autoEnabled) && trackingEnabled;
			if (autoEnabled && (!Settings::VREnabled || !Settings::VRHeadTracking))
			{
				FrameTelemetryFlags |= ClientAutoEnabled;
				if (!AutoEnableLogged)
				{
					spdlog::info("VR renderer: live host pose auto-enabled renderer-side head tracking");
					AutoEnableLogged = true;
				}
			}

			if (!enabled)
				return;

			const bool recenter = RendererRecenterPressed();
			const bool hostChanged = !CenterValid || CenterHostPid != sample.hostPid;
			const bool referenceSpaceChanged = CenterValid &&
				CenterReferenceSpaceGeneration != sample.referenceSpaceGeneration;
			if (hostChanged || referenceSpaceChanged || recenter)
			{
				// Recenter yaw only. Pitch and roll continue to describe the actual
				// headset attitude, avoiding a tilted artificial horizon after F10.
				CenterOrientation = YawOnly(sample.orientation);
				CenterPosition = sample.positionValid ? sample.position : Vec3{ 0.0f, 0.0f, 0.0f };
				CenterPositionValid = sample.positionValid;
				CenterHostPid = sample.hostPid;
				CenterReferenceSpaceGeneration = sample.referenceSpaceGeneration;
				CenterValid = true;
				if (recenter)
					spdlog::info("VR renderer: yaw recentered HMD pose (configured VR Recenter action); pitch/roll preserved");
				else if (referenceSpaceChanged)
					spdlog::info("VR renderer: OpenXR reference space changed; tracking origin refreshed");
			}

			const Quat invCenter = Conjugate(Normalize(CenterOrientation));
			Quat relativeOrientation = Multiply(invCenter, sample.orientation);
			Vec3 relativePosition{ 0.0f, 0.0f, 0.0f };
			if (Settings::VRPositionalTracking && sample.positionValid)
			{
				if (!CenterPositionValid)
				{
					CenterPosition = sample.position;
					CenterPositionValid = true;
				}
				else
				{
					const Vec3 delta{
						sample.position.x - CenterPosition.x,
						sample.position.y - CenterPosition.y,
						sample.position.z - CenterPosition.z
					};
					relativePosition = RotateVector(invCenter, delta);
					relativePosition.x *= Settings::VRWorldScale;
					relativePosition.y *= Settings::VRWorldScale;
					relativePosition.z *= Settings::VRWorldScale;
				}
			}

			if (!CenterPositionValid && sample.positionValid)
			{
				CenterPosition = sample.position;
				CenterPositionValid = true;
			}

			relativeOrientation = ScaleRotation(relativeOrientation, Settings::VRRotationScale);
			LatchedHeadInverse = InverseRigid(MatrixFromPose(relativeOrientation, relativePosition));
			LatchedHeadInverseValid = MatrixFinite(LatchedHeadInverse);
			LatchedPoseSequence = sample.sequence;

			if (LatchedHeadInverseValid && sample.stereoValid)
			{
				LatchedStereo.valid = true;
				LatchedStereo.poseSequence = sample.sequence;
				LatchedStereo.eyeFov[0] = sample.eyeFov[0]; LatchedStereo.eyeFov[1] = sample.eyeFov[1];
				std::memcpy(LatchedStereo.eyeOffset, sample.eyeOffset, sizeof(LatchedStereo.eyeOffset));
				const Quat effectiveHeadOrientation = Normalize(
					Multiply(Normalize(CenterOrientation), relativeOrientation));
				// Frame.v2 positions are OpenXR LOCAL-space metres. WorldScale is
				// only the metres->game-units conversion used by LatchedHeadInverse.
				Vec3 effectiveHeadPosition = CenterPositionValid ? CenterPosition : sample.position;
				if (Settings::VRPositionalTracking && sample.positionValid)
					effectiveHeadPosition = sample.position;
				for (int eye = 0; eye < 2; ++eye)
				{
					const Quat eyeOrientation = sample.eyeOrientation[eye];
					LatchedStereo.eyeOrientation[eye][0] = eyeOrientation.x;
					LatchedStereo.eyeOrientation[eye][1] = eyeOrientation.y;
					LatchedStereo.eyeOrientation[eye][2] = eyeOrientation.z;
					LatchedStereo.eyeOrientation[eye][3] = eyeOrientation.w;

					const Quat effectiveEyeOrientation = Normalize(
						Multiply(effectiveHeadOrientation, eyeOrientation));
					LatchedStereo.effectiveEyeOrientation[eye][0] = effectiveEyeOrientation.x;
					LatchedStereo.effectiveEyeOrientation[eye][1] = effectiveEyeOrientation.y;
					LatchedStereo.effectiveEyeOrientation[eye][2] = effectiveEyeOrientation.z;
					LatchedStereo.effectiveEyeOrientation[eye][3] = effectiveEyeOrientation.w;

					// eyeOffset is already head-local metres; keep the projection-layer
					// pose in OpenXR units even though D3D9 multiplies IPD by WorldScale.
					const Vec3 localEye{
						sample.eyeOffset[eye][0],
						sample.eyeOffset[eye][1],
						sample.eyeOffset[eye][2]
					};
					const Vec3 worldEye = RotateVector(effectiveHeadOrientation, localEye);
					LatchedStereo.effectiveEyePosition[eye][0] = effectiveHeadPosition.x + worldEye.x;
					LatchedStereo.effectiveEyePosition[eye][1] = effectiveHeadPosition.y + worldEye.y;
					LatchedStereo.effectiveEyePosition[eye][2] = effectiveHeadPosition.z + worldEye.z;
				}
			}

			const float w = std::clamp(std::fabs(relativeOrientation.w), 0.0f, 1.0f);
			LatchedRelativeAngleDeg = 2.0f * std::acos(w) * (180.0f / Pi);

			// R41: the live OutRun camera may only follow the HMD during real
			// gameplay. Menu/car-select scenes do not receive stereo c64 injection;
			// moving only cam_pos/look there corrupts the preview model transform.
			if (LatchedHeadInverseValid && GameRendererIsActive())
				ApplyCullingCameraSync();
			else
				RestoreCullingCamera();
		}

		void InvalidateVerifiedWvp();

		void ReusePresentPoseForScene()
		{
			// Additional BeginScene calls before the same Present reuse the exact
			// pose chosen by the first scene. Only scene-local WVP classification
			// state is reset. This prevents mixed-pose geometry in one desktop frame.
			++ReusedPoseSceneCalls;
			InvalidateVerifiedWvp();
			RestoreCullingCamera();
			FrameTelemetryFlags = ClientHookAlive;
			if (LatchedPoseSequence != 0) FrameTelemetryFlags |= ClientHostPoseValid;
			if (Settings::VRAutoEnableWhenHostPresent) FrameTelemetryFlags |= ClientAutoEnabled;
			if (LatchedHeadInverseValid && GameRendererIsActive())
				ApplyCullingCameraSync();
			else
				RestoreCullingCamera();
		}

		bool UploadContainsOutRunWvp(UINT startRegister, UINT vector4fCount)
		{
			if (vector4fCount == 0 || vector4fCount > 256 || startRegister > OutRunWvpRegister)
				return false;
			const UINT offset = OutRunWvpRegister - startRegister;
			return vector4fCount >= offset + OutRunWvpRegisterCount;
		}

		bool UploadTouchesOutRunWvp(UINT startRegister, UINT vector4fCount)
		{
			if (vector4fCount == 0 || vector4fCount > 256) return false;
			const std::uint64_t first = startRegister;
			const std::uint64_t lastExclusive = first + vector4fCount;
			return first < OutRunWvpRegister + OutRunWvpRegisterCount &&
				lastExclusive > OutRunWvpRegister;
		}

		bool TryPrepareOutRunWvp(
			UINT startRegister, const float* constantData, UINT vector4fCount,
			float* patchedData)
		{
			if (!RendererInjectionAllowed.load(std::memory_order_acquire) ||
				!LatchedHeadInverseValid || !GameRendererIsActive())
				return false;
			if (!constantData || !patchedData || !UploadContainsOutRunWvp(startRegister, vector4fCount))
				return false;

			++WvpCandidateCalls;

			D3DMATRIX view{};
			D3DMATRIX projection{};
			D3DMATRIX worldView{};
			if (!ReadRendererMatrices(view, projection, worldView))
			{
				++UnsafeAddressRejects;
				if (!FirstUnsafeAddressLogged)
				{
					FirstUnsafeAddressLogged = true;
					spdlog::warn("VR renderer inject: OutRun renderer globals are not safely readable; injection disabled for this draw");
				}
				return false;
			}

			const UINT wvpOffsetRegisters = OutRunWvpRegister - startRegister;
			const float* uploadedWvp = constantData + wvpOffsetRegisters * 4;
			const D3DMATRIX expectedWvp = MultiplyMatrix(worldView, projection);
			if (!MatrixNear(uploadedWvp, expectedWvp, true, WvpVerifyAbsoluteEpsilon))
			{
				++WvpRejectedCalls;
				if (!FirstRejectedLogged)
				{
					FirstRejectedLogged = true;
					spdlog::info("VR renderer inject: first c64 upload did not match Transpose(WorldView*Proj); unmatched draws stay untouched");
				}
				return false;
			}

			++WvpVerifiedCalls;
			FrameTelemetryFlags |= ClientRendererWvpVerified;
			if (!FirstVerifiedLogged)
			{
				FirstVerifiedLogged = true;
				spdlog::info("VR renderer inject: verified OutRun c64 = Transpose(WorldView*Proj)");
			}

			D3DMATRIX correctedWvp{};
			if (Settings::VRMatrixOrder == 0)
			{
				correctedWvp = MultiplyMatrix(MultiplyMatrix(worldView, LatchedHeadInverse), projection);
			}
			else
			{
				// WorldView = World * View -> World = WorldView * inverse(View).
				const D3DMATRIX world = MultiplyMatrix(worldView, InverseRigid(view));
				correctedWvp = MultiplyMatrix(
					MultiplyMatrix(MultiplyMatrix(world, LatchedHeadInverse), view), projection);
			}
			if (!MatrixFinite(correctedWvp))
				return false;

			std::memcpy(patchedData, constantData, sizeof(float) * vector4fCount * 4);
			const D3DMATRIX transposed = TransposeMatrix(correctedWvp);
			std::memcpy(patchedData + wvpOffsetRegisters * 4, &transposed, sizeof(transposed));

			++WvpPreparedCalls;
			FrameTelemetryFlags |= ClientRendererMatrixPrepared;
			return true;
		}

		void InvalidateVerifiedWvp()
		{
			LastVerifiedWvpValid = false;
			LastVerifiedWvpPoseSequence = 0;
			LastVerifiedShaderIdentity = 0;
			LastVerifiedShaderSerial = 0;
		}

		void InvalidateGameWvpWrite() noexcept
		{
			LastGameWvpWriteValid = false;
			LastGameWvpTopLevelDrawSerial = 0;
			LastGameWvpShaderIdentity = 0;
			LastGameWvpShaderSerial = 0;
		}

		void RecordGameWvpWrite(const float* constants,
			const float* rawConstants) noexcept
		{
			if (!constants || !rawConstants)
			{
				InvalidateGameWvpWrite();
				return;
			}
			std::uintptr_t shaderIdentity = 0;
			std::uint64_t shaderSerial = 0;
			if (!OutRunVRStereo::GetCurrentShaderEpoch(shaderIdentity, shaderSerial) ||
				shaderIdentity == 0 || shaderSerial == 0)
			{
				InvalidateGameWvpWrite();
				return;
			}
			std::memcpy(LastGameWvpWrite, constants, sizeof(LastGameWvpWrite));
			std::memcpy(LastRawGameWvpWrite, rawConstants,
				sizeof(LastRawGameWvpWrite));
			if (++LastGameWvpWriteSerial == 0)
				++LastGameWvpWriteSerial;
			LastGameWvpTopLevelDrawSerial = OutRunVRStereo::GetTopLevelDrawSerial();
			LastGameWvpShaderIdentity = shaderIdentity;
			LastGameWvpShaderSerial = shaderSerial;
			LastGameWvpWriteValid = true;
		}

		void RecordVerifiedWvp(const float* constants)
		{
			if (!constants || LatchedPoseSequence == 0)
				return;
			std::uintptr_t shaderIdentity = 0;
			std::uint64_t shaderSerial = 0;
			if (!OutRunVRStereo::GetCurrentShaderEpoch(shaderIdentity, shaderSerial))
				return;
			std::memcpy(LastVerifiedWvp, constants, sizeof(LastVerifiedWvp));
			if (++LastVerifiedWvpGeneration == 0)
				++LastVerifiedWvpGeneration;
			LastVerifiedWvpPoseSequence = LatchedPoseSequence;
			LastVerifiedShaderIdentity = shaderIdentity;
			LastVerifiedShaderSerial = shaderSerial;
			LastVerifiedWvpValid = true;
		}

		void MaybeLogSummary()
		{
			if (!Settings::VRTelemetry)
				return;
			const ULONGLONG now = GetTickCount64();
			if (now - LastSummaryMs < 5000)
				return;
			LastSummaryMs = now;
			spdlog::info(
				"VR renderer: beginScene={} poseReuse={} c64Candidate={} verified={} prepared={} uploadOk={} uploadFail={} rejected={} unsafe={} latchedSeq={} poseSource={} v3Reads={} v2Fallbacks={} wvpGen={} presentPoseLocked={}",
				BeginSceneCalls, ReusedPoseSceneCalls, WvpCandidateCalls, WvpVerifiedCalls, WvpPreparedCalls,
				WvpUploadSucceededCalls, WvpUploadFailedCalls, WvpRejectedCalls,
				UnsafeAddressRejects, LatchedPoseSequence, LastPoseSourceV3 ? "v3" : "v2",
				V3PoseReads, V2PoseFallbacks, LastVerifiedWvpGeneration, PresentPoseLocked ? 1 : 0);
		}

		HRESULT __stdcall BeginSceneDest(IDirect3DDevice9* device)
		{
			const HRESULT result = BeginSceneHook.stdcall<HRESULT>(device);
			if (!IsGameDevice(device) || OutRunVRStereo::IsInternalStereoPassActive())
				return result;
			if (SUCCEEDED(result))
			{
				++BeginSceneCalls;
				if (!PresentPoseLocked)
				{
					LatchFramePose();
					PresentPoseLocked = GameRendererIsActive();
				}
				else
					ReusePresentPoseForScene();
			}
			else
			{
				PresentPoseLocked = false;
				ResetFrameState();
			}
			return result;
		}

		HRESULT __stdcall EndSceneDest(IDirect3DDevice9* device)
		{
			if (!IsGameDevice(device) || OutRunVRStereo::IsInternalStereoPassActive())
				return EndSceneHook.stdcall<HRESULT>(device);

			// Camera live-state sync is only for render-time culling/effects. Restore
			// it before returning control to post-scene game code.
			RestoreCullingCamera();
			const HRESULT result = EndSceneHook.stdcall<HRESULT>(device);

			std::uint32_t publishedFlags = FrameTelemetryFlags;
			if (SUCCEEDED(result))
			{
				publishedFlags |= OutRunVR::ClientFrameCompleted;
				if (publishedFlags & ClientRendererPoseInjected)
					publishedFlags |= ClientPoseApplied;
			}
			else
			{
				publishedFlags &= ~ClientPoseApplied;
			}

			PublishClientTelemetry(publishedFlags,
				(publishedFlags & ClientPoseApplied) ? LatchedRelativeAngleDeg : 0.0f);
			MaybeLogSummary();
			return result;
		}

		HRESULT __stdcall SetVertexShaderConstantFDest(
			IDirect3DDevice9* device, UINT startRegister, const float* constantData, UINT vector4fCount)
		{
			if (!IsGameDevice(device) || !constantData || OutRunVRStereo::IsInternalStereoPassActive())
			{
				return SetVertexShaderConstantFHook.stdcall<HRESULT>(
					device, startRegister, constantData, vector4fCount);
			}
			if (!UploadTouchesOutRunWvp(startRegister, vector4fCount))
			{
				return SetVertexShaderConstantFHook.stdcall<HRESULT>(
					device, startRegister, constantData, vector4fCount);
			}

			// Any GAME write touching c64..c67 supersedes the previous marker, even
			// if it updates only one register. Stereo's own per-register writes are
			// protected by InternalStereoPass above and must not disarm the marker.
			InvalidateVerifiedWvp();
			InvalidateGameWvpWrite();
			if (!UploadContainsOutRunWvp(startRegister, vector4fCount))
			{
				return SetVertexShaderConstantFHook.stdcall<HRESULT>(
					device, startRegister, constantData, vector4fCount);
			}

			float patchedData[256 * 4];
			const bool prepared = TryPrepareOutRunWvp(
				startRegister, constantData, vector4fCount, patchedData);
			const float* uploadedData = prepared ? patchedData : constantData;
			const HRESULT result = SetVertexShaderConstantFHook.stdcall<HRESULT>(
				device, startRegister, uploadedData, vector4fCount);

			if (SUCCEEDED(result))
			{
				const UINT wvpOffsetRegisters = OutRunWvpRegister - startRegister;
				RecordGameWvpWrite(
					uploadedData + wvpOffsetRegisters * 4,
					constantData + wvpOffsetRegisters * 4);
			}

			if (prepared)
			{
				if (SUCCEEDED(result))
				{
					++WvpUploadSucceededCalls;
					FrameTelemetryFlags |= ClientRendererPoseInjected;
					const UINT wvpOffsetRegisters = OutRunWvpRegister - startRegister;
					RecordVerifiedWvp(patchedData + wvpOffsetRegisters * 4);
					if (!FirstInjectedLogged)
					{
						FirstInjectedLogged = true;
						spdlog::info("VR renderer inject: HEAD TRACKING ACTIVE after successful VS c64 upload");
					}
				}
				else
				{
					++WvpUploadFailedCalls;
					FrameTelemetryFlags |= ClientRendererUploadFailed;
					if (!FirstUploadFailedLogged)
					{
						FirstUploadFailedLogged = true;
						spdlog::warn("VR renderer inject: patched c64 matrix prepared but D3D9 upload failed (HRESULT=0x{:08X})",
							static_cast<unsigned int>(result));
					}
				}
			}
			return result;
		}

		bool InstallD3D9Hooks(IDirect3DDevice9* device)
		{
			if (!device)
				return false;
			void** vtable = *reinterpret_cast<void***>(device);
			if (!vtable)
				return false;

			if (!ValidateRendererGlobals() && !FirstUnsafeAddressLogged)
			{
				FirstUnsafeAddressLogged = true;
				spdlog::warn("VR renderer: OutRun renderer globals failed executable/readability validation; c64 injection will fail closed");
			}

			RendererInstallState.store(RendererInstallPending, std::memory_order_release);
			RendererInjectionAllowed.store(true, std::memory_order_release);
			BeginSceneHook = safetyhook::create_inline(vtable[BeginSceneVtableIndex], BeginSceneDest);
			EndSceneHook = safetyhook::create_inline(vtable[EndSceneVtableIndex], EndSceneDest);
			SetVertexShaderConstantFHook = safetyhook::create_inline(
				vtable[SetVertexShaderConstantFVtableIndex], SetVertexShaderConstantFDest);

			if (!BeginSceneHook || !EndSceneHook || !SetVertexShaderConstantFHook)
			{
				BeginSceneHook = {};
				EndSceneHook = {};
				SetVertexShaderConstantFHook = {};
				RendererInjectionAllowed.store(false, std::memory_order_release);
				RendererInstallState.store(RendererInstallFailed, std::memory_order_release);
				spdlog::error("VR renderer: failed to hook D3D9 renderer boundary; transactional rollback completed");
				return false;
			}

			EnsureSharedState();
			RendererInstallState.store(RendererInstallReady, std::memory_order_release);
			spdlog::info("VR renderer: D3D9 hooks installed; atomic renderer install state=READY; v3-primary/v2-fallback frame-latched c64 WVP injection armed (vtbl 41/42/94)");
			return true;
		}

		DWORD WINAPI RendererInstallThread(void*)
		{
			for (int attempt = 0; attempt < 1200; ++attempt)
			{
				if (Game::D3DDevice_ptr && *Game::D3DDevice_ptr)
				{
					if (!InstallD3D9Hooks(*Game::D3DDevice_ptr))
						spdlog::error("VR renderer: renderer hook installation failed");
					return 0;
				}
				Sleep(100);
			}
			RendererInjectionAllowed.store(false, std::memory_order_release);
			RendererInstallState.store(RendererInstallFailed, std::memory_order_release);
			spdlog::warn("VR renderer: D3D9 device did not appear; renderer hook not installed");
			return 0;
		}
	}

	std::uint32_t GetActiveCadenceRequestId() noexcept
	{
		return ActiveCadenceRequestId.load(std::memory_order_acquire);
	}

	bool IsCadencePacingActive() noexcept
	{
		return CadencePacingActive.load(std::memory_order_acquire);
	}

	bool GetLatchedStereoFrame(LatchedStereoFrame& out)
	{
		out = LatchedStereo;
		return out.valid && out.poseSequence != 0;
	}

	bool GetLatchedHeadInverse(float outMatrix[16],
		std::uint32_t& poseSequence) noexcept
	{
		if (!outMatrix || !LatchedHeadInverseValid ||
			LatchedPoseSequence == 0)
			return false;
		std::memcpy(outMatrix, &LatchedHeadInverse,
			sizeof(LatchedHeadInverse));
		poseSequence = LatchedPoseSequence;
		return true;
	}

	std::uint64_t GetBeginSceneCallCount()
	{
		return BeginSceneCalls;
	}

	void NotifyGamePresent()
	{
		MarkCadencePresented();
		PresentPoseLocked = false;
		InvalidateVerifiedWvp();
		InvalidateGameWvpWrite();
		RestoreCullingCamera();
		WaitForNextCadenceRequest();
	}

	void NotifyGameReset()
	{
		ActiveCadenceRequestId.store(0, std::memory_order_release);
		CadencePacingActive.store(false, std::memory_order_release);
		CadenceAcceptedRequestId = 0;
		CadencePresentedRequestId = 0;
		CadenceTimedOutRequestId = 0;
		PresentPoseLocked = false;
		InvalidateGameWvpWrite();
		RestoreCullingCamera();
		ResetFrameState();
	}

	bool GetRendererBaseProjection(float outMatrix[16])
	{
		if (!outMatrix || !ValidateRendererGlobals() || !RendererProjection)
			return false;
		D3DMATRIX projection{};
		if (CullingProjectionOverridden)
			projection = CullingProjectionSaved;
		else
			std::memcpy(&projection, RendererProjection, sizeof(projection));
		if (!MatrixFinite(projection))
			return false;
		std::memcpy(outMatrix, &projection, sizeof(projection));
		return true;
	}

	bool GetLastVerifiedWvp(float outConstants[16], std::uint32_t& generation,
		std::uint32_t& poseSequence, std::uintptr_t& shaderIdentity,
		std::uint64_t& shaderSerial)
	{
		if (!outConstants || !LastVerifiedWvpValid || LastVerifiedWvpGeneration == 0 ||
			LastVerifiedWvpPoseSequence == 0 || LastVerifiedShaderIdentity == 0 ||
			LastVerifiedShaderSerial == 0)
			return false;
		std::memcpy(outConstants, LastVerifiedWvp, sizeof(LastVerifiedWvp));
		generation = LastVerifiedWvpGeneration;
		poseSequence = LastVerifiedWvpPoseSequence;
		shaderIdentity = LastVerifiedShaderIdentity;
		shaderSerial = LastVerifiedShaderSerial;
		return true;
	}

	bool GetLastGameWvpWrite(float outConstants[16], std::uint64_t& writeSerial,
		std::uint64_t& topLevelDrawSerial, std::uintptr_t& shaderIdentity,
		std::uint64_t& shaderSerial) noexcept
	{
		if (!outConstants || !LastGameWvpWriteValid ||
			LastGameWvpWriteSerial == 0 || LastGameWvpShaderIdentity == 0 ||
			LastGameWvpShaderSerial == 0)
			return false;
		std::memcpy(outConstants, LastGameWvpWrite, sizeof(LastGameWvpWrite));
		writeSerial = LastGameWvpWriteSerial;
		topLevelDrawSerial = LastGameWvpTopLevelDrawSerial;
		shaderIdentity = LastGameWvpShaderIdentity;
		shaderSerial = LastGameWvpShaderSerial;
		return true;
	}

	bool GetLastRawGameWvpWrite(float outConstants[16],
		std::uint64_t& writeSerial, std::uint64_t& topLevelDrawSerial,
		std::uintptr_t& shaderIdentity, std::uint64_t& shaderSerial) noexcept
	{
		if (!outConstants || !LastGameWvpWriteValid ||
			LastGameWvpWriteSerial == 0 || LastGameWvpShaderIdentity == 0 ||
			LastGameWvpShaderSerial == 0)
			return false;
		std::memcpy(outConstants, LastRawGameWvpWrite,
			sizeof(LastRawGameWvpWrite));
		writeSerial = LastGameWvpWriteSerial;
		topLevelDrawSerial = LastGameWvpTopLevelDrawSerial;
		shaderIdentity = LastGameWvpShaderIdentity;
		shaderSerial = LastGameWvpShaderSerial;
		return true;
	}

	class VRRendererHook : public Hook
	{
	public:
		std::string_view description() override { return "OpenXRVRRenderer"; }
		bool validate() override { return true; }

		bool apply() override
		{
			HANDLE thread = CreateThread(nullptr, 0, RendererInstallThread, nullptr, 0, nullptr);
			if (!thread)
			{
				RendererInjectionAllowed.store(false, std::memory_order_release);
				RendererInstallState.store(RendererInstallFailed, std::memory_order_release);
				spdlog::error("VR renderer: failed to create installer thread: {}", GetLastError());
				return false;
			}
			CloseHandle(thread);
			return true;
		}

		static VRRendererHook instance;
	};

	VRRendererHook VRRendererHook::instance;
}
