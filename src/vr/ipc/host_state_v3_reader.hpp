#pragma once

#include "vr/ipc/protocol_v3.hpp"
#include "vr/ipc/win32_channel.hpp"

namespace OutRunVR::IpcV3
{
    class HostStateReader
    {
    public:
        bool Read(HostState& out) noexcept
        {
            if (!mapping_.EnsureOpen(HostStateName))
                return false;
            if (!Ipc::StableRead(mapping_.Get(), out))
                return false;
            return HeaderValid(out);
        }

        void Reset() noexcept
        {
            mapping_.Reset();
        }

        bool IsOpen() const noexcept
        {
            return mapping_.IsOpen();
        }

        static bool HeaderValid(const HostState& state) noexcept
        {
            return state.magic == HostMagic &&
                state.version == ProtocolVersion &&
                state.structSize == sizeof(HostState);
        }

    private:
        Ipc::ReadOnlyMapping<HostState> mapping_{};
    };
}
