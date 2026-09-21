cmake_minimum_required(VERSION 3.20)
project(outrun-vr-host LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "outrun-vr-host currently supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "outrun-vr-host must be configured as x64 (-A x64)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG release-1.1.63
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openxr_sdk)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
    OUTPUT_VARIABLE OUTRUN_VR_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT OUTRUN_VR_GIT_SHA)
    set(OUTRUN_VR_GIT_SHA "unknown")
endif()

add_executable(outrun-vr-host
    src/main_r23.cpp
    src/ipc/v3_shadow_bridge.cpp
    src/diagnostics/runtime_watchdog.cpp
)
target_include_directories(outrun-vr-host PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_compile_definitions(outrun-vr-host PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    OUTRUN_VR_BUILD_SHA="${OUTRUN_VR_GIT_SHA}"
)
if(MSVC)
    # R23 remains the verified source/pose owner. R24 remains the visible
    # fallback authority. R32 adds an A/B SafeEye fallback and a final verified
    # DirectGPU single-projection path with asynchronous producer ACK; every
    # uncertain case still falls through to R26/R24.
    target_compile_options(outrun-vr-host PRIVATE
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/openxr_api_compat.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/bounded_swapchain_wait.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/sbs_capture_override.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/d3d9ex_direct_passthrough.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/d3d9ex_direct_passthrough_r32.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/review_hardening.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/r21_runtime_hardening.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/r22_runtime_hardening.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/r23_runtime_hardening.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/r24_black_screen_guard.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/r26_recenter_hardening.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/r32_direct_submit.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/capture_wait_guard.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/src/runtime/r21_crash_diagnostics.hpp"
        "$<$<CONFIG:Release>:/Zi>"
    )
    target_link_options(outrun-vr-host PRIVATE
        "$<$<CONFIG:Release>:/DEBUG:FULL>"
        "$<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>"
    )
endif()
target_link_libraries(outrun-vr-host PRIVATE openxr_loader d3d11 dxgi d3dcompiler user32 dbghelp)

add_executable(outrun-vr-diagnostic
    src/diagnostic_pattern.cpp
)
target_compile_definitions(outrun-vr-diagnostic PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(outrun-vr-diagnostic PRIVATE openxr_loader d3d11 dxgi user32)

add_executable(outrun-vr-shader-smoke tests/stereo_shader_smoke.cpp)
target_include_directories(outrun-vr-shader-smoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_compile_definitions(outrun-vr-shader-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(outrun-vr-shader-smoke PRIVATE d3dcompiler)

add_executable(outrun-vr-protocol-v3-smoke tests/protocol_v3_smoke.cpp)
target_include_directories(outrun-vr-protocol-v3-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_compile_definitions(outrun-vr-protocol-v3-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(outrun-vr-core-math-smoke tests/core_math_smoke.cpp)
target_include_directories(outrun-vr-core-math-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_compile_definitions(outrun-vr-core-math-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(outrun-vr-host-state-v3-channel-smoke tests/host_state_v3_channel_smoke.cpp)
target_include_directories(outrun-vr-host-state-v3-channel-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_compile_definitions(outrun-vr-host-state-v3-channel-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(outrun-vr-v2-v3-shadow-conversion-smoke tests/v2_v3_shadow_conversion_smoke.cpp)
target_include_directories(outrun-vr-v2-v3-shadow-conversion-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_compile_definitions(outrun-vr-v2-v3-shadow-conversion-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(outrun-vr-host-pose-v3-validation-smoke tests/host_pose_v3_validation_smoke.cpp)
target_include_directories(outrun-vr-host-pose-v3-validation-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_compile_definitions(outrun-vr-host-pose-v3-validation-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# R23 behavior regressions. These are intentionally small enough to run in CI
# without a headset/game process and cover the fail-closed state machine plus
# host-owned verified-bundle identity/TTL semantics.
add_executable(outrun-vr-runtime-eligibility-smoke tests/runtime_eligibility_smoke.cpp)
target_include_directories(outrun-vr-runtime-eligibility-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
)
target_compile_definitions(outrun-vr-runtime-eligibility-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(outrun-vr-r23-verified-bundle-smoke tests/r23_verified_bundle_smoke.cpp)
target_include_directories(outrun-vr-r23-verified-bundle-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
    "${CMAKE_CURRENT_LIST_DIR}/src"
)
target_compile_definitions(outrun-vr-r23-verified-bundle-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(outrun-vr-r23-verified-bundle-smoke PRIVATE
    openxr_loader d3d11 dxgi d3dcompiler user32)

# R32 review regressions are pure policy tests: no headset, game or graphics
# device is needed to validate reset-relative safety and fail-closed decisions.
add_executable(outrun-vr-r32-policy-smoke tests/r32_policy_smoke.cpp)
target_include_directories(outrun-vr-r32-policy-smoke PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../src"
)
target_compile_definitions(outrun-vr-r32-policy-smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

foreach(target
    outrun-vr-host
    outrun-vr-diagnostic
    outrun-vr-shader-smoke
    outrun-vr-protocol-v3-smoke
    outrun-vr-core-math-smoke
    outrun-vr-host-state-v3-channel-smoke
    outrun-vr-v2-v3-shadow-conversion-smoke
    outrun-vr-host-pose-v3-validation-smoke
    outrun-vr-runtime-eligibility-smoke
    outrun-vr-r23-verified-bundle-smoke
    outrun-vr-r32-policy-smoke)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
        PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        PDB_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    )
endforeach()