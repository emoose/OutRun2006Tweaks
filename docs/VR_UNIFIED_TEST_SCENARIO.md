# OutRun 2006 VR Unified Backend Test Scenario

Branch: `vr-unified-backends`

This package is a test integration harness for three renderer paths. It intentionally switches complete backend payloads before game launch instead of stacking mutually exclusive D3D9 hooks in one process.

## Backend activation

From the game directory:

```bat
Select-OutRunVRBackend.cmd d3d9
Select-OutRunVRBackend.cmd dxvk
Select-OutRunVRBackend.cmd dx12
```

Always exit the game and `outrun-vr-host.exe` before switching.

Selecting a mode creates a unique session identity and a config snapshot **before game launch**. Existing root logs are sealed into the previous session before the new session starts, so game-side log truncate/overwrite behavior cannot destroy an already sealed test.

**You do not run the collector before a test.** The easiest path is the GUI selector: choose a backend once, then click **현재 모드 테스트 실행 (종료 후 로그 자동수집)**. This launches `Run-OutRunVRTest.cmd`, starts the game with a clean session, waits for the game to exit, collects the finished logs, removes the sealed root copies, and immediately prepares the next session for another run of the same backend.

For manual launches, run `Collect-OutRunVRLogs.cmd` **after** each game/host test has ended. The collector now prepares the next session automatically. If you switch backends without collecting first, the selector preserves pending root logs in the previous session before switching. Run `Collect-OutRunVRLogs.cmd -All` at the end to create the matrix ZIP.

Do not launch the game twice manually without either collecting between runs or using `Run-OutRunVRTest.cmd`; a game that truncates the same filename can overwrite an unsealed run before any tool has a chance to preserve it.

## Recommended launch flow

1. Start `OutRunVR-Backend-Selector.cmd`.
2. Pick the backend you want to test.
3. Click **현재 모드 테스트 실행 (종료 후 로그 자동수집)**.
4. Test normally and exit the game.
5. If `outrun-vr-host.exe` also exits within 15 seconds, the launcher automatically creates the session ZIP and prepares the next session.
6. If the host remains open, close it and run `Collect-OutRunVRLogs.cmd` once; the launcher deliberately leaves the root logs untouched in that case.

The collector does not need to remain running in the background.

## Recovery checkpoint: 2026-09-20 stereo transport / DX12 device creation

This package must include the transport fixes from all backend source branches before it is considered testable:

- D3D9 SAFE: game-side R9 transport policy follows the parsed `DirectGpuOnly` setting directly. With SAFE defaults (`DirectGpuOnly=false`), classic SBS/Desktop Duplication fallback must not be suppressed by a stale process environment value.
- DXVK SAFE/MULTIVIEW: the DXVK game branch carries the same R9 transport-policy fix; do not accept a package whose DXVK source SHA predates that branch fix.
- All D3D11 OpenXR hosts: when no completed SBS frame is published, recovery theater uses the full-width mono source. Left-half cropping is permitted only for a frame explicitly marked `StereoSbsActive` + `RenderFrameStereoComplete`.
- DX12 STRICT: use D3D9On12 legacy `CreateDevice` semantics first. If the runtime rejects OutRun's original presentation values, retry with the bounded compatibility normalization (window handle/refresh, one back buffer, default interval) while remaining on D3D9On12; native D3D9 fallback stays disabled.

For the next retest, success criteria are intentionally narrow: D3D9 SAFE must publish a completed stereo frame instead of remaining at `composeOk=0`; DXVK SAFE must do the same; DX12 STRICT must at least pass device creation before any later renderer/VR judgement.

## Recommended short evening test order

Use 2D ORIGINAL only as a quick no-VR overhead smoke check. Then compare D3D9 control first, followed by DXVK SAFE. Test DXVK MULTIVIEW or DX12 STRICT only if the earlier modes start cleanly. A full A-F sequence is not required; this package contains no fabricated B/C/D variants.

### A. D3D9 safe baseline
1. Run `Select-OutRunVRBackend.cmd d3d9`.
2. Confirm no local `d3d9.dll` remains.
3. Start Virtual Desktop / VDXR first, then OutRun.
4. Menu: recenter once, rotate and translate head, enter car selection.
5. Gameplay: run a short Mission and then OutRun mode.
6. Check: world stereo, HUD alignment, white rank/score text, sky/cloud head-lock behavior, smoke/skid placement, opponent rank labels, 6th/6 position, Yes/No dialog.
7. Record HMD refresh rate and FPS.
8. Save `OutRun2006Tweaks.log`, `outrun-vr-host-v3*.log`, `outrun-vr-host-pipeline*.log`, and watchdog log.

### E1. DXVK safe
1. Exit all OutRun/host processes.
2. Run `Select-OutRunVRBackend.cmd dxvk-safe`.
3. Confirm root contains `d3d9.dll` but not `multiviewpatcher.dll`.
4. Repeat the exact A scenario and collect the session ZIP.

### E2. DXVK multiview
1. Exit all OutRun/host processes.
2. Run `Select-OutRunVRBackend.cmd dxvk`.
3. Confirm root contains `d3d9.dll` and `multiviewpatcher.dll`.
4. Repeat the exact A scenario.
5. Confirm log contains `VR DXVK PROBE:`, `VR DXVK MULTIVIEW:`, and when eligible `TRUE MULTIVIEW world rendering active`.
6. Compare HMD smoothness and frame time against A. Do not judge only by monitor output.

### F. DX12 strict D3D9On12
1. Exit all OutRun/host processes.
2. Run `Select-OutRunVRBackend.cmd dx12`.
3. Confirm root `d3d9.dll` is absent.
4. Start VDXR then OutRun.
5. If startup fails, stop there and keep the log; strict mode intentionally does not silently fall back to D3D9/DXVK.
6. If it starts, repeat A and verify DX12 log markers including D3D9On12 / D3D12 host initialization.

## Pass/fail sheet

Record for each backend:
- game reaches menu
- menu recenter works
- car-selection model intact
- gameplay reaches first stage
- left/right world geometry agrees
- HUD not split
- white score/rank/Yes-No not split
- sky/cloud stays world-fixed
- smoke/skid placement correct
- opponent rank labels follow cars
- 6th/6 moves to intended VR HUD placement
- no black/loading-screen recenter regression
- headset FPS / refresh
- subjective stutter: none / mild / severe
- monitor mirror mode
- final log filenames

Do not switch backends while either the game or host is running.
