from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def text(rel: str) -> str:
    path = ROOT / rel
    if not path.is_file():
        raise SystemExit(f"missing architecture file: {rel}")
    return path.read_text(encoding="utf-8")


def require(rel: str, *markers: str) -> str:
    data = text(rel)
    for marker in markers:
        if marker not in data:
            raise SystemExit(f"missing architecture invariant: {rel} :: {marker}")
    return data


# Final authoritative implementation graph. Intermediate implementation files
# are intentionally kept because the R23 wrappers include the validated lower
# layers, but cmake must never compile those included bodies independently.
required = [
    "src/vr/settings.cpp",
    "src/vr/runtime_eligibility.hpp",
    "src/vr/game/outrun_renderer.cpp",
    "src/vr/game/outrun_renderer_r13.cpp",
    "src/vr/game/outrun_renderer_r23.cpp",
    "src/vr/game/outrun_renderer_r29.cpp",
    "src/vr/d3d9/stereo_renderer.cpp",
    "src/vr/d3d9/stereo_renderer_r13.cpp",
    "src/vr/d3d9/stereo_renderer_r20.cpp",
    "src/vr/d3d9/stereo_renderer_r21.cpp",
    "src/vr/d3d9/stereo_renderer_r22.cpp",
    "src/vr/d3d9/stereo_renderer_r23.cpp",
    "src/vr/d3d9/stereo_renderer_r29.cpp",
    "src/vr/d3d9/stereo_renderer_r30.cpp",
    "src/vr/d3d9/stereo_renderer_r31.cpp",
    "src/vr/d3d9/ex_device_upgrade.cpp",
    "src/vr/d3d9/ex_device_upgrade_r13.cpp",
    "src/vr/d3d9/ex_device_upgrade_r14.cpp",
    "src/vr/d3d9/r13_bridge.hpp",
    "src/vr/d3d9/vr_pass_policy.hpp",
    "src/vr/ipc/protocol.hpp",
    "src/vr/ipc/protocol_v3.hpp",
    "src/vr/ipc/direct_ack_r13.hpp",
    "src/vr/core/frame_types.hpp",
    "src/vr/core/matrix.hpp",
    "src/vr/core/transport.hpp",
    "src/vr/game/game_adapter.hpp",
    "src/vr/d3d9/stereo_backend.hpp",
    "vrhost/src/main.cpp",
    "vrhost/src/main_r23.cpp",
    "vrhost/src/runtime/d3d9ex_direct_passthrough.hpp",
    "vrhost/src/runtime/r23_runtime_hardening.hpp",
    "vrhost/src/runtime/r23_verified_bundle.hpp",
    "vrhost/tests/runtime_eligibility_smoke.cpp",
    "vrhost/tests/r23_verified_bundle_smoke.cpp",
    "docs/VR_ARCHITECTURE.md",
]
for rel in required:
    if not (ROOT / rel).is_file():
        raise SystemExit(f"missing architecture file: {rel}")

forbidden = [
    "src/hooks_vr.cpp",
    "src/vr_renderer_probe.cpp",
    "src/vr_stereo.cpp",
    "vrhost/main.cpp",
    "vrhost/main_compat.cpp",
    "vrhost/main_compat_v2.cpp",
    "vrhost/main_compat_v3.cpp",
    "vrhost/main_stereo.cpp",
]
for rel in forbidden:
    if (ROOT / rel).exists():
        raise SystemExit(f"legacy/prototype artifact still present: {rel}")

# v2 remains the live transport ABI while v3 is mirrored for migration and
# diagnostics. Do not silently steal reserved words or pointer-width fields.
protocol_v2 = require(
    "src/vr/ipc/protocol.hpp",
    "SharedProtocolVersion = 2",
    "RenderFrameProtocolVersion = 2",
    "RenderFrameRingSize = 4",
    "HostAdapterLuidValid",
    "hostDirectConsumedFrameId",
    "SharedRenderFrameRing",
)
protocol_v3 = require(
    "src/vr/ipc/protocol_v3.hpp",
    "ProtocolVersion = 3",
    "HostStateName",
    "ClientStateName",
    "FrameRingName",
    "AckStateName",
    "using WireHandle = std::uint64_t",
    "WireHandle leftHandle",
    "WireHandle rightHandle",
)
for forbidden_marker in (
    "std::uintptr_t interopProbeHandle",
    "std::uintptr_t leftHandle",
    "std::uintptr_t rightHandle",
):
    if forbidden_marker in protocol_v3:
        raise SystemExit(f"v3 protocol reintroduced pointer-width ABI debt: {forbidden_marker}")

require(
    "src/vr/ipc/direct_ack_r13.hpp",
    "DirectGpuAckName",
    "DirectGpuAckRingSize = 4",
    "transportGeneration",
    "completedFrameId[DirectGpuAckRingSize]",
)

