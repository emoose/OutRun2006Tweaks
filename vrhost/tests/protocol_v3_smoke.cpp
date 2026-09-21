#include "vr/ipc/protocol_v3.hpp"

#include <cstddef>
#include <cstdint>

int main()
{
    using namespace OutRunVR::IpcV3;

    static_assert(ProtocolVersion == 3);
    static_assert(RingSize == 4);
    static_assert(sizeof(WireHandle) == 8);
    static_assert(sizeof(WireFov) == 16);
    static_assert(sizeof(WireEyeView) == 48);
    static_assert(sizeof(FrameRing) == 32 + sizeof(FrameDescriptor) * RingSize);

    HostState host{};
    ClientState client{};
    FrameRing frames{};
    AckState ack{};

    return host.magic == HostMagic && client.magic == ClientMagic &&
        frames.magic == FrameMagic && ack.magic == AckMagic ? 0 : 1;
}
