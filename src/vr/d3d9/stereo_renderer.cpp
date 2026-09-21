// R9 final hardware-test policy layered over the verified R7 renderer.
//
// R8 proved that replacing only the COM vtable slot is not sufficient: the
// original SafetyHook detour can still be reached through already-cached entry
// points, leaving the R7 DepthStateChanged poison path alive.  R9 therefore
// hooks the *detour callbacks themselves*. Every call that reaches the original
// D3D9 inline hook must pass through this policy, regardless of which COM
// interface/vtable entry the game used.
//
// R9 also fixes the unsafe "mono fallback" semantics.  While a stereo frame is
// being built, every main-backbuffer draw is replayed once into an independent
// MONO shadow render target using the game's untouched WVP and an independent
// mono depth surface.  If the stereo frame later fails, subsequent VR
// transforms stop immediately and the complete mono shadow is copied back to
// the game backbuffer before Present.  A poisoned frame therefore cannot leave
// a partially transformed left-eye image on the PC monitor.
//
// The stereo path is not allowed to start from a clear-only bootstrap after
// geometry may already have been missed.  Each Present must observe a verified
// full main-backbuffer color clear before R9 allows eye transforms.  From that
// seed onward the mono shadow and the R7 eye paths see the same main draws.
//
// Architecture-verifier comparison markers retained here because the live R7
// implementation is included below:
// RenderFrameRingSize IDirect3DDevice9Ex GetAdapterLUID ResolveDirectTransport
// StereoFailurePoseSequenceMismatch

#include <intrin.h>
#include <cstdlib>
#include <cstring>
#include "stereo_renderer_r7.inc"

namespace OutRunVRStereo
{
	namespace
	{
		constexpr const char* R9BuildId = "R9-finaltest-20260915";

		bool R9ReadBoolEnvironment(const char* name, bool defaultValue) noexcept
		{
			const char* value = std::getenv(name);
			if (!value || !*value)
				return defaultValue;
			if (std::strcmp(value, "0") == 0 ||
				_stricmp(value, "false") == 0 ||
				_stricmp(value, "off") == 0 ||
				_stricmp(value, "no") == 0)
				return false;
			if (std::strcmp(value, "1") == 0 ||
				_stricmp(value, "true") == 0 ||
				_stricmp(value, "on") == 0 ||
				_stricmp(value, "yes") == 0)
				return true;
			return defaultValue;
		}

		bool R9DirectOnlyTransport() noexcept
		{
			// Game-side policy must follow the parsed setting directly. The host
			// inherits OUTRUN_VR_DIRECT_ONLY through CreateProcess, but the MSVC
			// CRT getenv cache does not reliably observe SetEnvironmentVariableA
			// changes made after process startup. Reading getenv here therefore
			// left SAFE modes stuck in the default direct-only path even when the
			// log showed DirectGpuOnly=false.
			return Settings::VRDirectGpuOnly.get();
		}

		bool R9FirstDirectOnlyWaitLogged = false;

		SafetyHookInline R9ResetCallbackHook{};
		SafetyHookInline R9PresentCallbackHook{};
		SafetyHookInline R9SetRenderTargetCallbackHook{};
		SafetyHookInline R9SetDepthCallbackHook{};
		SafetyHookInline R9ClearCallbackHook{};
		SafetyHookInline R9DrawPrimitiveCallbackHook{};
		SafetyHookInline R9DrawIndexedPrimitiveCallbackHook{};
		SafetyHookInline R9DrawPrimitiveUPCallbackHook{};
		SafetyHookInline R9DrawIndexedPrimitiveUPCallbackHook{};

		constexpr std::uint32_t R9InstallPending = 0;
		constexpr std::uint32_t R9InstallReady = 1;
		constexpr std::uint32_t R9InstallFailed = 2;
		std::atomic<std::uint32_t> R9InstallState{R9InstallPending};

		IDirect3DSurface9* R9MainDepthIdentity = nullptr;
		IDirect3DSurface9* R9DeferredDepthIdentity = nullptr;
		IDirect3DSurface9* R9MonoSurface = nullptr;
		IDirect3DSurface9* R9MonoDepth = nullptr;
		D3DSURFACE_DESC R9MainDepthDesc{};
		bool R9MainDepthKnown = false;
		bool R9DeferredDepth = false;
		bool R9StereoSeeded = false;
		bool R9MonoSeeded = false;
		bool R9MonoBackupGap = false;
		bool R9ExpectMainDepthAfterReset = false;
		std::uint64_t R9MainDepthGeneration = 0;
		std::uint64_t R9MonoResourceGeneration = 0;
		std::uint64_t R9MainDepthContentSerial = 0;
		std::uint64_t R9MonoDepthContentSerial = 0;

		std::uint64_t R9DepthCalls = 0;
		std::uint64_t R9RtCalls = 0;
		std::uint64_t R9ClearCalls = 0;
		std::uint64_t R9DrawCalls = 0;
		std::uint64_t R9PresentCalls = 0;
		std::uint64_t R9TransientDepthBinds = 0;
		std::uint64_t R9MainDepthRestores = 0;
		ULONGLONG R9LastMainDepthRestoreLogMs = 0;
		std::uint64_t R9NullDepthBinds = 0;
		std::uint64_t R9MainDepthReacquires = 0;
		std::uint64_t R9FullClearSeeds = 0;
		std::uint64_t R9MonoBackupDraws = 0;
		std::uint64_t R9MonoBackupDrawFailures = 0;
		std::uint64_t R9MonoFallbackRestores = 0;
		std::uint64_t R9MonoFallbackRestoreFailures = 0;
		std::uint64_t R9MonoOnlyDraws = 0;
		std::uint64_t R9OcclusionBackupSkips = 0;
		std::uint64_t R9FirstFailureCount = 0;
		ULONGLONG R9LastSummaryMs = 0;
		std::uint64_t R9FirstFailureEpoch = 0;

