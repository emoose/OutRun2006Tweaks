# OutRun2006Tweaks Wheel FFB v0.1

[English](#english) | [한국어](#한국어)

<a id="english"></a>
## English

First public release of the `wheel-ffb` branch.

### Highlights

- SDL3 raw multi-device input for modern wheel setups.
- One Input Bindings path for steering, pedals, shifter and buttons.
- Native Windows DirectInput COM wheel FFB.
- Device-independent Physics SAT / Natural SAT force model with mechanical/caster trail.
- Dynamic damping, grip-loss unloading and improved counter-steer response.
- Light low-speed centering spring so SAT remains the main cornering return force.
- Universal ConstantForce road/slip tactile fallback.
- Snow-stage curb/shoulder compensation with four-wheel mixed/full-rough detection.
- Gear-shift and collision feedback.
- Exact DirectInput FFB GUID persistence, reconnect handling and safe 20% direction tests.
- Optional estimated-RPM Engine Vibration. Default OFF; default stored strength 0.20 with conservative internal scaling.

### Tested hardware

Primary physical validation was performed with **MOZA R3**. The current FFB tuning no longer branches on manufacturer/model name, so supported DirectInput wheels run the same force logic; physical feel can still differ by motor torque, firmware, wheel diameter and driver-side effects.

### Recommended setup

Use **F11 → Input Bindings** for steering, pedals, shifter and buttons, then **Save & Return to game**. Use **F11 → Force Feedback** to select the actual FFB output wheel and start with **Load Universal Physics SAT**. FFB tuning applies live; use **Save Force Feedback** to persist changes.

Road and snow/curb tactile feedback remain enabled. The snow-stage compensation is retained because it provides useful curb/shoulder feedback in physical testing. The v0.1 refresh also fixes a persistent-vibration bug after snow stages: the snow curb latch now expires after its fixed 450 ms grace period, and low-roughness route-fork material changes no longer trigger the curb boost.

Engine Vibration is intentionally an opt-in effect. When enabled, estimated RPM follows speed, current gear and throttle. The v0.1 release smooths amplitude/frequency, shifts the texture away from the heavy low-frequency pulse region and uses only a small output reserve during normal driving so it is less likely to appear/disappear at SAT peaks. Collision and gear events retain priority.

### Install

Extract `OutRun2006Tweaks-Wheel-FFB-v0.1.zip` into the OutRun 2006: Coast 2 Coast game directory and replace files when prompted.

### Package contents

- `dinput8.dll`
- `OutRun2006Tweaks.ini`
- `OutRun2006Tweaks.lods.ini`
- `OR2006C2C.exe`
- `README.md`
- `LICENSES.txt`

### Credits

Based on `emoose/OutRun2006Tweaks`, with public design/reference work from `hyp36rmax/multi-device-input`, `d-b-c-e/OutRun2006Tweaks-FFB`, and OutRun community hardware reports.

This is an unofficial community fork and is not affiliated with SEGA or MOZA.

---

<a id="한국어"></a>
## 한국어

`wheel-ffb` 브랜치의 첫 공개 릴리즈입니다.

### 주요 기능

- 현대식 휠 구성을 위한 SDL3 Raw 멀티 디바이스 입력
- 스티어링, 페달, 시프터, 버튼을 설정하는 단일 Input Bindings 경로
- Windows DirectInput COM 기반 네이티브 휠 FFB
- 제조사/모델명에 따라 달라지지 않는 Physics SAT / Natural SAT 힘 계산과 Mechanical/Caster Trail
- Dynamic Damping, 그립 손실 시 하중 감소, 개선된 카운터스티어 반응
- SAT가 주된 코너링 복원력이 되도록 약하게 유지한 저속 센터링 스프링
- 모든 휠에서 동일하게 사용하는 ConstantForce 노면/슬립 촉각 폴백
- 네 바퀴 mixed/full-rough 감지를 포함한 눈 맵 연석/숄더 보정
- 기어 변속 및 충돌 피드백
- 정확한 DirectInput FFB GUID 저장, 재연결 처리, 20% 안전 방향 테스트
- 추정 RPM 기반 Engine Vibration 선택 옵션. 기본 OFF, 저장 강도 기본값 0.20이며 내부 출력은 보수적으로 제한

### 테스트한 하드웨어

실제 하드웨어 검증은 주로 **MOZA R3**로 진행했습니다. 현재 FFB 튜닝은 제조사/모델명으로 힘 계산을 바꾸지 않으며 지원되는 DirectInput 휠에 같은 로직을 사용합니다. 다만 모터 토크, 펌웨어, 휠 직경, 드라이버 자체 효과 때문에 실제 체감은 달라질 수 있습니다.

### 권장 설정

**F11 → Input Bindings**에서 스티어링, 페달, 시프터, 버튼을 설정하고 **Save & Return to game**으로 저장합니다. **F11 → Force Feedback**에서 실제 FFB 출력 휠을 선택한 뒤 **Load Universal Physics SAT**를 시작점으로 권장합니다. FFB 변경은 실시간 적용되며 **Save Force Feedback**으로 저장합니다.

노면과 눈길 연석 진동은 유지했습니다. 눈 맵 보정은 실제 테스트에서 연석/숄더 감각을 어느 정도 살려주는 효과가 있어 v0.1에 그대로 포함합니다. 이번 v0.1 갱신에서는 눈길 종료 후 진동이 계속되는 버그도 수정했습니다. 눈길 연석 latch는 확인된 mixed 접촉 후 450ms까지만 유지되며, 거칠기가 낮은 일반 갈림길 재질 변화는 연석 진동으로 증폭하지 않습니다.

Engine Vibration은 취향에 따라 켜는 선택 옵션이며 기본 OFF입니다. 켜면 속도, 현재 기어, 스로틀로 RPM을 추정합니다. v0.1에서는 진폭/주파수를 부드럽게 필터링하고 무겁게 느껴지는 저주파 펄스 영역을 피하며, 일반 주행에서 SAT 피크 때문에 진동이 갑자기 사라지는 현상을 줄이기 위해 아주 작은 출력 여유만 확보합니다. 충돌과 기어 이벤트가 우선합니다.

### 설치

`OutRun2006Tweaks-Wheel-FFB-v0.1.zip`을 OutRun 2006: Coast 2 Coast 게임 폴더에 압축 해제하고 파일 교체 안내가 나오면 덮어씁니다.

### 배포 파일 구성

- `dinput8.dll`
- `OutRun2006Tweaks.ini`
- `OutRun2006Tweaks.lods.ini`
- `OR2006C2C.exe`
- `README.md`
- `LICENSES.txt`

### 크레딧

`emoose/OutRun2006Tweaks`를 기반으로 하며 `hyp36rmax/multi-device-input`, `d-b-c-e/OutRun2006Tweaks-FFB`의 공개 설계/참고 작업과 OutRun 커뮤니티 하드웨어 보고를 참고했습니다.

이 프로젝트는 비공식 커뮤니티 포크이며 SEGA 또는 MOZA와 제휴하거나 공식 지원받는 프로젝트가 아닙니다.
