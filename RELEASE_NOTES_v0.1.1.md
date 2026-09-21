# OutRun2006Tweaks Wheel FFB v0.1.1

[English](#english) | [한국어](#한국어)

<a id="english"></a>
## English

v0.1.1 is a compatibility and stability update to the first Wheel FFB release. It keeps the same Modern / Physics SAT direction of the project and focuses on making the existing force-feedback implementation work more reliably across real DirectInput wheel-driver layouts.

**X-Force is not included in this release.** The experimental X-Force work remains separate; v0.1.1 uses the project's own Physics SAT / Natural SAT force path.

### Changes since v0.1

- Hardened DirectInput FFB selection for wheelbases that expose multiple Windows interfaces.
- Added exhaustive compatible-interface probing instead of assuming the first matching interface is usable for force feedback.
- Enumerates and probes every reported force-actuator axis, with validated ConstantForce creation and live zero-force update testing.
- Improved physical-device matching using the saved GUID plus DirectInput product identity, VID/PID and FFB driver/vendor information where available.
- Added safer interface quarantine, retry and reinitialization when a candidate interface or live ConstantForce update fails.
- Improved transient device-loss and hot reconnect recovery. A working wheel can be disconnected and reconnected during gameplay without requiring a game restart when the driver exposes it again correctly.
- Expanded F11 Force Feedback runtime diagnostics and setup status. Menu-time `waiting / released / inactive` is now shown as a pending state rather than a hardware failure because the FFB output is acquired when gameplay starts.
- `Direction test` is shown as pending until the 20% left/right test is actually run, instead of looking like a device error.
- Added named FFB feel profiles under `OutRun2006Tweaks.profiles\FFB\<name>.ini`. Feel profiles do not silently reroute the selected physical FFB device.
- Profile saves use a staged write, backup-on-overwrite, rename/copy fallback and final-file verification so an interrupted or rejected replacement is less likely to destroy the previous profile.
- Fixed the Windows first-save bug where a profile that did not exist yet could return `ERROR_FILE_NOT_FOUND` from `std::filesystem` and was incorrectly treated as a save failure.
- Profile save failures now report/log the real destination path and Windows filesystem error to make remote hardware testing easier.
- Further hardened Physics SAT transitions, counter-steer/reversal release and fallback behavior while retaining Natural SAT as the safe fallback when physics telemetry is not valid.
- Added more FFB tuning/diagnostic visibility, including mechanical/pneumatic trail response, output headroom/clipping information and optional per-wheel response correction controls.

### Hardware validation

- **MOZA R3:** primary development/regression hardware; current DirectInput ConstantForce, Spring and Damper paths remain working.
- **Simucube 3:** community testing confirmed that an intentional disconnect/reconnect during gameplay was recognized again and FFB resumed correctly.
- **Fanatec / multi-interface driver layouts:** v0.1.1 includes the new interface-selection, all-actuator probing and recovery work based on tester reports. Additional model-specific reports are welcome.
- A real Windows test also confirmed that named FFB profiles now save successfully to `OutRun2006Tweaks.profiles\FFB`.

### Upgrade / setup

Extract `OutRun2006Tweaks-Wheel-FFB-v0.1.1.zip` into the OutRun 2006: Coast 2 Coast game directory and replace the supplied files. Existing `OutRun2006Tweaks.user.ini`, input bindings and profile folders are not part of the release archive and should remain in place.

After upgrading, open **F11 → Force Feedback**, confirm the intended DirectInput FFB output, enter gameplay so the runtime acquires the device, and run the safe 20% left/right direction test once. Named FFB feel profiles are stored in `<game folder>\OutRun2006Tweaks.profiles\FFB`.

### Package contents

- `dinput8.dll`
- `OutRun2006Tweaks.ini`
- `OutRun2006Tweaks.lods.ini`
- `OR2006C2C.exe`
- `README.md`
- `RELEASE_NOTES_v0.1.1.md`
- `LICENSES.txt`

### Credits

Based on `emoose/OutRun2006Tweaks`, with public design/reference work from `hyp36rmax/multi-device-input`, `d-b-c-e/OutRun2006Tweaks-FFB`, and OutRun community hardware reports and testing.

This is an unofficial community fork and is not affiliated with SEGA, MOZA, Fanatec or Simucube.

---

<a id="한국어"></a>
## 한국어

v0.1.1은 첫 Wheel FFB 공개 버전인 v0.1의 **호환성·안정화 업데이트**입니다. 새로운 FFB 성격을 추가하기보다 기존에 만든 Modern / Physics SAT 계열을 유지하면서 실제 DirectInput 휠 드라이버 구성에서 더 안정적으로 동작하도록 개선했습니다.

**이번 릴리즈에는 X-Force를 포함하지 않습니다.** X-Force 실험은 별도 작업으로 남겨 두며, v0.1.1은 우리가 만든 Physics SAT / Natural SAT FFB 경로를 사용합니다.

### v0.1 이후 변경 사항

- 여러 Windows 인터페이스를 노출하는 휠베이스를 위한 DirectInput FFB 장치 선택 로직 강화
- 첫 번째 후보만 사용하는 대신 실제로 동작하는 호환 FFB 인터페이스를 끝까지 탐색하도록 개선
- 드라이버가 보고하는 모든 force-actuator axis를 열거·검증하고 ConstantForce 생성 및 0-force 실시간 업데이트 경로 확인
- 저장된 GUID뿐 아니라 DirectInput product identity, VID/PID, FFB driver/vendor 정보를 활용한 동일 물리 장치 매칭 강화
- 잘못된 인터페이스 또는 실시간 ConstantForce 출력 실패 시 안전한 격리, 재시도, 재초기화 처리 강화
- 일시적인 장치 손실과 핫 리커넥트 복구 개선. 드라이버가 장치를 정상적으로 다시 노출하면 게임 중 분리/재연결 후에도 게임 재시작 없이 FFB를 복구할 수 있도록 보강
- F11 Force Feedback 런타임 진단과 설정 상태 표시 개선. 메뉴에서의 `waiting / released / inactive`는 정상적인 대기 상태이므로 오류가 아니라 `pending`으로 표시
- 20% 좌/우 테스트를 아직 실행하지 않은 상태도 장치 오류가 아니라 `Direction test (not run)` 대기 상태로 표시
- `OutRun2006Tweaks.profiles\FFB\<이름>.ini`에 저장되는 이름별 FFB 감각 프로필 추가. FFB 감각 프로필을 불러와도 실제 출력 휠 장치를 임의로 변경하지 않음
- 프로필 저장을 `.tmp` staged write → 기존 파일 백업 → rename/copy fallback → 최종 파일 확인 방식으로 강화
- Windows에서 최초 저장 대상 파일이 아직 없을 때 `std::filesystem`의 `ERROR_FILE_NOT_FOUND`를 저장 실패로 잘못 처리하던 버그 수정
- 프로필 저장 실패 시 실제 대상 경로와 Windows 파일시스템 오류를 UI와 로그에 남기도록 개선
- Physics SAT 전환, 카운터스티어/토크 반전 해제, 유효하지 않은 물리 샘플에서 Natural SAT로 넘어가는 폴백 동작을 추가로 안정화
- Mechanical/Pneumatic Trail 응답, 출력 headroom/clipping, 선택형 휠 response correction 등 FFB 튜닝·진단 가시성 확대

### 하드웨어 검증

- **MOZA R3:** 주 개발·회귀 테스트 장비이며 현재 DirectInput ConstantForce, Spring, Damper 경로 정상 동작 확인
- **Simucube 3:** 커뮤니티 테스트에서 주행 중 의도적으로 장치를 분리했다 다시 연결해도 재인식되고 FFB가 정상 복구됨을 확인
- **Fanatec 및 멀티 인터페이스 드라이버 구성:** 사용자 테스트 결과를 바탕으로 인터페이스 선택, 전체 actuator 탐색, 실패 복구 경로를 v0.1.1에 반영. 모델별 추가 테스트 결과는 계속 환영
- 실제 Windows 환경에서 이름별 FFB 프로필이 `OutRun2006Tweaks.profiles\FFB`에 정상 저장되는 것도 확인

### 업데이트 / 설정

`OutRun2006Tweaks-Wheel-FFB-v0.1.1.zip`을 OutRun 2006: Coast 2 Coast 게임 폴더에 압축 해제하고 포함된 파일을 덮어씁니다. 기존 `OutRun2006Tweaks.user.ini`, Input Bindings, 프로필 폴더는 배포 ZIP에 포함하지 않으므로 그대로 유지됩니다.

업데이트 후 **F11 → Force Feedback**에서 원하는 DirectInput FFB 출력 장치를 확인하고, 실제 주행에 들어가 FFB 장치가 acquire된 뒤 20% 좌/우 방향 테스트를 한 번 실행하는 것을 권장합니다. 이름별 FFB 감각 프로필은 `<게임 폴더>\OutRun2006Tweaks.profiles\FFB`에 저장됩니다.

### 배포 파일 구성

- `dinput8.dll`
- `OutRun2006Tweaks.ini`
- `OutRun2006Tweaks.lods.ini`
- `OR2006C2C.exe`
- `README.md`
- `RELEASE_NOTES_v0.1.1.md`
- `LICENSES.txt`

### 크레딧

`emoose/OutRun2006Tweaks`를 기반으로 하며 `hyp36rmax/multi-device-input`, `d-b-c-e/OutRun2006Tweaks-FFB`의 공개 설계/참고 작업과 OutRun 커뮤니티의 하드웨어 보고 및 테스트를 참고했습니다.

이 프로젝트는 비공식 커뮤니티 포크이며 SEGA, MOZA, Fanatec 또는 Simucube와 제휴하거나 공식 지원받는 프로젝트가 아닙니다.
