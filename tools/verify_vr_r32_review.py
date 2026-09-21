from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(rel: str) -> str:
    path = ROOT / rel
    if not path.is_file():
        raise SystemExit(f"R32/R33 verification missing file: {rel}")
    return path.read_text(encoding="utf-8")


def require(rel: str, *markers: str) -> str:
    data = load(rel)
    for marker in markers:
        if marker not in data:
            raise SystemExit(f"R32/R33 invariant missing: {rel} :: {marker}")
    return data


eligibility = require(
    "src/vr/runtime_eligibility.hpp",
    "ExternalSafetyBlock",
    "SetExternalSafetyBlock",
    "!ExternalSafetyBlock.load(std::memory_order_acquire)",
)
baseline_verified = eligibility.find("inline void BaselineVerified()")
baseline_block = eligibility.find(
    "ExternalSafetyBlock.load(std::memory_order_acquire)", baseline_verified)
stereo_open = eligibility.find(
    "StereoAllowed.store(true, std::memory_order_release)", baseline_verified)
if min(baseline_verified, baseline_block, stereo_open) < 0 or         not (baseline_verified < baseline_block < stereo_open):
    raise SystemExit(
        "external ResetEx safety block must be checked before BaselineVerified reopens stereo")

policy = require(
    "src/vr/d3d9/r32_policy.hpp",
    "RearmMonoSafetyEpoch",
    "ForceZeroDisparity",
    "ProducerFenceBudgetMs = 2",
    "PendingFenceDecision",
    "ClassifyPendingFence",
)

r32 = require(
    "src/vr/d3d9/stereo_renderer_r32.cpp",
    '#include "stereo_renderer_r31.cpp"',
    "R32ResetR22Hook",
    "reinterpret_cast<void*>(&ResetDestR22)",
    "R32ResetAfterGameReset",
    "R32InvalidateResetCaches",
    "R29MonoSafetyThroughEpoch = OutRunVR::R32::RearmMonoSafetyEpoch",
    "R32EffectIsFragileLive",
    "draw is forced to stock-WVP zero disparity",
    "R32SetWvpBatch",
    "OutRunWvpRegisterCount",
    "R32WaitProducerFence",
    "QueryPerformanceCounter",
    "static const LONGLONG qpcFrequency",
    "Budget starts before the FLUSH request",
    "R32ProducerFencePending",
    "R32DrainPendingProducerFence",
    "timed-out producer EVENT remains pending",
    "R32DirectIdentityMatches",
    "R32DirectCopyPathRejected",
    "A query error does not prove GPU completion",
    "StretchRect commands are already queued",
    "DirectGPU copy path is disabled until Reset/interop revalidation",
    "R32EnsureDirectResources must run before this cached rejection",
    "VR R32 PERF 5s",
    "VR R32 REVIEW2",
)
if "R32ResetR13Hook" in r32:
    raise SystemExit("R32 must no longer install a competing ResetDestR13 hook")
if "R22ShadowState = {};" in r32[r32.find("void R32ResetAfterGameReset"):]:
    raise SystemExit("R32 successful Reset post-processing must preserve R22's freshly primed viewport/scissor shadow")
resolve_start = r32.find("bool ResolveDirectTransportR32")
ensure_direct = r32.find("R32EnsureDirectResources(device)", resolve_start)
copy_reject = r32.find("if (R32DirectCopyPathRejected)", resolve_start)
if min(resolve_start, ensure_direct, copy_reject) < 0 or ensure_direct > copy_reject:
    raise SystemExit(
        "R32 DirectGPU copy rejection must be checked only after host identity/interop revalidation")

drain_start = r32.find("bool R32DrainPendingProducerFence")
drain_error = r32.find("A query error does not prove GPU completion", drain_start)
drain_end = r32.find("bool ResolveDirectTransportR32", drain_start)
if min(drain_start, drain_error, drain_end) < 0:
    raise SystemExit("could not locate R32 pending-fence query-error quarantine")
if "R32ProducerFencePending[slotIndex] = false;" in r32[drain_error:drain_end]:
    raise SystemExit(
        "R32 query-error path must not mark a producer EVENT complete/reusable")

issue_marker = r32.find("StretchRect commands are already queued", resolve_start)
issue_reject = r32.find("R32DirectCopyPathRejected = true;", issue_marker)
if min(issue_marker, issue_reject) < 0 or issue_reject < issue_marker:
    raise SystemExit(
        "R32 EVENT Issue failure must quarantine DirectGPU after queued eye copies")