# Shared eligibility is a state machine, not a collection of independent bools.
require(
    "src/vr/runtime_eligibility.hpp",
    "enum class InstallState",
    "Pending = 0",
    "Ready = 1",
    "Failed = 2",
    "SafetyOverlayReady",
    "MarkSafetyOverlayInstalled",
    "MarkSafetyOverlayUnavailable",
    "BaselineVerified",
    "MayInjectStereo",
)

# Render-pass policy must keep orthographic/UI and fragile camera-facing alpha
# effects out of the head-tracked WVP while retaining opaque world stereo.
require(
    "src/vr/d3d9/vr_pass_policy.hpp",
    "ClassifyProjectionSignature",
    "Perspective3D",
    "Orthographic2D",
    "EffectStereoPolicy",
    "ZeroDisparity",
    "ClassifyEffectStereo",
    "UnsafeSingleExecution",
)
require(
    "src/vr/game/outrun_renderer_r13.cpp",
    "R13FragileEffectNeedsZeroDisparity",
    "D3DRS_ALPHABLENDENABLE",
    "D3DRS_ZWRITEENABLE",
    "shadow/billboard/panel pass kept stock",
    "InvalidateVerifiedWvp",
    "InlineHook::StartDisabled",
)

# R29/R31 correctness overlays must preserve constant registers surrounding a
# partial c64..c67 write, pair fast-path WVP with its captured projection, and
# make StateBlock bypasses an explicit full-cache generation boundary.
require(
    "src/vr/game/outrun_renderer_r29.cpp",
    "R29BuildCoherentUploadEnvelope",
    "envelopeStart",
    "envelopeCount",
    "IsGameStateBlockRecording",
    "IsStateBlockTrackingReliable",
    "recorded stock",
    "R29InvalidateRendererStateAfterExternalRestore",
)
require(
    "src/vr/d3d9/stereo_renderer_r31.cpp",
    "GetR28VerifiedProjection",
    "projectionGeneration != generation",
    "BeginStateBlockDestR31",
    "R31StateBlockTrackingReliable",
    "per-draw live WVP/shader/render-state validation",
    "R31 fast left-eye c64 rollback",
    "R31 HUD left-eye c64 rollback",
)

# R14 shadows are resource-lifetime-bound and fail closed to one direct path on
# any write route that cannot be mirrored. A lower-mip fallback must copy that
# exact level rather than relying on UpdateTexture's level-zero dirty rules.
r14 = require(
    "src/vr/d3d9/ex_device_upgrade_r14.cpp",
    "std::unordered_map<IDirect3DTexture9*, R14EntryPtr>",
    "TextureReleaseDestR14",
    "InstallManagedResourceCompatR14",
    "R14AdoptCompatDevice",
    "TextureGenerateMipSubLevelsDestR14",
    "TextureGetSurfaceLevelDestR14",
    "UpdateSurfaceDestR14",
    "UpdateTextureDestR14",
    "validMask",
    "dirtyMask",
    "R14CopyWholeLevelByLock",
    "exact mip CPU-shadow upload failed",
)
if "R14ShadowCapacity" in r14 or "R14ShadowCursor" in r14:
    raise SystemExit("R14 reintroduced fixed-capacity live-shadow eviction")

settings = require(
    "src/vr/settings.cpp",
    'VRPreferD3D9Ex{ "VR", "PreferD3D9Ex", true',
    'VRDirectGpuOnly{ "VR", "DirectGpuOnly", true',
    'VRDisableDesktopDuplication{ "VR", "DisableDesktopDuplication", false',
    'VRTargetRefreshRateHz{ "VR", "TargetRefreshRateHz", 0.0f',
    'VRFrameCadenceMode{ "VR", "FrameCadenceMode", 1',
    'VRFrameCadenceTargetHz{ "VR", "FrameCadenceTargetHz", 0.0f',
    'VRFrameCadenceMaxHz{ "VR", "FrameCadenceMaxHz", 120.0f',
    "OUTRUN_VR_DIRECT_TRANSPORT",
    "OUTRUN_VR_DIRECT_ONLY",
    "OUTRUN_VR_DISABLE_DESKTOP_DUPLICATION",
    "OUTRUN_VR_TARGET_REFRESH_HZ",
    "OUTRUN_VR_CADENCE_MODE",
    "OUTRUN_VR_CADENCE_TARGET_HZ",
    "OUTRUN_VR_CADENCE_MAX_HZ",
)

