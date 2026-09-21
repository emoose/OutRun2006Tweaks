#pragma once
// Scheduled-review baseline marker: UIScaling-derived VR HUD semantics.

#include <array>
#include <cstdint>

namespace OutRunVRHudSemantics
{
    enum class SpacePolicy : std::uint8_t
    {
        Unknown,
        ScreenHud,
        WorldBillboard
    };

    struct SemanticInfo
    {
        const char* area;
        const char* semantic;
        SpacePolicy space;
    };

    struct SemanticAnchor
    {
        std::uint32_t rva;
        const char* semantic;
        SpacePolicy space;
        const char* note;
    };

    constexpr const char* SpacePolicyName(SpacePolicy policy) noexcept
    {
        switch (policy)
        {
        case SpacePolicy::ScreenHud: return "SCREEN_HUD";
        case SpacePolicy::WorldBillboard: return "WORLD_BILLBOARD";
        default: return "UNKNOWN";
        }
    }

    constexpr bool InRange(
        std::uint32_t value, std::uint32_t begin,
        std::uint32_t end) noexcept
    {
        return value >= begin && value < end;
    }

    constexpr SemanticInfo UnknownInfo() noexcept
    {
        return { "", "UNKNOWN", SpacePolicy::Unknown };
    }

    // Semantic ranges are deliberately based on the already-shipped
    // hooks_uiscaling.cpp reverse-engineering instead of D3D primitive-count
    // guesses. Screen HUD remains zero-disparity/common-centre VR content.
    // World-attached rival/heart markers remain true stereo billboards.
    constexpr SemanticInfo ClassifyCaller(std::uint32_t callRva) noexcept
    {
        // HAM attached-heart draw; anchored by HeartDisp_PulseAngle=0x05B43A.
        if (InRange(callRva, 0x05B300, 0x05B700))
            return { "HeartDisp_car_heart", "WORLD_HEART", SpacePolicy::WorldBillboard };

        // C2C mission HUD anchors present in the original UI-scaling fixes.
        if (InRange(callRva, 0x081A00, 0x081B00))
            return { "C2C_Fruit", "HUD_FRUIT", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x081B00, 0x081C00))
            return { "C2C_Heart", "HUD_HEART_TOTAL", SpacePolicy::ScreenHud };

        if (InRange(callRva, 0x096A80, 0x096D00))
            return { "C2CSpeechBubble", "HUD_GF_SPEECH", SpacePolicy::ScreenHud };

