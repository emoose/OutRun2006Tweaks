#pragma once

#include "vr/core/frame_types.hpp"

#include <cstdint>

namespace OutRunVR::Core
{
    struct AdapterId
    {
        std::uint32_t luidLow{};
        std::int32_t luidHigh{};

        friend constexpr bool operator==(const AdapterId&, const AdapterId&) = default;
    };

    enum class TransportKind : std::uint32_t
    {
        None = 0,
        D3D9ExShared = 1,
        DesktopDuplication = 2,
        Dxvk = 3,
    };

    struct FrameSlot
    {
        std::uint32_t generation{};
        std::uint32_t slot{};
        std::uint64_t frameId{};
        std::uintptr_t leftHandle{};
        std::uintptr_t rightHandle{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t format{};
    };

    class IFrameProducer
    {
    public:
        virtual ~IFrameProducer() = default;
        virtual TransportKind kind() const noexcept = 0;
        virtual void invalidate() noexcept = 0;
        virtual bool publish(const PresentedFrame& frame, FrameSlot& outSlot) noexcept = 0;
    };

    class IFrameConsumer
    {
    public:
        virtual ~IFrameConsumer() = default;
        virtual TransportKind kind() const noexcept = 0;
        virtual void invalidate() noexcept = 0;
        virtual bool acquire(const PresentedFrame& frame, const FrameSlot& slot) noexcept = 0;
        virtual void acknowledge(std::uint64_t frameId) noexcept = 0;
    };
}