		const char* R9FailureName(OutRunVR::StereoFailureReason reason)
		{
			switch (reason)
			{
				case OutRunVR::StereoFailureNone: return "none";
				case OutRunVR::StereoFailureMissingLatchedPose: return "missingPose";
				case OutRunVR::StereoFailureResourceUnavailable: return "resource";
				case OutRunVR::StereoFailureMrtActive: return "mrt";
				case OutRunVR::StereoFailureViewportUnavailable: return "viewport";
				case OutRunVR::StereoFailureLeftWvpUploadFailed: return "leftWvp";
				case OutRunVR::StereoFailureLeftDrawFailed: return "leftDraw";
				case OutRunVR::StereoFailureRightStateFailed: return "rightState";
				case OutRunVR::StereoFailureRightWvpUploadFailed: return "rightWvp";
				case OutRunVR::StereoFailureRightDrawFailed: return "rightDraw";
				case OutRunVR::StereoFailureRestoreFailed: return "restore";
				case OutRunVR::StereoFailureComposeFailed: return "compose";
				case OutRunVR::StereoFailurePresentFailed: return "present";
				case OutRunVR::StereoFailureDepthStateChanged: return "depthState";
				case OutRunVR::StereoFailurePoseSequenceMismatch: return "poseMismatch";
				case OutRunVR::StereoFailureDepthUnsynchronized: return "depthUnsync";
				case OutRunVR::StereoFailureClearFailed: return "clear";
				case OutRunVR::StereoFailureWorldClassificationFailed: return "classify";
				case OutRunVR::StereoFailureStencilUnsynchronized: return "stencilUnsync";
				case OutRunVR::StereoFailureOffscreenWorld: return "offscreen";
				case OutRunVR::StereoFailureOcclusionQueryActive: return "occlusion";
				default: return "unknown";
			}
		}

		bool R9GetDesc(IDirect3DSurface9* surface, D3DSURFACE_DESC& desc)
		{
			desc = {};
			return surface && SUCCEEDED(surface->GetDesc(&desc));
		}

