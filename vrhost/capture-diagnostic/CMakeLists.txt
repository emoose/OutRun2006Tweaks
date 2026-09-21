cmake_minimum_required(VERSION 3.20)
project(outrun-vr-capture-diagnostic LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "capture diagnostic currently supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "capture diagnostic must be configured as x64 (-A x64)")
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

add_executable(outrun-vr-capture-diagnostic
    "${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/ipc/v3_shadow_bridge.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/diagnostics/runtime_watchdog.cpp"
)
target_include_directories(outrun-vr-capture-diagnostic PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../src"
    "${CMAKE_CURRENT_LIST_DIR}/../src"
)
target_compile_definitions(outrun-vr-capture-diagnostic PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    OUTRUN_VR_CAPTURE_DIAGNOSTIC=1
)
if(MSVC)
    target_compile_options(outrun-vr-capture-diagnostic PRIVATE
        "/FI${CMAKE_CURRENT_LIST_DIR}/../src/runtime/openxr_api_compat.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/../src/runtime/sbs_capture_override.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/../src/runtime/d3d9ex_direct_passthrough.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/../src/runtime/review_hardening.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/../src/runtime/capture_chain_diagnostic.hpp"
        "/FI${CMAKE_CURRENT_LIST_DIR}/../src/runtime/r18_host_pipeline_diagnostic.hpp"
    )
endif()
target_link_libraries(outrun-vr-capture-diagnostic PRIVATE
    openxr_loader d3d11 dxgi d3dcompiler user32
)
set_target_properties(outrun-vr-capture-diagnostic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
)
