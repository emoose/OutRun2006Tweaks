#pragma once

#include "vr/core/frame_types.hpp"
#include "vr/core/transport.hpp"

namespace OutRunVR::Host
{
    class IFrameSource
    {
    public:
        virtual ~IFrameSource() = default;
        virtual Core::TransportKind kind() const noexcept = 0;
        virtual bool available() const noexcept = 0;
        virtual bool acquire(const Core::PresentedFrame& descriptor) noexcept = 0;
        virtual void release(std::uint64_t frameId) noexcept = 0;
        virtual void invalidate() noexcept = 0;
    };
}