r33 = require(
    "src/vr/d3d9/stereo_renderer_r33.cpp",
    '#include "stereo_renderer_r32.cpp"',
    "R31OwnedResult R33TryFastWorld",
    "R31OwnedResult R33TryHud",
    "R31ObserveDraw(device)",
    "R32LowerFailClosed",
    "R31DiscardUnreliableDrawCaches();",
    "changing one tracked render state cannot",
    "R30DrawPrimitiveR29Hook",
    "R33ResetR32Hook.stdcall<HRESULT>",
    "R33 -> R32 -> R22",
    "top-level telemetry counted once",
)
if "const HRESULT hr = ResetDestR22" in r33:
    raise SystemExit("R33 must not bypass the corrected R32 Reset lifecycle")

ex = require(
    "src/vr/d3d9/ex_device_upgrade.cpp",
    "#include <algorithm>",
    "#include <utility>",
    "TestCooperativeLevelCompatDest",
    "deviceEx->CheckDeviceState(window)",
    "S_PRESENT_MODE_CHANGED",
    "D3DERR_DEVICENOTRESET",
    "D3DERR_DEVICEHUNG",
    "D3DERR_DEVICEREMOVED",
    "D3DERR_DRIVERINTERNALERROR",
    "EvictManagedResourcesCompatDest",
    "CaptureClassicBaseline",
    "RestoreClassicResetState",
    "fresh-device classic state baseline",
    "for (DWORD stage = 0; stage < 8; ++stage)",
    "fullscreen.ScanLineOrdering = current.ScanLineOrdering",
    "a second promoted game device was requested",
    "bool allowNonDynamicFallback",
    "failing closed hr=0x{:08X}",
    "compatibility hook transaction was partial",
    "const D3DPRESENT_PARAMETERS originalParams = *params",
    "auto classicFallback = [&]() -> HRESULT",
    "D3DCREATE_PUREDEVICE",
    "FinalCompatOverlayReady",
    "final R15 compatibility overlay is not ready",
    "subsequent CreateDevice stays on classic D3D9",
)
if "for (DWORD stage = 0; stage < 16; ++stage)\n                if (FAILED(device->SetTexture" in ex:
    raise SystemExit("classic Reset replay must not treat invalid texture stages 8..15 as failures")

bridge = require(
    "src/vr/d3d9/r13_bridge.hpp",
    "NormalizeLegacyPresentResult",
    "ResetCompatDevice",
)

ex_r13 = require(
    "src/vr/d3d9/ex_device_upgrade_r13.cpp",
    "ResetCompatDevice",
    "deviceEx->ResetEx",
    "UpdateCompatPresentationState(device, params)",
    "RestoreClassicResetState(device)",
    "ResetEx + classic-state replay",
    "NormalizeLegacyPresentResult",
    "S_PRESENT_MODE_CHANGED",
    "S_PRESENT_OCCLUDED",
    "CompatWindowed.load",
    "legacy ResetEx shim retained through final R15 validation",
)
resetex = ex_r13.find("deviceEx->ResetEx")
restore = ex_r13.find("RestoreClassicResetState(device)", resetex)
if resetex < 0 or restore < resetex:
    raise SystemExit("authoritative R13 ResetEx path must replay classic state after successful ResetEx")

ex_r15 = require(
    "src/vr/d3d9/ex_device_upgrade_r15.cpp",
    "SetFinalCompatOverlayReady(false)",
    "SetExternalSafetyBlock(true)",
    "SetExternalSafetyBlock(!healthy)",
    "SetFinalCompatOverlayReady(true)",
    "DisarmLegacyResetHook()",
    "InstallStereoHooksSynchronously(device)",
    "synchronous CreateDevice-thread stereo handoff failed",
    "R14AbandonCompatDevice(device)",
)
if ex_r15.find("DisarmLegacyResetHook()") > ex_r15.find("InstallStereoHooksSynchronously(device)"):
    raise SystemExit("R15 must disarm the temporary Reset shim immediately before synchronous stereo ownership handoff")

stereo_base = require(
    "src/vr/d3d9/stereo_renderer_r7.inc",
    "#include <mutex>",
    "StereoBaseCallbacksArmed",
    "InlineHook::StartDisabled",
    "disabled-first callbacks armed atomically at the behavior boundary",
    "std::mutex StereoInstallMutex",
    "std::lock_guard<std::mutex> installLock",
    "InstallStereoHooksSynchronously",
    "synchronous CreateDevice-thread base hook handoff READY",
)
worker_start = stereo_base.find("DWORD WINAPI StereoInstallThread")
worker_ready = stereo_base.find("StereoInstallState.load(std::memory_order_acquire)==StereoInstallReady", worker_start)
worker_device = stereo_base.find("Game::D3DDevice_ptr&&*Game::D3DDevice_ptr", worker_start)
if min(worker_start, worker_ready, worker_device) < 0 or worker_ready > worker_device:
    raise SystemExit("stereo worker must exit for synchronously installed Ex devices before touching the game D3D device")