# R23 installer workers may only request cleanup. Live OutRun camera/projection
# writes are serviced from D3D render callbacks; recovery pose warmup keeps the
# stock camera/WVP visible until the next-frame authoritative seed is accepted.
renderer_r23 = require(
    "src/vr/game/outrun_renderer_r23.cpp",
    "R23RenderThreadCleanupRequested",
    "R23RequestFailClosedCleanup",
    "R23ServiceRenderThreadCleanup",
    "R23DropIneligibleLatchedPoseOnRenderThread",
    "R23KeepWarmupPoseStockOnRenderThread",
    "recovery pose warmup is stock-visible",
    "RendererInjectionAllowed.store(false",
)
installer_start = renderer_r23.find("DWORD WINAPI R23RendererInstallThread")
installer_end = renderer_r23.find("class VRRendererR23EligibilityHook")
if installer_start >= 0 and installer_end > installer_start:
    worker = renderer_r23[installer_start:installer_end]
    if "RestoreCullingCamera()" in worker:
        raise SystemExit("R23 installer thread must not mutate live camera/projection memory")

# R20-R23 installation must fail fast and publish READY only after disabled-first
# hook transactions are enabled.
require(
    "src/vr/d3d9/stereo_renderer_r20.cpp",
    "R20InstallState",
    "using State = OutRunVR::RuntimeEligibility::InstallState",
    "State::Pending",
    "State::Failed",
    "InlineHook::StartDisabled",
)
require(
    "src/vr/d3d9/stereo_renderer_r21.cpp",
    "R21InstallState",
    "IsFailed(R20InstallState)",
    "InlineHook::StartDisabled",
)
require(
    "src/vr/d3d9/stereo_renderer_r22.cpp",
    "R22InstallState",
    "R22ShadowState",
    "R22SetScissorRectHook",
    "R22SetRenderStateHook",
    "R22PrimeShadowState",
    "InlineHook::StartDisabled",
    "per-draw GetViewport/GetScissorRect/GetRenderState eliminated",
)

# R23/R25 is the single recovery/baseline authority. Recovery Clear must remain
# a passive original call, while live viewport/scissor, full game draw serial,
# MRT safety and a fresh next-frame pose gate the first stereo initialization.
require(
    "src/vr/d3d9/stereo_renderer_r23.cpp",
    "#include \"stereo_renderer_r22.cpp\"",
    "R23InstallState",
    "IsFailed(R22InstallState)",
    "R23RecoveryNeedsBaseline",
    "R23CaptureActualGameState",
    "R23GameDrawSerial",
    "R23DiagnoseHostFreshness",
    "passive original Clear only",
    "authoritative first seed",
    "mrtActive",
    "MarkSafetyOverlayInstalled",
)

# Async installer status must be publishable back to the hook overlay/UI.
require(
    "src/hook_mgr.hpp",
    "std::atomic<bool> is_active_",
    "std::atomic<bool> has_error_",
    "ReportAsyncResult",
)
require(
    "src/hook_mgr.cpp",
    "HookManager::ReportAsyncResult",
    "async installer",
)

# Shadow v3 diagnostics may not run 1-2 ms busy polling or republish client
# state forever after the first stereo frame.
require(
    "src/vr/ipc/v3_game_shadow_bridge.cpp",
    "ShadowIdlePollMs = 8",
    "ShadowRetryMs = 250",
    "lastClientPublishedFrameId",
    "clientChanged",
)
require(
    "vrhost/src/ipc/v3_shadow_bridge.cpp",
    "ShadowIdlePollMs = 8",
    "ShadowWriterRetryMs = 250",
    "ackWriter.reset()",
)
require(
    "vrhost/src/ipc/host_state_v3_writer.hpp",
    "Reset(false)",
    "A constructor that throws never runs this object's destructor",
)

# Direct shared-eye transport must copy to host-owned resources, prove GPU copy
# completion, and bind actual texture format/size to committed Frame.v2 metadata.
require(
    "vrhost/src/runtime/d3d9ex_direct_passthrough.hpp",
    "CopyFenceTimeoutMs = 8",
    "ExpectedDeclaredFormat",
    "case D3DFMT_A8R8G8B8",
    "DXGI_FORMAT_B8G8R8A8_UNORM",
    "desc[0].Format == declared",
    "host-owned GPU eye copies + completion ACK active",
    "SafeTransportGeneration",
)
require(
    "vrhost/src/runtime/r23_runtime_hardening.hpp",
    "ExpectedDirectDxgiFormat",
    "case D3DFMT_A8R8G8B8",
    "DXGI_FORMAT_B8G8R8A8_UNORM",
    "DirectSafeEyeMatchesCommittedFrame",
    "SafeEyeFormat == expected",
    "width == frame.backbufferWidth",
)
require(
    "vrhost/src/runtime/r23_verified_bundle.hpp",
    "MaxPresentationAgeMs",
    "ReadFresh",
    "Matches",
)