        if (InRange(callRva, 0x0B9000, 0x0B9200))
            return { "DispGearPosition", "HUD_GEAR_REV", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0B9E00, 0x0BA100))
            return { "DispRank", "HUD_RANK", SpacePolicy::ScreenHud };

        // sub_4BAD20: position markers projected from rival-car world position.
        if (InRange(callRva, 0x0BAD20, 0x0BB320))
            return { "RankMarker/sub_4BAD20", "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard };

        if (InRange(callRva, 0x0BD2E0, 0x0BD360))
            return { "C2CTestSlipstream", "HUD_SLIPSTREAM", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0BD360, 0x0BD500))
            return { "C2CDontLoseGF", "HUD_GF_WARNING", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0BD900, 0x0BE100))
            return { "GhostGap", "HUD_GHOST", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0BE300, 0x0BEA40))
            return { "DispTimeAttack2D", "HUD_TIME_ATTACK", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0BEA40, 0x0BEB20))
            return { "NaviPub_DispTimeAttackGoal", "HUD_GOAL_TIME", SpacePolicy::ScreenHud };

        // NaviPub_Disp interleaves Rival and Heart groups, so keep the known
        // sub-ranges split instead of classifying the whole function as one HUD.
        if (InRange(callRva, 0x0BEB60, 0x0BEBC0) ||
            InRange(callRva, 0x0BEC50, 0x0BECA0))
            return { "NaviPub_Disp_Rival", "HUD_RIVAL", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0BEBC0, 0x0BEC50) ||
            InRange(callRva, 0x0BECA0, 0x0BED20))
            return { "NaviPub_Disp_Heart", "HUD_HEART_TOTAL", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0BED20, 0x0BEE80))
            return { "NaviPub_Disp", "HUD_NAV_GENERIC", SpacePolicy::ScreenHud };

        if (InRange(callRva, 0x0FC800, 0x0FC8A0))
            return { "C2CSpeechBubbleGF_RankEmoji", "HUD_RANK_EMOJI", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0FC8A0, 0x0FC900))
            return { "C2CSpeechBubbleGF_RankText", "HUD_RANK_TEXT", SpacePolicy::ScreenHud };
        if (InRange(callRva, 0x0FC900, 0x0FCC00) ||
            InRange(callRva, 0x0FCD80, 0x0FD080) ||
            InRange(callRva, 0x0FD560, 0x0FD680) ||
            InRange(callRva, 0x0FE860, 0x0FE900))
            return { "C2CSpeechBubbleGF", "HUD_GF_SPEECH", SpacePolicy::ScreenHud };

        return UnknownInfo();
    }

    // Exact reverse-engineered anchor inventory from hooks_uiscaling.cpp.
    // This is a review/test source-of-truth, even where the anchor itself is
    // not a direct sprite call and therefore may not appear in hudtrace.csv.
    inline constexpr std::array<SemanticAnchor, 50> Anchors{{
        {0x05B43A, "WORLD_HEART", SpacePolicy::WorldBillboard, "HeartDisp_car_heart pulse angle"},
        {0x081A86, "HUD_FRUIT", SpacePolicy::ScreenHud, "C2C fruit scaling disable"},
        {0x081A8B, "HUD_FRUIT", SpacePolicy::ScreenHud, "C2C fruit scaling enable"},
        {0x081B76, "HUD_HEART_TOTAL", SpacePolicy::ScreenHud, "C2C heart scaling disable"},

        {0x0B9096, "HUD_GEAR_REV", SpacePolicy::ScreenHud, "REV left adjustment #1"},
        {0x0B90B3, "HUD_GEAR_REV", SpacePolicy::ScreenHud, "REV left adjustment #2"},
        {0x0B90F6, "HUD_GEAR_REV", SpacePolicy::ScreenHud, "REV left adjustment #3"},

        {0x0B9F3A, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #1"},
        {0x0B9F5E, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #2"},
        {0x0B9F81, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #3"},
        {0x0B9FD0, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #4"},
        {0x0B9FFC, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #5"},
        {0x0BA01E, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #6"},
        {0x0BA035, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #7"},
        {0x0BA052, "HUD_RANK", SpacePolicy::ScreenHud, "DispRank adjustment #8"},

        {0x0BB0FB, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker sprani #1"},
        {0x0BB133, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker sprani #2"},
        {0x0BB16C, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker sprani #3"},
        {0x0BB1A5, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker sprani #4"},
        {0x0BB21F, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker clip #1"},
        {0x0BB241, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker clip #2"},
        {0x0BB271, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker clip #3"},
        {0x0BB2BC, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker clip #4"},
        {0x0BB2D0, "WORLD_RIVAL_MARKER", SpacePolicy::WorldBillboard, "rank marker clip #5"},

        {0x0BD32E, "HUD_SLIPSTREAM", SpacePolicy::ScreenHud, "test your slipstream"},
        {0x0BD397, "HUD_GF_WARNING", SpacePolicy::ScreenHud, "don't lose girlfriend #1"},
        {0x0BD414, "HUD_GF_WARNING", SpacePolicy::ScreenHud, "don't lose girlfriend #2"},
        {0x0BD472, "HUD_GF_WARNING", SpacePolicy::ScreenHud, "don't lose girlfriend #3"},
        {0x0BDAE8, "HUD_GHOST", SpacePolicy::ScreenHud, "Ghost/You/Diff sub adjustment"},
        {0x0BDE3A, "HUD_GHOST", SpacePolicy::ScreenHud, "Ghost/You/Diff adjustment"},

        {0x0BE4BC, "HUD_TIME_ATTACK", SpacePolicy::ScreenHud, "TimeAttack force right"},
        {0x0BE4E7, "HUD_TIME_ATTACK", SpacePolicy::ScreenHud, "TimeAttack force left"},
        {0x0BE5CD, "HUD_TIME_ATTACK", SpacePolicy::ScreenHud, "TimeAttack scroll #1"},
        {0x0BE8D8, "HUD_TIME_ATTACK", SpacePolicy::ScreenHud, "TimeAttack scroll later group"},
        {0x0BEA64, "HUD_GOAL_TIME", SpacePolicy::ScreenHud, "TimeAttack goal"},
        {0x0BEB8E, "HUD_RIVAL", SpacePolicy::ScreenHud, "Rival scaling disable"},
        {0x0BEBAF, "HUD_RIVAL", SpacePolicy::ScreenHud, "Rival scaling enable"},
        {0x0BEBE1, "HUD_HEART_TOTAL", SpacePolicy::ScreenHud, "Heart total disable"},
        {0x0BEBE6, "HUD_HEART_TOTAL", SpacePolicy::ScreenHud, "Heart total enable"},
        {0x0BEC83, "HUD_RIVAL", SpacePolicy::ScreenHud, "Rival online disable"},
        {0x0BEC88, "HUD_RIVAL", SpacePolicy::ScreenHud, "Rival online enable"},
        {0x0BECBA, "HUD_HEART_TOTAL", SpacePolicy::ScreenHud, "C2C heart enable"},
        {0x0BECE0, "HUD_HEART_TOTAL", SpacePolicy::ScreenHud, "C2C heart enable #2"},

        {0x0FC84E, "HUD_RANK_EMOJI", SpacePolicy::ScreenHud, "ranking emoji #1"},
        {0x0FC882, "HUD_RANK_EMOJI", SpacePolicy::ScreenHud, "ranking emoji #2"},
        {0x0FC8B4, "HUD_RANK_TEXT", SpacePolicy::ScreenHud, "rank: aaa text"},
        {0x0FC9EB, "HUD_GF_SPEECH", SpacePolicy::ScreenHud, "speech initial position #1"},
        {0x0FCA1E, "HUD_GF_SPEECH", SpacePolicy::ScreenHud, "speech initial position #2"},
        {0x0FCA51, "HUD_GF_SPEECH", SpacePolicy::ScreenHud, "speech initial position #3"},
        {0x0FCB20, "HUD_GF_SPEECH", SpacePolicy::ScreenHud, "speech initial position #4"},
    }};

    static_assert(ClassifyCaller(0x0B9F3A).space == SpacePolicy::ScreenHud);
    static_assert(ClassifyCaller(0x0BB0FB).space == SpacePolicy::WorldBillboard);
    static_assert(ClassifyCaller(0x0BE5CD).space == SpacePolicy::ScreenHud);
    static_assert(ClassifyCaller(0x0FC84E).space == SpacePolicy::ScreenHud);
}