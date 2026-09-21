#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "sbs_capture_override.hpp"
#include "vr_shared.hpp"

namespace OutRunVrR23VerifiedBundle
{
    enum class SourceKind : std::uint32_t
    {
        None = 0,
        ClassicSbs = 1,
        DirectGpu = 2,
    };

    // Keep the same grace budget as the host compositor. A newer game frame may
    // be published while this bundle is still valid; that must not invalidate a
    // texture/pose pair the host already owns and is about to submit.
    inline constexpr std::uint64_t MaxPresentationAgeMs = 1000;

    struct Snapshot
    {
        OutRunVR::SharedRenderFrameState frame{};
        std::uint32_t frameId{};
        std::uint32_t poseSequence{};
        std::int64_t presentQpc{};
        std::int64_t sourceCaptureQpc{};
        std::uint64_t publishedAtMs{};
        SourceKind kind{ SourceKind::None };
    };

    inline OutRunVR::SharedRenderFrameState Frame{};
    inline std::atomic<std::uint32_t> FrameId{ 0 };
    inline std::atomic<std::uint32_t> PoseSequence{ 0 };
    inline std::atomic<std::int64_t> PresentQpc{ 0 };
    inline std::atomic<std::int64_t> SourceCaptureQpc{ 0 };
    inline std::atomic<std::uint64_t> PublishedAtMs{ 0 };
    inline std::atomic<std::uint32_t> Kind{ static_cast<std::uint32_t>(SourceKind::None) };
    inline std::atomic<std::uint32_t> PublishSequence{ 0 };

    inline void Invalidate() noexcept
    {
        PublishSequence.fetch_add(1, std::memory_order_acq_rel);
        std::memset(&Frame, 0, sizeof(Frame));
        Kind.store(static_cast<std::uint32_t>(SourceKind::None), std::memory_order_relaxed);
        FrameId.store(0, std::memory_order_relaxed);
        PoseSequence.store(0, std::memory_order_relaxed);
        PresentQpc.store(0, std::memory_order_relaxed);
        SourceCaptureQpc.store(0, std::memory_order_relaxed);
        PublishedAtMs.store(0, std::memory_order_relaxed);
        PublishSequence.fetch_add(1, std::memory_order_release);
    }

    inline void Publish(const OutRunVR::SharedRenderFrameState& frame,
        SourceKind kind, std::int64_t sourceCaptureQpc = 0) noexcept
    {
        // main_r23 publishes the production capture before it promotes a classic
        // candidate. Snapshot that exact source as part of the committed bundle.
        if (kind == SourceKind::ClassicSbs && sourceCaptureQpc <= 0)
            sourceCaptureQpc = OutRunVrSbsCaptureOverride::LastProductionPresentQpc;

        PublishSequence.fetch_add(1, std::memory_order_acq_rel);
        std::memcpy(&Frame, &frame, sizeof(Frame));
        FrameId.store(frame.frameId, std::memory_order_relaxed);
        PoseSequence.store(frame.sourcePoseSequence, std::memory_order_relaxed);
        PresentQpc.store(frame.presentQpc, std::memory_order_relaxed);
        SourceCaptureQpc.store(sourceCaptureQpc, std::memory_order_relaxed);
        PublishedAtMs.store(GetTickCount64(), std::memory_order_relaxed);
        Kind.store(static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
        PublishSequence.fetch_add(1, std::memory_order_release);
    }

    inline bool Read(Snapshot& out) noexcept
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::uint32_t before = PublishSequence.load(std::memory_order_acquire);
            if (before & 1u) continue;

            Snapshot s{};
            std::memcpy(&s.frame, &Frame, sizeof(s.frame));
            s.frameId = FrameId.load(std::memory_order_relaxed);
            s.poseSequence = PoseSequence.load(std::memory_order_relaxed);
            s.presentQpc = PresentQpc.load(std::memory_order_relaxed);
            s.sourceCaptureQpc = SourceCaptureQpc.load(std::memory_order_relaxed);
            s.publishedAtMs = PublishedAtMs.load(std::memory_order_relaxed);
            s.kind = static_cast<SourceKind>(Kind.load(std::memory_order_relaxed));

            const std::uint32_t after = PublishSequence.load(std::memory_order_acquire);
            if (before == after && !(after & 1u))
            {
                out = s;
                return true;
            }
        }
        return false;
    }

    inline bool IsFresh(const Snapshot& s,
        std::uint64_t nowMs = GetTickCount64()) noexcept
    {
        return s.kind != SourceKind::None && s.frameId != 0 &&
            s.frame.frameId == s.frameId &&
            s.frame.sourcePoseSequence == s.poseSequence &&
            s.frame.presentQpc == s.presentQpc &&
            s.publishedAtMs != 0 && nowMs >= s.publishedAtMs &&
            nowMs - s.publishedAtMs <= MaxPresentationAgeMs;
    }

    inline bool ReadFresh(Snapshot& out) noexcept
    {
        return Read(out) && IsFresh(out);
    }

    inline bool Matches(const OutRunVR::SharedRenderFrameState& frame,
        SourceKind requiredKind, Snapshot* snapshot = nullptr) noexcept
    {
        Snapshot s{};
        if (!ReadFresh(s)) return false;
        const bool match = s.kind == requiredKind && s.frameId == frame.frameId &&
            s.poseSequence == frame.sourcePoseSequence && s.presentQpc == frame.presentQpc;
        if (match && snapshot) *snapshot = s;
        return match;
    }
}