r9 = require(
    "src/vr/d3d9/stereo_renderer.cpp",
    "rawPresentHr = PresentHook.stdcall<HRESULT>",
    "OutRunVRD3D9ExUpgradeR13::NormalizeLegacyPresentResult",
    "if (composedStereo && SUCCEEDED(hr) && !FrameStereoIncomplete)",
)
raw_present = r9.find("rawPresentHr = PresentHook.stdcall<HRESULT>")
normalized_present = r9.find("NormalizeLegacyPresentResult", raw_present)
present_success = r9.find("if (composedStereo && SUCCEEDED(hr)", normalized_present)
if min(raw_present, normalized_present, present_success) < 0 or not (
        raw_present < normalized_present < present_success):
    raise SystemExit(
        "legacy Present normalization must happen before stereo success/publication decisions")

require(
    "vrhost/src/runtime/d3d9ex_direct_passthrough_r32.hpp",
    "R32SharedSlotCache",
    "R32OpenSharedSlot",
    "R32InactiveEye",
    "R32SafeTimeoutPreserves",
    "queued copy cannot mutate SafeFrameId image identity",
    "EnsureSafeFrameR32",
)

host_direct = require(
    "vrhost/src/runtime/r32_direct_submit.hpp",
    "CanFastSubmit",
    "AckFaultGeneration",
    "Completion is unknowable",
    "SafeEye fallback perform a separately fenced copy/ACK",
    "ProjectionMatchesSnapshot",
    "ArmConsumptionFence",
    "D3D11_ASYNC_GETDATA_DONOTFLUSH",
    "flushIssued",
    "deferred until actual direct-ring slot pressure",
    "PublishCompletedFrame",
    "AckedFrame",
    "AckedGeneration",
    "R32 direct PERF 5s",
    "verified incoming DirectGPU projection submitted once",
    "OutRunVrR26RecenterHardening::EndFrame",
)
# Flush remains permitted only in the slot-pressure escalation block. Reject the
# old unconditional End(query)+Flush() sequence.
if "Context->End(pending.fence);\n        OutRunVrFinalTest::Context->Flush();" in host_direct:
    raise SystemExit("R32 host must not Flush every direct frame")
query_error = host_direct.find("if (FAILED(hr))")
fault_generation = host_direct.find("AckFaultGeneration = generation", query_error)
fast_gate = host_direct.find("AckFaultGeneration == generation")
if min(query_error, fault_generation, fast_gate) < 0:
    raise SystemExit("R32 host ACK query failure must disable fast-submit for that generation")
if not (query_error < fault_generation < fast_gate):
    raise SystemExit("R32 host ACK fault must be recorded before the fast-submit generation gate")

r34 = require(
    "src/vr/d3d9/stereo_renderer_r34.cpp",
    "SetExternalSafetyBlock(true)",
    "SetExternalSafetyBlock(!healthy)",
    "Install/state-sync",
    "Present/pre",
)
if r34.find("Present/pre") > r34.find("R34PresentR33Hook.stdcall<HRESULT>"):
    raise SystemExit("R34 must reassert Reset replay fail-close before lower Present work")

r31 = load("src/vr/d3d9/stereo_renderer_r31.cpp")
state_enable = r31.find("const bool stateHooks =")
end_enable = r31.find("R31EndStateBlockHook.enable()", state_enable)
begin_enable = r31.find("R31BeginStateBlockHook.enable()", state_enable)
create_enable = r31.find("R31CreateStateBlockHook.enable()", state_enable)
if min(state_enable, end_enable, begin_enable, create_enable) < 0 or         not (end_enable < begin_enable < create_enable):
    raise SystemExit("R31 StateBlock hooks must arm End before Begin before Create")

host_main = require(
    "vrhost/src/main.cpp",
    "PrepareDirectStereoSource",
    "CommitDirectStereoSource",
    "legacy private snapshot + synchronous fence",
)
prepare_direct = host_main.find("bool PrepareDirectStereoSource")
legacy_commit = host_main.find("bool CommitDirectStereoSource", prepare_direct)
snapshot_use = host_main.find("directSnapshotLeft_", prepare_direct)
if min(prepare_direct, legacy_commit, snapshot_use) < 0 or snapshot_use < legacy_commit:
    raise SystemExit(
        "production DirectGPU prepare path must not execute the legacy private snapshot/fence")

