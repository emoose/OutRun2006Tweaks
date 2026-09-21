# Third-party notices and acknowledgements

[English](#english) | [한국어](#한국어)

<a id="english"></a>
## English

This file documents the main upstream/reference projects used by the `wheel-ffb` branch and how binary-release notices are handled.

### Upstream base

#### emoose / OutRun2006Tweaks

Repository: https://github.com/emoose/OutRun2006Tweaks

Role: upstream/base project. The wrapper, game hooks, overlay/configuration system and much of the codebase originate from this project.

License: MIT. The original `Copyright (c) 2023 emoose` notice is preserved in this repository's `LICENSE.md`.

### Wheel/input references

#### hyp36rmax / multi-device-input

Repository/branch: https://github.com/hyp36rmax/multi-device-input/tree/multi-device-input

Role: important public reference/adaptation source for SDL3 raw multi-device input, separate USB-device handling and modern wheel compatibility behavior.

The branch retains the upstream MIT license notice.

#### d-b-c-e / OutRun2006Tweaks-FFB

Repository: https://github.com/d-b-c-e/OutRun2006Tweaks-FFB

Role: important public reference for DirectInput wheel-FFB architecture and force-model/signal-conditioning ideas.

The repository retains the upstream MIT license notice. This fork does not redistribute the separate `WheelFfb.dll` toolkit from that project; the v0.1 FFB backend is implemented directly with Windows DirectInput COM.

### VR GPL source reuse

#### bo3b / 3Dmigoto

Repository: https://github.com/bo3b/3Dmigoto

License: GNU GPL version 3.

Role: the `vr-openxr-gpl-reuse` branch adapts 3Dmigoto's DirectX 9 shader
fingerprinting approach and its FNV-1 64-bit byte-buffer hash routine. The
adapted implementation is isolated in
`src/vr/d3d9/shader_fingerprint_gpl.hpp` and is used by the R26 safe-draw
diagnostic path to identify vertex/pixel shaders by stable bytecode fingerprint
instead of process-local COM pointer values.

The GPLv3 license text is included as `COPYING.GPL3`. Original MIT notices for
OutRun2006Tweaks remain preserved.

### Build dependencies

The produced DLL also incorporates open-source dependencies from the upstream build, including projects fetched or linked through CMake/submodules such as SDL, SafetyHook/Zydis, spdlog, Dear ImGui, xxHash, IXWebSocket, zlib, jsoncpp, Ogg/FLAC, miniz and related support libraries.

Rather than manually maintaining a second copy of every dependency license in the release workflow, CI scans the exact checked-out/fetched dependency source trees used for the build and concatenates unique `LICENSE*`, `COPYING*` and `COPYRIGHT*` files into the release package as `LICENSES.txt`.

This keeps the public ZIP small while retaining the notices supplied by the actual dependency revisions that were compiled.

### Replacement game executable

The release package includes `OR2006C2C.exe` downloaded from the public upstream OutRun2006Tweaks v0.1 release asset. The upstream release describes it as a replacement executable for Steam/DVD installations so DLL wrappers can work with the game.

Source release: https://github.com/emoose/OutRun2006Tweaks/releases/tag/v0.1

Game code/assets and related trademarks remain the property of their respective rights holders; inclusion here does not change those rights.

### Community testing

Community discussions and user hardware logs were used as practical testing/design input for wheel behavior, device-interface quirks, force direction, reconnect handling and R3 tactile tuning.

### License summary

The upstream/base MIT-covered source remains under MIT with its notices preserved. The `vr-openxr-gpl-reuse` branch contains GPLv3-derived source and the combined branch distribution is GPL-3.0-only. Binary-release dependency notices are generated from the actual source/dependency trees and shipped as `LICENSES.txt`.

This file is a project attribution/packaging record, not legal advice.

---

<a id="한국어"></a>
## 한국어

이 문서는 `wheel-ffb` 브랜치에서 사용하거나 참고한 주요 상위 프로젝트와 공개 프로젝트, 그리고 바이너리 배포 시 라이선스 고지를 처리하는 방식을 설명합니다.

### 원본 기반 프로젝트

#### emoose / OutRun2006Tweaks

저장소: https://github.com/emoose/OutRun2006Tweaks

역할: 이 포크의 원본/기반 프로젝트입니다. DLL 래퍼, 게임 훅, 오버레이/설정 시스템 및 코드베이스의 상당 부분이 이 프로젝트에서 유래했습니다.

라이선스: MIT. 원본의 `Copyright (c) 2023 emoose` 고지는 이 저장소의 `LICENSE.md`에 그대로 유지합니다.

### 휠/입력 관련 참고 프로젝트

#### hyp36rmax / multi-device-input

저장소/브랜치: https://github.com/hyp36rmax/multi-device-input/tree/multi-device-input

역할: SDL3 Raw 멀티 디바이스 입력, 별도 USB 장치 처리, 현대식 레이싱 휠 호환 동작을 구현하는 데 중요한 공개 참고/적용 소스로 사용했습니다.

해당 브랜치는 원본 MIT 라이선스 고지를 유지합니다.

#### d-b-c-e / OutRun2006Tweaks-FFB

저장소: https://github.com/d-b-c-e/OutRun2006Tweaks-FFB

역할: DirectInput 휠 FFB 구조와 포스 모델/신호 처리 아이디어의 중요한 공개 참고 자료로 사용했습니다.

해당 저장소는 원본 MIT 라이선스 고지를 유지합니다. 이 포크는 그 프로젝트의 별도 `WheelFfb.dll` 툴킷을 재배포하지 않으며, v0.1 FFB 백엔드는 Windows DirectInput COM을 직접 사용해 구현했습니다.

### VR GPL 소스 재사용

#### bo3b / 3Dmigoto

저장소: https://github.com/bo3b/3Dmigoto

라이선스: GNU GPL version 3.

역할: `vr-openxr-gpl-reuse` 브랜치는 3Dmigoto의 DirectX 9 셰이더
fingerprint 방식과 FNV-1 64-bit 바이트 버퍼 해시 루틴을 적용했습니다.
적용 코드는 `src/vr/d3d9/shader_fingerprint_gpl.hpp`에 분리되어 있으며,
R26 safe-draw 진단 경로에서 실행마다 달라질 수 있는 COM 포인터 대신
셰이더 바이트코드 기반의 안정적인 식별값을 기록하는 데 사용합니다.

GPLv3 전문은 `COPYING.GPL3`에 포함되어 있으며, 기존 OutRun2006Tweaks의
MIT 저작권/허가 고지는 그대로 유지합니다.

### 빌드 의존성

생성되는 DLL에는 원본 빌드에서 사용하는 여러 오픈소스 의존성도 포함됩니다. CMake 또는 서브모듈로 가져오거나 링크되는 SDL, SafetyHook/Zydis, spdlog, Dear ImGui, xxHash, IXWebSocket, zlib, jsoncpp, Ogg/FLAC, miniz 및 관련 지원 라이브러리 등이 이에 해당합니다.

릴리즈 워크플로에서 각 의존성 라이선스 파일을 수동으로 별도 관리하는 대신, CI가 실제 빌드에 사용한 체크아웃/다운로드된 의존성 소스 트리를 스캔합니다. 이후 중복을 제거한 `LICENSE*`, `COPYING*`, `COPYRIGHT*` 파일을 하나의 `LICENSES.txt`로 합쳐 배포 패키지에 넣습니다.

이 방식은 실제 컴파일에 사용된 의존성 버전의 고지를 유지하면서 공개 ZIP이 불필요하게 복잡해지는 것을 줄입니다.

### 교체용 게임 실행 파일

릴리즈 패키지에는 공개된 원본 OutRun2006Tweaks v0.1 릴리즈 자산에서 가져온 `OR2006C2C.exe`가 포함됩니다. 원본 릴리즈에서는 Steam/DVD 설치본에서 DLL 래퍼가 동작하도록 하기 위한 교체용 실행 파일로 설명하고 있습니다.

원본 릴리즈: https://github.com/emoose/OutRun2006Tweaks/releases/tag/v0.1

게임 코드/자산 및 관련 상표의 권리는 각각의 권리자에게 있으며, 이 패키지에 포함한다고 해서 해당 권리가 변경되는 것은 아닙니다.

### 커뮤니티 테스트

커뮤니티 토론과 사용자 하드웨어 로그를 통해 휠 동작, 장치 인터페이스 특성, 힘의 방향, 재연결 처리, R3 촉각 튜닝에 대한 실사용 테스트/설계 정보를 참고했습니다.

### 라이선스 요약

원본 및 기존 MIT 코드의 MIT 권리와 고지는 그대로 유지합니다. 다만 `vr-openxr-gpl-reuse` 브랜치는 GPLv3 유래 소스를 포함하므로 결합된 브랜치 배포물은 GPL-3.0-only 조건으로 배포합니다. 바이너리 배포에 필요한 의존성 고지는 실제 소스/의존성 트리에서 생성해 `LICENSES.txt`로 함께 배포합니다.

이 문서는 프로젝트의 출처 표시 및 배포 패키징 기록이며 법률 자문이 아닙니다.
