#include "vr/ipc/protocol_v3.hpp"

#include <cstddef>

namespace
{
    using namespace OutRunVR::IpcV3;

    static_assert(ProtocolVersion == 3);
    static_assert(RingSize == 4);
    static_assert(sizeof(WireHandle) == 8);
    static_assert(offsetof(HostState, poseId) % 4 == 0);
    static_assert(offsetof(ClientState, interopProbeHandle) % 4 == 0);
    static_assert(offsetof(FrameDescriptor, leftHandle) % 4 == 0);
    static_assert(offsetof(FrameDescriptor, renderedEyes) > offsetof(FrameDescriptor, rightHandle));
    static_assert(sizeof(FrameRing) == 32 + sizeof(FrameDescriptor) * RingSize);
}
