# OutRun2006Tweaks Wheel FFB v0.1

[English](#english) | [한국어](#한국어)

<a id="english"></a>
## English

Unofficial wheel / force-feedback fork for **OutRun 2006: Coast 2 Coast**, based on [emoose/OutRun2006Tweaks](https://github.com/emoose/OutRun2006Tweaks).

v0.1 adds modern multi-device wheel input and native Windows DirectInput force feedback. Hardware development and validation were performed primarily with **MOZA R3**, while the FFB force model and default tuning are now device-independent and use the same logic on every supported DirectInput wheel.

### Highlights

- SDL3 raw multi-device input for wheel, pedals, shifter, button box and gamepad.
- One **Input Bindings** path for steering, pedals, shifter and buttons.
- Native DirectInput COM FFB; no vJoy or external FFB mapper required.
- Physics SAT with Natural SAT fallback, pneumatic/mechanical trail and fast counter-steer response.
- Light low-speed centering spring plus dynamic damping.
- Universal ConstantForce tactile path for road/slip detail where hardware periodic effects are not useful.
- Snow-stage curb/shoulder compensation and four-wheel mixed/full-rough surface handling.
- Gear-shift and collision feedback.
- Exact DirectInput FFB-device GUID persistence, reconnect handling and safe 20% direction tests.
- Optional estimated-RPM **Engine Vibration**. It is **OFF by default**; the default stored strength is **0.20** and is internally scaled so it remains a subtle texture rather than 20% wheel torque.

### Setup

1. Extract **OutRun2006Tweaks-Wheel-FFB-v0.1.zip** into the OutRun 2006: Coast 2 Coast game directory and replace files when prompted.
2. Connect the wheel and pedals before starting the game.
3. Open the overlay with **F11**.
4. Configure steering, pedals, shifter and buttons in **Input Bindings**, then use **Save & Return to game**.
5. Open **Force Feedback** and select the actual DirectInput FFB wheel if it is not already selected.
6. Use the 20% left/right direction tests before increasing wheel-base torque.
7. Use **Load Universal Physics SAT** as the recommended starting feel. FFB tuning applies live; use **Save Force Feedback** to persist it.

### Force-feedback notes

The main cornering return force comes from SAT rather than a strong artificial centre spring. Wheel/driver-side centering, damping, inertia and friction are additional forces, so keep them conservative while evaluating game-side FFB.

Road and snow/curb tactile effects remain enabled in the default universal preset. The snow-stage compensation is retained because it gives useful curb/shoulder feedback, though exact strength can still vary with the surface and wheel hardware.

**Engine Vibration** is intentionally optional. When enabled, RPM is estimated from speed, current gear and throttle. Its amplitude and frequency are smoothed, the frequency is kept in a lighter haptic range, and only a very small amount of output headroom may be reserved during ordinary driving so the texture does not abruptly disappear at brief SAT peaks. Collision/gear events retain priority.

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

**OutRun 2006: Coast 2 Coast**에서 현대식 레이싱 휠과 네이티브 포스피드백을 사용할 수 있도록 만든 비공식 포크입니다. [emoose/OutRun2006Tweaks](https://github.com/emoose/OutRun2006Tweaks)를 기반으로 합니다.

v0.1은 멀티 디바이스 입력과 Windows DirectInput 기반 휠 FFB를 추가합니다. 개발과 실제 하드웨어 검증은 주로 **MOZA R3**로 진행했지만, 현재 FFB 힘 계산과 기본 튜닝은 제조사/모델명에 따라 달라지지 않고 지원되는 DirectInput 휠에 같은 로직을 적용합니다.

### 주요 기능

- 휠, 별도 페달, 시프터, 버튼박스, 게임패드를 함께 사용하는 SDL3 Raw 멀티 디바이스 입력
- 조향, 페달, 시프터, 버튼을 설정하는 단일 **Input Bindings** 경로
- vJoy나 외부 매퍼가 필요 없는 Windows DirectInput COM 네이티브 FFB
- Physics SAT + Natural SAT 폴백, Pneumatic/Mechanical Trail, 빠른 카운터스티어 반응
- SAT를 가리지 않는 약한 저속 센터링 스프링과 Dynamic Damping
- 하드웨어 periodic effect가 실질적으로 유용하지 않은 경우에도 동일하게 동작하는 범용 ConstantForce 노면/슬립 촉각 경로
- 눈 맵 연석/숄더 보정과 네 바퀴 mixed/full-rough 표면 처리
- 기어 변속 및 충돌 피드백
- 정확한 DirectInput FFB GUID 저장, 재연결 처리, 20% 안전 방향 테스트
- 추정 RPM 기반 **Engine Vibration** 옵션. 기본값은 **OFF**, 저장 강도 기본값은 **0.20**이며 실제 휠 토크 20%가 바로 들어가는 방식이 아니라 내부에서 약하게 스케일됩니다.

### 설정 방법

1. **OutRun2006Tweaks-Wheel-FFB-v0.1.zip**을 게임 폴더에 압축 해제하고 파일을 덮어씁니다.
2. 게임 실행 전에 휠과 페달을 연결합니다.
3. **F11**로 오버레이를 엽니다.
4. **Input Bindings**에서 스티어링, 페달, 시프터, 버튼을 설정하고 **Save & Return to game**으로 저장합니다.
5. **Force Feedback**에서 실제 DirectInput FFB 휠이 선택됐는지 확인합니다.
6. 휠베이스 토크를 올리기 전에 20% 좌/우 방향 테스트를 사용합니다.
7. 기본 시작점은 **Load Universal Physics SAT**를 권장합니다. FFB 변경은 실시간 적용되며 **Save Force Feedback**으로 저장합니다.

### FFB 참고 사항

코너링에서 중앙으로 돌아가려는 주된 힘은 강한 인위적 센터 스프링이 아니라 SAT가 담당합니다. 휠베이스/드라이버 자체의 센터링, 댐핑, 관성, 마찰은 게임 FFB에 추가되는 힘이므로 과도하게 높이지 않는 것을 권장합니다.

노면과 눈길 연석 진동은 기본 범용 프리셋에서 유지합니다. 눈 맵 보정은 실제 테스트에서 연석/숄더 감각을 어느 정도 살려주는 효과가 있어 그대로 포함했습니다. 다만 표면과 휠 하드웨어에 따라 강도 차이는 있을 수 있습니다.

**Engine Vibration**은 취향에 따라 켜는 선택 옵션으로 기본 OFF입니다. 켜면 속도, 현재 기어, 스로틀로 RPM을 추정하고 진폭/주파수를 부드럽게 필터링합니다. 주행 중 너무 무겁게 느껴지지 않도록 더 높은 촉각 주파수 대역과 낮은 내부 출력 스케일을 사용하며, 일반 주행에서는 SAT 피크 때문에 갑자기 사라지는 현상을 줄이기 위해 아주 작은 출력 여유만 확보합니다. 충돌/기어 이벤트는 우선합니다.

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
