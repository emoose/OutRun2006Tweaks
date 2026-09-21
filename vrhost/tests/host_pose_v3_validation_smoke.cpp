#include <cmath>
#include <cstdint>
#include <iostream>

#include "vr/ipc/host_pose_v3.hpp"

using namespace OutRunVR::IpcV3;

namespace
{
    HostState MakeValid()
    {
        HostState state{};
        InitializeWireState(state);
        state.hostPid = 42;
        state.flags = HostAlive | OrientationValid | PositionValid |
            SessionVisible | SessionFocused | StereoViewsValid | HostShouldRender;
        state.poseId = 1234;
        state.sampleQpc = 9'950'000;
        state.headOrientation[3] = 1.0f;
        state.headPositionMeters[0] = 1.0f;
        state.headPositionMeters[1] = 2.0f;
        state.headPositionMeters[2] = 3.0f;
        for (int eye = 0; eye < 2; ++eye)
        {
            state.eyes[eye].orientation[3] = 1.0f;
            state.eyes[eye].positionMeters[0] = eye == 0 ? -0.032f : 0.032f;
            state.eyes[eye].fov.angleLeft = -0.8f;
            state.eyes[eye].fov.angleRight = 0.8f;
            state.eyes[eye].fov.angleUp = 0.9f;
            state.eyes[eye].fov.angleDown = -0.9f;
        }
        return state;
    }

    bool Expect(bool condition, const char* name)
    {
        if (!condition)
            std::cerr << "FAILED: " << name << '\n';
        return condition;
    }
}

int main()
{
    constexpr std::int64_t now = 10'000'000;
    constexpr std::int64_t frequency = 1'000'000;
    bool ok = true;

    auto state = MakeValid();
    ok &= Expect(HostStateUsable(state, now, frequency), "valid pose accepted");

    state = MakeValid();
    state.sampleQpc = now - frequency;
    ok &= Expect(!HostStateUsable(state, now, frequency), "stale pose rejected");

    state = MakeValid();
    state.headOrientation[0] = std::nanf("");
    ok &= Expect(!HostStateUsable(state, now, frequency), "NaN quaternion rejected");

    state = MakeValid();
    state.eyes[0].positionMeters[0] = -0.5f;
    ok &= Expect(!HostStateUsable(state, now, frequency), "implausible eye offset rejected");

    state = MakeValid();
    state.eyes[1].fov.angleRight = state.eyes[1].fov.angleLeft + 0.01f;
    ok &= Expect(!HostStateUsable(state, now, frequency), "degenerate FOV rejected");

    state = MakeValid();
    state.flags &= ~HostShouldRender;
    ok &= Expect(!HostStateUsable(state, now, frequency), "non-renderable host rejected");

    state = MakeValid();
    state.flags &= ~StereoViewsValid;
    ok &= Expect(HostStateUsable(state, now, frequency), "mono-valid pose remains usable");

    if (!ok)
        return 1;
    std::cout << "HostState.v3 pose validation rules passed.\n";
    return 0;
}