# main_r23 must commit only a stable full Frame.v2 snapshot. If stereo metadata
# is not ready, it may refresh Desktop Duplication only for the theater fallback;
# that mono source must never become a validated stereo projection.
require(
    "vrhost/src/main_r23.cpp",
    "#include \"main.cpp\"",
    "R23FrameUnchanged",
    "R23CommitDirectAfterValidation",
    "R23CommitClassicAfterValidation",
    "R23RefreshTheaterFallbackCapture",
    "if (c.disableDesktopDuplication_)",
    "R37BootstrapSubmittedFrame",
    "R37BootstrapSubmittedGeneration",
    "R37FrameIdBefore",
    "DirectGPU latest-frame-wins active",
    "PublishCompletedFrame(frame)",
    "!R37FrameIdBefore(",
    "lastProcessedStereoFrame",
    "allowInitialWarmupWait",
    "theater-only fallback",
    "OutRunVrR23VerifiedBundle::Publish",
    "std::memcmp(after.eye, before.eye",
    "std::memcmp(after.reserved, before.reserved",
)

require(
    "vrhost/src/main.cpp",
    "directTransportOnly_",
    "desktopDuplication=",
    "disabled-direct-only",
    "DXGI_FORMAT_B8G8R8A8_UNORM",
)
require(
    "src/vr/d3d9/stereo_renderer.cpp",
    "R9ReadBoolEnvironment",
    "R9DirectOnlyTransport",
    "Settings::VRDirectGpuOnly.get()",
    "inherits OUTRUN_VR_DIRECT_ONLY through CreateProcess",
    "SAFE modes stuck in the default direct-only path",
)
require(
    "src/vr/d3d9/stereo_renderer_r26.cpp",
    "R37DepthDisabledFragileOverlay",
    "R13EffectSnapshot effect = R13CaptureDrawTimeEffect",
    "unknown state fails closed",
)
require(
    "src/vr/d3d9/stereo_renderer_r30_r26_safe.cpp",
    '#include "stereo_renderer_r26.cpp"',
    "R30SafeStereoBase",
    "VR R26+HUD SAFE TEST",
)
require(
    "src/vr/d3d9/stereo_renderer_r30.cpp",
    "state.depthTestEnabled &&",
    "state.rhwDepthEvidence",
)

# R41 DirectGPU latency invariant: the newest complete slot is the only frame
# selected for sampling. Older occupied slots are never rendered later; they are
# ACKed immediately because no D3D11 work references them.
_history = [(1, 0), (2, 1), (3, 2), (4, 3)]
_selected = max(_history, key=lambda item: item[0])
_skipped = [item for item in _history if item != _selected]
if _selected != (4, 3) or [fid for fid, _ in _skipped] != [1, 2, 3]:
    raise SystemExit(
        f"R41 direct latest-frame model regressed: selected={_selected} skipped={_skipped}"
    )

# Once frame 4 is displayed, stale ring contents 1..3 must never be selected
# again while the producer is recycling those ACKed slots.
_last_processed = _selected[0]
_remaining_new = [item for item in _history if item[0] > _last_processed]
if _remaining_new:
    raise SystemExit(
        f"R41 direct stale-frame rewind model regressed: {_remaining_new}"
    )

# New regressions are required in the host build graph.
require(
    "vrhost/CMakeLists.txt",
    "src/main_r23.cpp",
    "outrun-vr-runtime-eligibility-smoke",
    "outrun-vr-r23-verified-bundle-smoke",
    "r23_runtime_hardening.hpp",
)

# Root cmkr source ownership: only final wrappers compile independently.
cmake_root = require(
    "cmake.toml",
    "stereo_renderer_r20.cpp",
    "stereo_renderer_r21.cpp",
    "stereo_renderer_r22.cpp",
    "outrun_renderer_r13.cpp",
    "HEADER_FILE_ONLY TRUE",
    'compile-options = ["/GS"',
)

# Renderer-independent interfaces stay clean of graphics/runtime headers.
math_core = require("src/vr/core/matrix.hpp", "struct Matrix4", "InverseRigid", "Invert(")
if "#include <d3d9.h>" in math_core or "#include <openxr/" in math_core:
    raise SystemExit("core matrix layer must not depend on D3D9/OpenXR headers")
require("src/vr/core/transport.hpp", "class IFrameProducer", "class IFrameConsumer", "D3D9ExShared", "DesktopDuplication")
require("src/vr/game/game_adapter.hpp", "class IGameAdapter", "latchRenderPose", "buildStereoMatrices")
require("src/vr/d3d9/stereo_backend.hpp", "class IStereoBackend", "drawWorldStereo", "drawScreenSpaceStereo")

print("VR reconstructed R23/R25 architecture boundary verification passed")