host_r23 = require(
    "vrhost/src/main_r23.cpp",
    "R23DirectHoldState",
    "R23StageDirectHold",
    "PrepareDirectStereoSource",
    "single-copy production path active",
    "srv[0] = R23DirectHold.srv[0]",
)
if "c.CommitDirectStereoSource(frame)" in host_r23:
    raise SystemExit(
        "R23 production DirectGPU path must bypass legacy snapshot/fence commit")

r14 = require(
    "src/vr/d3d9/ex_device_upgrade_r14.cpp",
    "singleLevelTexture",
    "entry.gpu->GetLevelCount() <= 1",
    "R14EnsureSurfaceHooks",
    "SurfaceLockRectDestR14",
    "SurfaceGetDCDestR14",
    "StretchRectDestR14",
    "ColorFillDestR14",
    "SUCCEEDED(hr) && destination && R14InternalUploadDepth == 0",
    "keeping translated MANAGED texture on tracked DirectOnly compatibility path",
    "hard fail-close retained because DirectOnly lifetime cannot be proven",
    "R14TrackDirectOnly",
    "return D3DERR_NOTAVAILABLE;",
)
if "external GetSurfaceLevel" in r14:
    raise SystemExit("R14 must not retire the CPU shadow merely because GetSurfaceLevel borrowed a read-only alias")
create_r14 = r14.find("HRESULT __stdcall CreateTextureCompatDestR14")
direct_r14 = r14.find("R14TrackDirectOnly(device, *texture)", create_r14)
hard_fail_r14 = r14.find("return D3DERR_NOTAVAILABLE;", direct_r14)
if min(create_r14, direct_r14, hard_fail_r14) < 0 or \
        not (create_r14 < direct_r14 < hard_fail_r14):
    raise SystemExit(
        "R14 must degrade CPU-shadow allocation failure to tracked DirectOnly before the hard coverage fail-close")

require(
    "vrhost/tests/runtime_eligibility_smoke.cpp",
    "SetExternalSafetyBlock(true)",
    "ObserveSoftHostSuspend()",
    "assert(!HostRenderable.load());",
    "BaselineVerified();",
    "assert(!MayInjectStereo());",
)

require(
    "src/vr/d3d9/stereo_renderer_r21.cpp",
    "R21HostStatus::SoftSuspend",
    "VR R21 SOFT-SUSPEND",
    "without baseline reset",
)

require(
    "src/vr/d3d9/stereo_renderer_r23.cpp",
    "SetRenderTarget implicit viewport/scissor transition observed",
    "R23/SetRenderTarget/live-state-capture",
)

require(
    "src/vr/d3d9/stereo_renderer.cpp",
    "R9LastMainDepthRestoreLogMs",
    "totalRestores={}",
)

effect_policy = require(
    "src/vr/d3d9/vr_pass_policy.hpp",
    "Depth testing is the decisive",
    "if (!depthTestEnabled)",
    "return EffectStereoPolicy::ZeroDisparity;",
    "ClassifyEffectStereo(true, false, false, false, true)",
    "EffectStereoPolicy::ZeroDisparity",
)
if "cullNone && alphaBlendEnabled && !depthWriteEnabled;" in effect_policy:
    raise SystemExit(
        "effect policy regressed to zero-disparity classification without the depth-test signal")

r31_lazy = require(
    "src/vr/d3d9/stereo_renderer_r31.cpp",
    "R31StateBlockResyncPending",
    "R31MarkStateBlockCachesDirty",
    "R31FlushPendingStateBlockResync",
    "lazily re-primed at the next actual draw",
)
r33_lazy = require(
    "src/vr/d3d9/stereo_renderer_r33.cpp",
    "R31FlushPendingStateBlockResync(device);",
)

require(
    "vrhost/tests/r32_policy_smoke.cpp",
    "RearmMonoSafetyEpoch(1) == 3",
    "EffectSnapshotDecision::ForceZeroDisparity",
    "ProducerFenceBudgetMs <= 2",
    "PendingFenceDecision::BlockReuse",
    "PendingFenceDecision::QueryError",
)

