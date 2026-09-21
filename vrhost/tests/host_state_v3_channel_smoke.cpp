#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "ipc/host_state_v3_writer.hpp"
#include "vr/ipc/host_state_v3_reader.hpp"

namespace
{
    bool Near(float a, float b, float epsilon = 1.0e-6f)
    {
        return std::fabs(a - b) <= epsilon;
    }

    bool Check(bool condition, const char* message)
    {
        if (!condition)
            std::cerr << "host_state_v3_channel_smoke: " << message << "\n";
        return condition;
    }
}

int main()
{
    using namespace OutRunVR;

    constexpr std::uint32_t luidLow = 0x12345678u;
    constexpr std::int32_t luidHigh = -17;

    Host::HostStateV3Writer writer(luidLow, luidHigh);
    IpcV3::HostStateReader reader;

    IpcV3::HostState first{};
    IpcV3::InitializeWireState(first);
    first.flags = IpcV3::HostAlive |
        IpcV3::OrientationValid |
        IpcV3::PositionValid |
        IpcV3::SessionVisible |
        IpcV3::SessionFocused |
        IpcV3::StereoViewsValid |
        IpcV3::HostShouldRender |
        IpcV3::DirectGpuTransportSupported |
        IpcV3::AdapterLuidValid;
    first.directTransportGeneration = 7;
    first.poseId = 0x1122334455667788ull;
    first.predictedDisplayTime = 123456789;
    first.sampleQpc = 987654321;
    first.headOrientation[0] = 0.1f;
    first.headOrientation[1] = -0.2f;
    first.headOrientation[2] = 0.3f;
    first.headOrientation[3] = 0.9f;
    first.headPositionMeters[0] = 1.0f;
    first.headPositionMeters[1] = 2.0f;
    first.headPositionMeters[2] = -3.0f;
    first.recommendedWidth[0] = 2016;
    first.recommendedWidth[1] = 2016;
    first.recommendedHeight[0] = 2208;
    first.recommendedHeight[1] = 2208;

    for (int eye = 0; eye < 2; ++eye)
    {
        first.eyes[eye].orientation[3] = 1.0f;
        first.eyes[eye].positionMeters[0] = eye == 0 ? -0.032f : 0.032f;
        first.eyes[eye].fov.angleLeft = -0.8f;
        first.eyes[eye].fov.angleRight = 0.8f;
        first.eyes[eye].fov.angleUp = 0.9f;
        first.eyes[eye].fov.angleDown = -0.9f;
    }
    std::strncpy(first.runtimeName, "v3-channel-smoke", sizeof(first.runtimeName) - 1);

    const std::uint32_t sequence1 = writer.Publish(first);
    if (!Check(sequence1 != 0 && (sequence1 & 1u) == 0, "first publish did not finish on an even sequence"))
        return 1;

    IpcV3::HostState observed{};
    if (!Check(reader.Read(observed), "reader could not open/read HostState.v3"))
        return 2;
    if (!Check((observed.sequence & 1u) == 0, "reader observed an odd sequence"))
        return 3;
    if (!Check(observed.hostPid == GetCurrentProcessId(), "host pid mismatch"))
        return 4;
    if (!Check(observed.poseId == first.poseId, "pose id mismatch"))
        return 5;
    if (!Check(observed.predictedDisplayTime == first.predictedDisplayTime, "predicted display time mismatch"))
        return 6;
    if (!Check(observed.sampleQpc == first.sampleQpc, "sample QPC mismatch"))
        return 7;
    if (!Check(observed.adapterLuidLow == luidLow && observed.adapterLuidHigh == luidHigh, "adapter LUID mismatch"))
        return 8;
    if (!Check(observed.referenceSpaceGeneration == writer.ReferenceSpaceGeneration(), "reference generation mismatch"))
        return 9;
    if (!Check(observed.flags == first.flags, "host flags mismatch"))
        return 10;
    if (!Check(Near(observed.eyes[0].positionMeters[0], -0.032f) &&
        Near(observed.eyes[1].positionMeters[0], 0.032f), "eye offsets mismatch"))
        return 11;
    if (!Check(std::strcmp(observed.runtimeName, "v3-channel-smoke") == 0, "runtime name mismatch"))
        return 12;

    const std::uint32_t generation1 = writer.ReferenceSpaceGeneration();
    writer.ReferenceSpaceChanged();
    first.poseId++;
    first.sampleQpc++;
    const std::uint32_t sequence2 = writer.Publish(first);
    if (!Check(sequence2 > sequence1 && (sequence2 & 1u) == 0, "second publish sequence did not advance"))
        return 13;

    IpcV3::HostState observed2{};
    if (!Check(reader.Read(observed2), "reader failed after reference-space change"))
        return 14;
    if (!Check(observed2.poseId == first.poseId, "second pose id mismatch"))
        return 15;
    if (!Check(observed2.referenceSpaceGeneration != generation1 &&
        observed2.referenceSpaceGeneration == writer.ReferenceSpaceGeneration(),
        "reference-space generation did not advance"))
        return 16;

    std::cout << "HostState.v3 named mapping + seqlock round-trip passed.\n";
    return 0;
}