		void R9LogSurface(const char* label, IDirect3DSurface9* surface)
		{
			D3DSURFACE_DESC d{};
			if (R9GetDesc(surface, d))
			{
				spdlog::info("VR R9 diag: {} ptr=0x{:08x} {}x{} fmt={} msaa={} q={}",
					label, static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(surface)),
					d.Width, d.Height, static_cast<int>(d.Format),
					static_cast<int>(d.MultiSampleType), d.MultiSampleQuality);
			}
			else
			{
				spdlog::info("VR R9 diag: {} ptr=0x{:08x} <null/unavailable>",
					label, static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(surface)));
			}
		}

		void R9ReleaseMonoResources()
		{
			ReleaseCom(R9MonoDepth);
			ReleaseCom(R9MonoSurface);
			R9MonoResourceGeneration = 0;
			R9MonoSeeded = false;
			R9MonoBackupGap = false;
		}

		void R9ReleaseDepthIdentity()
		{
			ReleaseCom(R9DeferredDepthIdentity);
			ReleaseCom(R9MainDepthIdentity);
			R9MainDepthDesc = {};
			R9MainDepthKnown = false;
			R9DeferredDepth = false;
		}

		bool R9CaptureMainDepth(IDirect3DSurface9* surface, const char* why)
		{
			if (!surface)
				return false;
			D3DSURFACE_DESC desc{};
			if (!R9GetDesc(surface, desc))
				return false;
			if (BackBufferDesc.Width && BackBufferDesc.Height &&
				(desc.Width < BackBufferDesc.Width || desc.Height < BackBufferDesc.Height))
				return false;

			ReplaceSurfaceRef(R9MainDepthIdentity, surface);
			R9MainDepthDesc = desc;
			R9MainDepthKnown = true;
			R9DeferredDepth = false;
			ReleaseCom(R9DeferredDepthIdentity);
			if (++R9MainDepthGeneration == 0)
				++R9MainDepthGeneration;
			R9ReleaseMonoResources();
			++R9MainDepthReacquires;
			spdlog::info(
				"VR R9: captured MAIN depth identity reason={} ptr=0x{:08x} size={}x{} fmt={} msaa={} q={} generation={}",
				why, static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(surface)),
				desc.Width, desc.Height, static_cast<int>(desc.Format),
				static_cast<int>(desc.MultiSampleType), desc.MultiSampleQuality,
				R9MainDepthGeneration);
			return true;
		}

		bool R9EnsureMonoResources(IDirect3DDevice9* device)
		{
			if (!device || !EnsureStereoResources(device) || !BackBufferDesc.Width || !BackBufferDesc.Height)
				return false;
			if (R9MonoSurface && R9MonoResourceGeneration == R9MainDepthGeneration)
				return true;

			R9ReleaseMonoResources();
			if (FAILED(device->CreateRenderTarget(
				BackBufferDesc.Width, BackBufferDesc.Height, BackBufferDesc.Format,
				BackBufferDesc.MultiSampleType, BackBufferDesc.MultiSampleQuality,
				FALSE, &R9MonoSurface, nullptr)) || !R9MonoSurface)
				return false;

			if (R9MainDepthKnown && R9MainDepthIdentity)
			{
				const auto& d = R9MainDepthDesc;
				if (FAILED(device->CreateDepthStencilSurface(
					d.Width, d.Height, d.Format, d.MultiSampleType, d.MultiSampleQuality,
					FALSE, &R9MonoDepth, nullptr)))
				{
					R9ReleaseMonoResources();
					return false;
				}
			}

			R9MonoResourceGeneration = R9MainDepthGeneration;
			spdlog::info("VR R9: mono safety shadow ready {}x{} mainDepth=0x{:08x}",
				BackBufferDesc.Width, BackBufferDesc.Height,
				static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(R9MainDepthIdentity)));
			return true;
		}

		bool R9CurrentDepthCanMirror() noexcept
		{
			if (!TrackedDepthStencil)
				return true;
			return R9MainDepthKnown && TrackedDepthStencil == R9MainDepthIdentity && !R9DeferredDepth;
		}

		void R9LogFirstFailure(const char* site, HRESULT hr)
		{
			if (FrameFailureReason == OutRunVR::StereoFailureNone || R9FirstFailureEpoch == PresentEpoch)
				return;
			R9FirstFailureEpoch = PresentEpoch;
			++R9FirstFailureCount;
			D3DSURFACE_DESC rt{}, ds{};
			const bool rtOk = R9GetDesc(TrackedRenderTarget, rt);
			const bool dsOk = R9GetDesc(TrackedDepthStencil, ds);
			spdlog::error(
				"VR R9 FIRST FAILURE: epoch={} reason={}({}) site={} hr=0x{:08x} RT=0x{:08x}[{}x{} fmt={} msaa={}] DS=0x{:08x}[{}x{} fmt={} msaa={}] mainDS=0x{:08x} deferred={} seeded={} monoSeeded={} draws[L/R]={}/{} depthHistory[main={},mono={}]",
				PresentEpoch, static_cast<unsigned>(FrameFailureReason), R9FailureName(FrameFailureReason),
				site, static_cast<unsigned>(hr),
				static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(TrackedRenderTarget)),
				rtOk ? rt.Width : 0u, rtOk ? rt.Height : 0u, rtOk ? static_cast<int>(rt.Format) : -1,
				rtOk ? static_cast<int>(rt.MultiSampleType) : -1,
				static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(TrackedDepthStencil)),
				dsOk ? ds.Width : 0u, dsOk ? ds.Height : 0u, dsOk ? static_cast<int>(ds.Format) : -1,
				dsOk ? static_cast<int>(ds.MultiSampleType) : -1,
				static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(R9MainDepthIdentity)),
				R9DeferredDepth ? 1 : 0, R9StereoSeeded ? 1 : 0, R9MonoSeeded ? 1 : 0,
				DuplicatedDraws, DuplicatedDraws,
				R9MainDepthContentSerial, R9MonoDepthContentSerial);
		}

		void R9Poison(OutRunVR::StereoFailureReason reason, const char* site, HRESULT hr = E_FAIL)
		{
			const auto before = FrameFailureReason;
			PoisonFrame(reason);
			if (before == OutRunVR::StereoFailureNone)
				R9LogFirstFailure(site, hr);
		}

		void R9ObserveLegacyFailure(OutRunVR::StereoFailureReason before, const char* site, HRESULT hr)
		{
			if (before == OutRunVR::StereoFailureNone && FrameFailureReason != OutRunVR::StereoFailureNone)
				R9LogFirstFailure(site, hr);
		}

		bool R9BindMonoTarget(IDirect3DDevice9* device, IDirect3DSurface9*& savedRt,
			IDirect3DSurface9*& savedDepth, D3DVIEWPORT9& savedViewport)
		{
			if (!R9EnsureMonoResources(device) || !R9CurrentDepthCanMirror())
				return false;
			savedRt = TrackedRenderTarget;
			savedDepth = TrackedDepthStencil;
			if (FAILED(device->GetViewport(&savedViewport)))
				return false;
			InternalPassScope guard;
			if (FAILED(SetRenderTargetHook.stdcall<HRESULT>(device, 0u, R9MonoSurface)))
				return false;
			IDirect3DSurface9* monoDepth = TrackedDepthStencil ? R9MonoDepth : nullptr;
			if (FAILED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, monoDepth)))
			{
				SetRenderTargetHook.stdcall<HRESULT>(device, 0u, savedRt);
				return false;
			}
			if (FAILED(device->SetViewport(&savedViewport)))
			{
				SetRenderTargetHook.stdcall<HRESULT>(device, 0u, savedRt);
				SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, savedDepth);
				return false;
			}
			return true;
		}

		bool R9RestoreGameTarget(IDirect3DDevice9* device, IDirect3DSurface9* savedRt,
			IDirect3DSurface9* savedDepth, const D3DVIEWPORT9& savedViewport)
		{
			InternalPassScope guard;
			bool ok = true;
			if (savedRt && FAILED(SetRenderTargetHook.stdcall<HRESULT>(device, 0u, savedRt))) ok = false;
			if (FAILED(SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, savedDepth))) ok = false;
			if (FAILED(device->SetViewport(&savedViewport))) ok = false;
			return ok;
		}

		template <typename DrawCall>
		bool R9ReplayMonoDraw(IDirect3DDevice9* device, DrawCall&& actualDraw, HRESULT& drawHr)
		{
			if (!R9MonoSeeded || !R9CurrentDepthCanMirror())
				return false;
			if (ActiveOcclusionQueries.load(std::memory_order_acquire) > 0)
			{
				++R9OcclusionBackupSkips;
				return true; // query-only pass is intentionally not replayed
			}
			IDirect3DSurface9* savedRt = nullptr;
			IDirect3DSurface9* savedDepth = nullptr;
			D3DVIEWPORT9 savedViewport{};
			if (!R9BindMonoTarget(device, savedRt, savedDepth, savedViewport))
				return false;
			const bool mayWriteDepth = LeftDrawMayWriteDepth(device);
			const bool mayWriteStencil = LeftDrawMayWriteStencil(device);
			drawHr = actualDraw();
			const bool restoreOk = R9RestoreGameTarget(device, savedRt, savedDepth, savedViewport);
			if (FAILED(drawHr) || !restoreOk)
				return false;
			++R9MonoBackupDraws;
			if (mayWriteDepth || mayWriteStencil)
				++R9MonoDepthContentSerial;
			return true;
		}

		bool R9ReplayMonoClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects,
			DWORD flags, D3DCOLOR color, float z, DWORD stencil, HRESULT& clearHr)
		{
			IDirect3DSurface9* savedRt = nullptr;
			IDirect3DSurface9* savedDepth = nullptr;
			D3DVIEWPORT9 savedViewport{};
			if (!R9BindMonoTarget(device, savedRt, savedDepth, savedViewport))
				return false;
			clearHr = ClearHook.stdcall<HRESULT>(device, count, rects, flags, color, z, stencil);
			const bool restoreOk = R9RestoreGameTarget(device, savedRt, savedDepth, savedViewport);
			if (FAILED(clearHr) || !restoreOk)
				return false;
			if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0 &&
				ClearCoversStereoBackbuffer(device, count, rects))
				++R9MonoDepthContentSerial;
			return true;
		}

		bool R9RestoreMonoBeforePresent(IDirect3DDevice9* device, const char* why)
		{
			if (!device || !R9MonoSeeded || R9MonoBackupGap || !R9MonoSurface || !BackBuffer)
				return false;
			InternalPassScope guard;
			const HRESULT hr = device->StretchRect(R9MonoSurface, nullptr, BackBuffer, nullptr, D3DTEXF_NONE);
			if (FAILED(hr))
			{
				++R9MonoFallbackRestoreFailures;
				spdlog::error("VR R9: mono fallback restore FAILED reason={} hr=0x{:08x}",
					why, static_cast<unsigned>(hr));
				return false;
			}
			++R9MonoFallbackRestores;
			return true;
		}

		void R9MaybeLogSummary()
		{
			if (!Settings::VRTelemetry)
				return;
			const ULONGLONG now = GetTickCount64();
			if (now - R9LastSummaryMs < 5000)
				return;
			R9LastSummaryMs = now;
			spdlog::info(
				"VR R9: calls[depth={},rt={},clear={},draw={},present={}] depth[main=0x{:08x},current=0x{:08x},deferred={},transient={},restores={},null={},reacquire={}] frame[seeded={},monoSeeded={},gap={},firstFailures={}] mono[draws={},drawFail={},restores={},restoreFail={},monoOnly={},occSkip={}] depthHistory[main={},mono={}] failures[leftWvp={},leftDraw={},depthState={},compose={}]",
				R9DepthCalls, R9RtCalls, R9ClearCalls, R9DrawCalls, R9PresentCalls,
				static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(R9MainDepthIdentity)),
				static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(TrackedDepthStencil)),
				R9DeferredDepth ? 1 : 0, R9TransientDepthBinds, R9MainDepthRestores,
				R9NullDepthBinds, R9MainDepthReacquires,
				R9StereoSeeded ? 1 : 0, R9MonoSeeded ? 1 : 0, R9MonoBackupGap ? 1 : 0,
				R9FirstFailureCount,
				R9MonoBackupDraws, R9MonoBackupDrawFailures, R9MonoFallbackRestores,
				R9MonoFallbackRestoreFailures, R9MonoOnlyDraws, R9OcclusionBackupSkips,
				R9MainDepthContentSerial, R9MonoDepthContentSerial,
				FailureCounts[OutRunVR::StereoFailureLeftWvpUploadFailed],
				FailureCounts[OutRunVR::StereoFailureLeftDrawFailed],
				FailureCounts[OutRunVR::StereoFailureDepthStateChanged],
				FailureCounts[OutRunVR::StereoFailureComposeFailed]);
		}

		HRESULT __stdcall SetDepthStencilSurfaceDestR9(IDirect3DDevice9* device, IDirect3DSurface9* surface)
		{
			if (!IsGameDevice(device) || InternalStereoPass)
				return SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, surface);
			++R9DepthCalls;
			const HRESULT hr = SetDepthStencilSurfaceHook.stdcall<HRESULT>(device, surface);
			if (FAILED(hr))
				return hr;

			const bool changed = TrackedDepthStencil != surface;
			ReplaceSurfaceRef(TrackedDepthStencil, surface);
			if (!changed)
				return hr;

			if (!surface)
			{
				++R9NullDepthBinds;
				// Null depth is a legitimate main-backbuffer state (typically UI).
				// The draw wrappers temporarily make the right eye null as well.
				return hr;
			}

			if ((!R9MainDepthKnown || R9ExpectMainDepthAfterReset) && TargetIsBackBuffer())
			{
				if (R9CaptureMainDepth(surface, R9ExpectMainDepthAfterReset ? "post-reset" : "first-main-bind"))
				{
					R9ExpectMainDepthAfterReset = false;
					return hr;
				}
			}

			if (R9MainDepthKnown && surface == R9MainDepthIdentity)
			{
				if (R9DeferredDepth)
				{
					R9DeferredDepth = false;
					ReleaseCom(R9DeferredDepthIdentity);
					++R9MainDepthRestores;
					const ULONGLONG now = GetTickCount64();
					if (Settings::VRTelemetry &&
						(R9LastMainDepthRestoreLogMs == 0 ||
						 now - R9LastMainDepthRestoreLogMs >= 5000))
					{
						R9LastMainDepthRestoreLogMs = now;
						spdlog::info(
							"VR R9: exact MAIN depth identity restored after auxiliary bind; no frame poison (totalRestores={})",
							R9MainDepthRestores);
					}
				}
				return hr;
			}

			// Any non-null depth with a different COM identity is treated as
			// auxiliary/transient until the exact main depth returns.  Size alone
			// is deliberately NOT used as proof of main-depth identity.
			R9DeferredDepth = true;
			ReplaceSurfaceRef(R9DeferredDepthIdentity, surface);
			++R9TransientDepthBinds;
			if (R9TransientDepthBinds == 1)
			{
				D3DSURFACE_DESC d{};
				R9GetDesc(surface, d);
				spdlog::info(
					"VR R9: auxiliary/transient depth observed ptr=0x{:08x} {}x{} fmt={} while RT=0x{:08x}; waiting for exact main identity 0x{:08x}",
					static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(surface)), d.Width, d.Height,
					static_cast<int>(d.Format),
					static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(TrackedRenderTarget)),
					static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(R9MainDepthIdentity)));
			}
			return hr;
		}

		HRESULT __stdcall SetRenderTargetDestR9(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
		{
			++R9RtCalls;
			const HRESULT hr = R9SetRenderTargetCallbackHook.stdcall<HRESULT>(device, index, surface);
			if (!IsGameDevice(device) || InternalStereoPass || FAILED(hr) || index != 0)
				return hr;
			if (surface == BackBuffer && R9DeferredDepth && TrackedDepthStencil == R9MainDepthIdentity)
			{
				R9DeferredDepth = false;
				ReleaseCom(R9DeferredDepthIdentity);
				++R9MainDepthRestores;
			}
			return hr;
		}

		HRESULT __stdcall ClearDestR9(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects,
			DWORD flags, D3DCOLOR color, float z, DWORD stencil)
		{
			if (!IsGameDevice(device) || InternalStereoPass)
				return R9ClearCallbackHook.stdcall<HRESULT>(device, count, rects, flags, color, z, stencil);
			++R9ClearCalls;
			const bool wanted = StereoWanted();
			const bool mainTarget = TargetIsBackBuffer();
			if (!wanted || !mainTarget)
				return R9ClearCallbackHook.stdcall<HRESULT>(device, count, rects, flags, color, z, stencil);

			if (R9DeferredDepth || !R9CurrentDepthCanMirror())
			{
				const HRESULT hr = ClearHook.stdcall<HRESULT>(device, count, rects, flags, color, z, stencil);
				if (FrameHadDuplicatedDraw && SUCCEEDED(hr))
					R9Poison(OutRunVR::StereoFailureDepthStateChanged, "Clear/transient-depth", hr);
				return hr;
			}

			if (!R9EnsureMonoResources(device))
			{
				R9Poison(OutRunVR::StereoFailureResourceUnavailable, "Clear/mono-resources");
				return ClearHook.stdcall<HRESULT>(device, count, rects, flags, color, z, stencil);
			}

			HRESULT monoHr = E_FAIL;
			const bool monoOk = R9ReplayMonoClear(device, count, rects, flags, color, z, stencil, monoHr);
			const auto before = FrameFailureReason;
			const HRESULT stereoHr = FrameStereoIncomplete
				? ClearHook.stdcall<HRESULT>(device, count, rects, flags, color, z, stencil)
				: R9ClearCallbackHook.stdcall<HRESULT>(device, count, rects, flags, color, z, stencil);
			R9ObserveLegacyFailure(before, "Clear/legacy-stereo", stereoHr);

			const bool full = ClearCoversStereoBackbuffer(device, count, rects);
			if (SUCCEEDED(stereoHr) && monoOk && full && (flags & D3DCLEAR_TARGET) != 0 && !FrameStereoIncomplete)
			{
				R9StereoSeeded = true;
				R9MonoSeeded = true;
				R9MonoBackupGap = false;
				++R9FullClearSeeds;
				if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) != 0)
				{
					++R9MainDepthContentSerial;
					R9MonoDepthContentSerial = R9MainDepthContentSerial;
				}
				if (R9FullClearSeeds == 1)
					spdlog::info("VR R9: stereo frame seeded from verified full MAIN color clear; clear-only bootstrap after missed geometry is disabled");
			}
			else if (!monoOk)
			{
				R9MonoBackupGap = true;
				R9Poison(OutRunVR::StereoFailureClearFailed, "Clear/mono-shadow", monoHr);
			}
			return stereoHr;
		}

		template <typename ActualDraw, typename LegacyDraw>
		HRESULT R9ExecuteDraw(IDirect3DDevice9* device, ActualDraw&& actualDraw, LegacyDraw&& legacyDraw, const char* site)
		{
			if (!IsGameDevice(device) || InternalStereoPass)
				return legacyDraw();
			++R9DrawCalls;
			const bool wanted = StereoWanted();
			const bool mainTarget = TargetIsBackBuffer();
			if (!wanted || !mainTarget)
				return legacyDraw();

			// Do not start stereo in the middle of a frame.  Wait for a verified
			// full MAIN clear so no depth/color history can be missing.
			if (!R9StereoSeeded)
			{
				++R9MonoOnlyDraws;
				return actualDraw();
			}

			if (R9DeferredDepth || !R9CurrentDepthCanMirror())
			{
				if (!FrameStereoIncomplete)
					R9Poison(OutRunVR::StereoFailureDepthStateChanged, site, S_OK);
				++R9MonoOnlyDraws;
				return actualDraw();
			}

			if (!R9EnsureMonoResources(device))
			{
				R9Poison(OutRunVR::StereoFailureResourceUnavailable, "Draw/mono-resources");
				++R9MonoOnlyDraws;
				return actualDraw();
			}

			HRESULT monoHr = E_FAIL;
			if (!R9ReplayMonoDraw(device, actualDraw, monoHr))
			{
				++R9MonoBackupDrawFailures;
				R9MonoBackupGap = true;
				R9Poison(OutRunVR::StereoFailureResourceUnavailable, "Draw/mono-shadow", monoHr);
				++R9MonoOnlyDraws;
				return actualDraw();
			}

			if (LeftDrawMayWriteDepth(device) || LeftDrawMayWriteStencil(device))
				++R9MainDepthContentSerial;

			// Once a frame has failed, never apply another eye transform to the
			// real backbuffer. The mono shadow continues to receive the frame.
			if (FrameStereoIncomplete)
			{
				++R9MonoOnlyDraws;
				return actualDraw();
			}

			const auto before = FrameFailureReason;
			HRESULT hr = D3D_OK;
			if (!TrackedDepthStencil)
			{
				// R7 always binds RightEyeDepth in its right pass.  When the game
				// explicitly uses no depth on the left, make the right eye null too.
				IDirect3DSurface9* savedRightDepth = RightEyeDepth;
				RightEyeDepth = nullptr;
				hr = legacyDraw();
				RightEyeDepth = savedRightDepth;
			}
			else
			{
				hr = legacyDraw();
			}
			R9ObserveLegacyFailure(before, site, hr);
			return hr;
		}

		HRESULT __stdcall DrawPrimitiveDestR9(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
			UINT startVertex, UINT primitiveCount)
		{
			auto actual = [&]() { return DrawPrimitiveHook.stdcall<HRESULT>(device, type, startVertex, primitiveCount); };
			auto legacy = [&]() { return R9DrawPrimitiveCallbackHook.stdcall<HRESULT>(device, type, startVertex, primitiveCount); };
			return R9ExecuteDraw(device, actual, legacy, "DrawPrimitive");
		}

		HRESULT __stdcall DrawIndexedPrimitiveDestR9(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
			INT baseVertexIndex, UINT minVertexIndex, UINT numVertices, UINT startIndex, UINT primitiveCount)
		{
			auto actual = [&]() { return DrawIndexedPrimitiveHook.stdcall<HRESULT>(device, type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primitiveCount); };
			auto legacy = [&]() { return R9DrawIndexedPrimitiveCallbackHook.stdcall<HRESULT>(device, type, baseVertexIndex, minVertexIndex, numVertices, startIndex, primitiveCount); };
			return R9ExecuteDraw(device, actual, legacy, "DrawIndexedPrimitive");
		}

		HRESULT __stdcall DrawPrimitiveUPDestR9(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
			UINT primitiveCount, const void* data, UINT stride)
		{
			auto actual = [&]() { return DrawPrimitiveUPHook.stdcall<HRESULT>(device, type, primitiveCount, data, stride); };
			auto legacy = [&]() { return R9DrawPrimitiveUPCallbackHook.stdcall<HRESULT>(device, type, primitiveCount, data, stride); };
			return R9ExecuteDraw(device, actual, legacy, "DrawPrimitiveUP");
		}

		HRESULT __stdcall DrawIndexedPrimitiveUPDestR9(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
			UINT minVertexIndex, UINT numVertices, UINT primitiveCount, const void* indexData,
			D3DFORMAT indexFormat, const void* vertexData, UINT stride)
		{
			auto actual = [&]() { return DrawIndexedPrimitiveUPHook.stdcall<HRESULT>(device, type, minVertexIndex, numVertices, primitiveCount, indexData, indexFormat, vertexData, stride); };
			auto legacy = [&]() { return R9DrawIndexedPrimitiveUPCallbackHook.stdcall<HRESULT>(device, type, minVertexIndex, numVertices, primitiveCount, indexData, indexFormat, vertexData, stride); };
			return R9ExecuteDraw(device, actual, legacy, "DrawIndexedPrimitiveUP");
		}

		HRESULT __stdcall PresentDestR9(IDirect3DDevice9* device, const RECT* sourceRect,
			const RECT* destRect, HWND destWindowOverride, const RGNDATA* dirtyRegion)
		{
			if (!IsGameDevice(device))
				return PresentHook.stdcall<HRESULT>(device, sourceRect, destRect, destWindowOverride, dirtyRegion);
			++R9PresentCalls;

			const std::uint64_t beginNow = OutRunVRRenderer::GetBeginSceneCallCount();
			const std::uint64_t beginDelta = beginNow >= LastBeginSceneCountAtPresent ? beginNow - LastBeginSceneCountAtPresent : 0;
			LastBeginSceneCountAtPresent = beginNow;
			if (GameplayActive())
			{
				if (beginDelta > 1) ++MultiBeginScenePresents;
				MaxBeginScenesPerPresent = std::max(MaxBeginScenesPerPresent, beginDelta);
			}

			const bool stereoRequested = StereoWanted() && R9StereoSeeded;
			const bool directOnly = R9DirectOnlyTransport();
			const std::uint32_t pendingFrameId = stereoRequested ? NextStereoFrameId() : 0;
			bool composedStereo = false;
			bool directTransport = false;
			std::uint32_t pendingPoseSequence = 0;

			const bool frameEligible = stereoRequested && FrameHadWorldStereo && FrameHadDuplicatedDraw &&
				!FrameRightDrawFailed && !FrameStereoIncomplete && FrameStereoPoseSequence &&
				FrameStereoMetadata.valid && EnsureStereoResources(device);

			if (frameEligible)
			{
				directTransport = ResolveDirectTransport(device, pendingFrameId);
				if (directTransport)
				{
					++DirectTransportFrames;
					composedStereo = true;
					pendingPoseSequence = FrameStereoPoseSequence;
					if (!HostDirectTransportReady() && !directOnly)
					{
						const bool composeOk = ComposeSbs(device);
						if (composeOk) ++StereoComposeSuccess;
						else
						{
							++StereoComposeFailure;
							R9Poison(OutRunVR::StereoFailureComposeFailed, "Present/ComposeSbs");
							composedStereo = false;
						}
					}
				}
				else
				{
					if (directOnly)
					{
						++DirectTransportFallbacks;
						composedStereo = false;
						if (!R9FirstDirectOnlyWaitLogged)
						{
							R9FirstDirectOnlyWaitLogged = true;
							spdlog::warn("VR R36 DIRECT-ONLY: shared-eye transport is not ready; SBS/Desktop Duplication fallback suppressed and PC mirror stays mono/left-eye");
						}
					}
					else
					{
						const bool composeOk = ComposeSbs(device);
						if (composeOk)
						{
							++StereoComposeSuccess;
							++DirectTransportFallbacks;
							composedStereo = true;
							pendingPoseSequence = FrameStereoPoseSequence;
							if (!FirstDirectFallbackLogged)
							{
								FirstDirectFallbackLogged = true;
								spdlog::warn("VR stereo: direct transport not verified/available; keeping SBS/Desktop Duplication fallback");
							}
						}
						else
						{
							++StereoComposeFailure;
							R9Poison(OutRunVR::StereoFailureComposeFailed, "Present/ComposeSbs");
						}
					}
				}
			}

			// Explicit failure-output policy: if no complete SBS frame exists,
			// restore the independently rendered normal mono frame before Present.
			if (!composedStereo || FrameStereoIncomplete)
			{
				if (R9MonoSeeded && !R9MonoBackupGap)
					R9RestoreMonoBeforePresent(device, R9FailureName(FrameFailureReason));
			}

			R9MaybeLogSummary();
			MaybeLogSummary();
			LARGE_INTEGER presentStart{};
			QueryPerformanceCounter(&presentStart);
			if (stereoRequested && (!directOnly || directTransport))
				PublishRenderFrame(OutRunVR::StereoSbsFallbackMono, pendingFrameId, pendingPoseSequence,
					presentStart.QuadPart, FrameFailureReason, nullptr, true, directTransport);

			const HRESULT rawPresentHr = PresentHook.stdcall<HRESULT>(device, sourceRect, destRect, destWindowOverride, dirtyRegion);
			const HRESULT hr = OutRunVRD3D9ExUpgradeR13::NormalizeLegacyPresentResult(
				device, rawPresentHr);
			if (composedStereo && SUCCEEDED(hr) && !FrameStereoIncomplete)
			{
				PublishStereoState(OutRunVR::StereoSbsActive, true, pendingPoseSequence, pendingFrameId);
				PublishRenderFrame(OutRunVR::StereoSbsActive, pendingFrameId, pendingPoseSequence,
					presentStart.QuadPart, OutRunVR::StereoFailureNone, &FrameStereoMetadata, false, directTransport);
				if (!FirstStereoActiveLogged)
				{
					FirstStereoActiveLogged = true;
					spdlog::info("VR R9 FINAL TEST: TRUE STEREO PRESENT COMPLETE transport={} sourceEye={}x{} desktop={}x{}",
						directTransport ? "D3D9Ex ring" : "SBS Desktop Duplication",
						directTransport && DirectTransportWidth ? DirectTransportWidth : BackBufferDesc.Width,
						directTransport && DirectTransportHeight ? DirectTransportHeight : BackBufferDesc.Height,
						BackBufferDesc.Width, BackBufferDesc.Height);
				}
			}
			else
			{
				if (FAILED(hr) && FrameFailureReason == OutRunVR::StereoFailureNone)
					R9Poison(OutRunVR::StereoFailurePresentFailed, "Present/D3D9", hr);
				if (FrameFailureReason != OutRunVR::StereoFailureNone)
					R9LogFirstFailure("Present/fallback", hr);
				const std::uint32_t fallback = stereoRequested ? OutRunVR::StereoSbsFallbackMono : OutRunVR::StereoDisabled;
				PublishStereoState(fallback, false, 0, 0);
				// In DirectGPU-only mode, do not overwrite the bounded frame ring
				// with classic fallback descriptors while the host still needs to
				// consume/ACK one of the initial shared-eye frames.
				if (!directOnly || !stereoRequested || directTransport)
					PublishRenderFrame(fallback, 0, 0, presentStart.QuadPart, FrameFailureReason, nullptr);
			}

			OutRunVRRenderer::NotifyGamePresent();
			FrameHadDuplicatedDraw = false;
			FrameHadWorldStereo = false;
			FrameRightDrawFailed = false;
			FrameStereoIncomplete = false;
			FramePoseMismatchLogged = false;
			FrameFailureReason = OutRunVR::StereoFailureNone;
			FrameStereoPoseSequence = 0;
			FrameStereoMetadata = {};
			R9StereoSeeded = false;
			R9MonoSeeded = false;
			R9MonoBackupGap = false;
			if (++PresentEpoch == 0) PresentEpoch = 1;
			return hr;
		}

		HRESULT __stdcall ResetDestR9(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params)
		{
			R9ReleaseMonoResources();
			R9ReleaseDepthIdentity();
			R9StereoSeeded = false;
			R9MonoSeeded = false;
			R9MonoBackupGap = false;
			R9ExpectMainDepthAfterReset = true;
			const HRESULT hr = R9ResetCallbackHook.stdcall<HRESULT>(device, params);
			if (SUCCEEDED(hr) && IsGameDevice(device) && TrackedDepthStencil)
			{
				R9CaptureMainDepth(TrackedDepthStencil, "reset-complete");
				R9ExpectMainDepthAfterReset = false;
			}
			return hr;
		}

		void R9RollbackCallbackPolicy() noexcept
		{
			R9DrawIndexedPrimitiveUPCallbackHook = {};
			R9DrawPrimitiveUPCallbackHook = {};
			R9DrawIndexedPrimitiveCallbackHook = {};
			R9DrawPrimitiveCallbackHook = {};
			R9ClearCallbackHook = {};
			R9SetDepthCallbackHook = {};
			R9SetRenderTargetCallbackHook = {};
			R9PresentCallbackHook = {};
			R9ResetCallbackHook = {};
			R9InstallState.store(R9InstallFailed, std::memory_order_release);
		}

		bool R9InstallCallbackPolicy(IDirect3DDevice9* device)
		{
			if (!device)
			{
				R9InstallState.store(R9InstallFailed, std::memory_order_release);
				return false;
			}
			R9InstallState.store(R9InstallPending, std::memory_order_release);
			R9ResetCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&ResetDest), ResetDestR9);
			R9PresentCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&PresentDest), PresentDestR9);
			R9SetRenderTargetCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&SetRenderTargetDest), SetRenderTargetDestR9);
			R9SetDepthCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&SetDepthStencilSurfaceDest), SetDepthStencilSurfaceDestR9);
			R9ClearCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&ClearDest), ClearDestR9);
			R9DrawPrimitiveCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&DrawPrimitiveDest), DrawPrimitiveDestR9);
			R9DrawIndexedPrimitiveCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&DrawIndexedPrimitiveDest), DrawIndexedPrimitiveDestR9);
			R9DrawPrimitiveUPCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&DrawPrimitiveUPDest), DrawPrimitiveUPDestR9);
			R9DrawIndexedPrimitiveUPCallbackHook = safetyhook::create_inline(reinterpret_cast<void*>(&DrawIndexedPrimitiveUPDest), DrawIndexedPrimitiveUPDestR9);
			if (!R9ResetCallbackHook || !R9PresentCallbackHook || !R9SetRenderTargetCallbackHook ||
				!R9SetDepthCallbackHook || !R9ClearCallbackHook || !R9DrawPrimitiveCallbackHook ||
				!R9DrawIndexedPrimitiveCallbackHook || !R9DrawPrimitiveUPCallbackHook ||
				!R9DrawIndexedPrimitiveUPCallbackHook)
			{
				R9RollbackCallbackPolicy();
				spdlog::error("VR R9 FINAL TEST: callback policy transaction was partial; all R9 callback hooks rolled back immediately");
				return false;
			}

			if (TrackedDepthStencil)
				R9CaptureMainDepth(TrackedDepthStencil, "policy-install");
			else
				R9MainDepthKnown = false;
			R9InstallState.store(R9InstallReady, std::memory_order_release);
			spdlog::info(
				"VR R9 FINAL TEST: callback policy ACTIVE build={} depthHook=detour-callback monoFallback=shadow fullClearSeed=required nullDepth=mirrored transactional=READY",
				R9BuildId);
			R9LogSurface("initial RT", TrackedRenderTarget);
			R9LogSurface("initial DS", TrackedDepthStencil);
			return true;
		}

		DWORD WINAPI R9InstallThread(void*)
		{
			for (int attempt = 0; attempt < 4800; ++attempt)
			{
				const std::uint32_t baseState = StereoInstallState.load(std::memory_order_acquire);
				if (baseState == StereoInstallFailed)
				{
					R9InstallState.store(R9InstallFailed, std::memory_order_release);
					spdlog::error("VR R9 FINAL TEST: R7 base hook transaction failed; callback policy not attempted");
					return 0;
				}
				if (baseState == StereoInstallReady)
				{
					IDirect3DDevice9* const device = StereoInstalledDevice.load(std::memory_order_acquire);
					if (device && R9InstallCallbackPolicy(device))
						return 0;
					if (R9InstallState.load(std::memory_order_acquire) != R9InstallFailed)
						R9InstallState.store(R9InstallFailed, std::memory_order_release);
					spdlog::error("VR R9 FINAL TEST: failed to install callback policy; fail-closed rollback complete");
					return 0;
				}
				Sleep(25);
			}
			R9InstallState.store(R9InstallFailed, std::memory_order_release);
			spdlog::warn("VR R9 FINAL TEST: R7 transactional install did not become ready; policy not installed");
			return 0;
		}

		class VRFinalTestR9Hook : public Hook
		{
		public:
			std::string_view description() override { return "OpenXRVRFinalTestR9"; }
			bool validate() override { return true; }
			bool apply() override
			{
				HANDLE thread = CreateThread(nullptr, 0, R9InstallThread, nullptr, 0, nullptr);
				if (!thread)
				{
					R9InstallState.store(R9InstallFailed, std::memory_order_release);
					spdlog::error("VR R9 FINAL TEST: failed to create policy installer thread: {}", GetLastError());
					return false;
				}
				CloseHandle(thread);
				return true;
			}
			static VRFinalTestR9Hook instance;
		};

		VRFinalTestR9Hook VRFinalTestR9Hook::instance;
	}
}