cmake = require(
    "cmake.toml",
    "R33/R14 keep the validated",
    "stereo_renderer_r31.cpp",
    "stereo_renderer_r32.cpp",
    "PROPERTIES HEADER_FILE_ONLY TRUE",
    "OUTRUN_VR_POST_REVIEW_FINAL_TUS",
    "ex_device_upgrade_r15.cpp",
    "stereo_renderer_r34.cpp",
)
included_start = cmake.find("set(OUTRUN_VR_INCLUDED_IMPL_TUS")
included_end = cmake.find(
    "set_source_files_properties(${OUTRUN_VR_INCLUDED_IMPL_TUS}",
    included_start)
final_start = cmake.find("set(OUTRUN_VR_FINAL_TUS", included_end)
final_end = cmake.find("foreach(_vr_source", final_start)
if min(included_start, included_end, final_start, final_end) < 0:
    raise SystemExit("could not locate list-based VR source ownership blocks")
header_section = cmake[included_start:included_end]
final_section = cmake[final_start:final_end]
if "stereo_renderer_r31.cpp" not in header_section or \
        "stereo_renderer_r32.cpp" not in header_section:
    raise SystemExit("R31/R32 must be include-only implementation TUs")
if "stereo_renderer_r33.cpp" in header_section:
    raise SystemExit("R33 final TU must not be include-only")
if "stereo_renderer_r33.cpp" not in final_section or \
        "ex_device_upgrade_r14.cpp" not in final_section or \
        "outrun_renderer_r29.cpp" not in final_section:
    raise SystemExit("final VR wrapper TU list is incomplete")

generated = require(
    "CMakeLists.txt",
    "set(OUTRUN_VR_INCLUDED_IMPL_TUS",
    "set(OUTRUN_VR_FINAL_TUS",
    "stereo_renderer_r33.cpp",
    "ex_device_upgrade_r14.cpp",
    "outrun_renderer_r29.cpp",
    "ex_device_upgrade_r15.cpp",
    "stereo_renderer_r34.cpp",
)
for source in (
    "src/vr/d3d9/stereo_renderer_r31.cpp",
    "src/vr/d3d9/stereo_renderer_r32.cpp",
    "src/vr/d3d9/stereo_renderer_r33.cpp",
    "src/vr/d3d9/ex_device_upgrade_r14.cpp",
    "src/vr/game/outrun_renderer_r29.cpp",
):
    if source not in generated:
        raise SystemExit(f"generated CMakeLists is stale: missing {source}")

source_list_start = generated.find("set(outrun2006tweaks_SOURCES")
source_list_end = generated.find(
    ")\n\nadd_library(outrun2006tweaks SHARED)", source_list_start)
if source_list_start < 0 or source_list_end < 0:
    raise SystemExit("could not locate generated outrun2006tweaks target source list")
target_sources = generated[source_list_start:source_list_end]
for source in (
    "src/vr/d3d9/ex_device_upgrade_r14.cpp",
    "src/vr/d3d9/ex_device_upgrade_r15.cpp",
    "src/vr/d3d9/stereo_renderer_r20.cpp",
    "src/vr/d3d9/stereo_renderer_r21.cpp",
    "src/vr/d3d9/stereo_renderer_r22.cpp",
    "src/vr/d3d9/stereo_renderer_r23.cpp",
    "src/vr/d3d9/stereo_renderer_r26.cpp",
    "src/vr/d3d9/stereo_renderer_r29.cpp",
    "src/vr/d3d9/stereo_renderer_r30.cpp",
    "src/vr/d3d9/stereo_renderer_r31.cpp",
    "src/vr/d3d9/stereo_renderer_r32.cpp",
    "src/vr/d3d9/stereo_renderer_r33.cpp",
    "src/vr/d3d9/stereo_renderer_r34.cpp",
    "src/vr/game/outrun_renderer_r23.cpp",
    "src/vr/game/outrun_renderer_r29.cpp",
):
    if source not in target_sources:
        raise SystemExit(
            f"generated target source list is stale: missing {source}")

host_cmake = require(
    "vrhost/CMakeLists.txt",
    "d3d9ex_direct_passthrough_r32.hpp",
    "r32_direct_submit.hpp",
    "outrun-vr-r32-policy-smoke",
)
if host_cmake.find("d3d9ex_direct_passthrough_r32.hpp") > \
        host_cmake.find("r22_runtime_hardening.hpp"):
    raise SystemExit("R32 SafeEye A/B override must precede R22/R23/R24")
if host_cmake.find("r32_direct_submit.hpp") < \
        host_cmake.find("r26_recenter_hardening.hpp"):
    raise SystemExit("R32 direct submit must be final xrEndFrame owner after R26")

print("R32-R34 + R15 D3D9Ex compatibility/Present verification passed")
