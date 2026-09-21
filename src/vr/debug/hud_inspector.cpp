#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "hook_mgr.hpp"
#include "plugin.hpp"
#include "game_addrs.hpp"
#include "../hud_semantics.hpp"

namespace Settings
{
    Setting<bool> VRHudInspector{
        "VR", "HudInspector", false,
        "Passive HUD/sprite reverse-engineering trace. Records unique sprite call sites, IDs and coordinates with EXE RVAs so static EXE analysis can map them back to game functions."
    };
}

namespace OutRunVRHudInspector
{
    namespace
    {
        enum class EventKind : std::uint32_t
        {
            PutSprite = 1,
            SpriteAnim = 2,
            ClipSprite = 3,
            PutSprite2 = 4
        };

        SafetyHookInline SpriteAnimHook{};
        SafetyHookInline ClipSpriteHook{};

        std::mutex TraceMutex;
        std::ofstream TraceFile;
        std::unordered_map<std::uint64_t, std::uint32_t> Seen;
        std::uint64_t TraceLines = 0;
        ULONGLONG StartMs = 0;

        std::uint32_t ExeSizeOfImage() noexcept
        {
            if (!Module::ExeHandle)
                return 0;
            const auto* base = reinterpret_cast<const std::uint8_t*>(Module::ExeHandle);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return 0;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return 0;
            return nt->OptionalHeader.SizeOfImage;
        }

        std::uint32_t ToExeRva(const void* address) noexcept
        {
            const auto base = reinterpret_cast<std::uintptr_t>(Module::ExeHandle);
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            const auto size = static_cast<std::uintptr_t>(ExeSizeOfImage());
            if (!base || !value || value < base || !size || value >= base + size)
                return 0;
            return static_cast<std::uint32_t>(value - base);
        }

        std::uint64_t MakeKey(EventKind kind, std::uint32_t callRva,
            std::uint32_t arg0, std::uint32_t arg1) noexcept
        {
            std::uint64_t h = 1469598103934665603ull;
            auto mix = [&h](std::uint32_t v) {
                h ^= v;
                h *= 1099511628211ull;
            };
            mix(static_cast<std::uint32_t>(kind));
            mix(callRva);
            mix(arg0);
            mix(arg1);
            return h;
        }

        bool ShouldWrite(std::uint64_t key, std::uint32_t& count)
        {
            count = ++Seen[key];
            return count == 1 || (count <= 1024 && (count & (count - 1)) == 0);
        }

        int CurrentMode() noexcept
        {
            return Game::current_mode ? static_cast<int>(*Game::current_mode) : -1;
        }

        int CurrentStage() noexcept
        {
            return Game::stg_stage_num ? static_cast<int>(*Game::stg_stage_num) : -1;
        }

        void WriteEvent(EventKind kind, const char* eventName,
            const void* returnAddress,
            std::uint32_t arg0, std::uint32_t arg1,
            double arg2 = 0.0, double arg3 = 0.0,
            double arg4 = 0.0, double arg5 = 0.0,
            double arg6 = 0.0, double arg7 = 0.0)
        {
            if (!TraceFile)
                return;

            const std::uint32_t returnRva = ToExeRva(returnAddress);
            const std::uint32_t callRva =
                returnRva >= 5 ? returnRva - 5 : returnRva;
            const std::uint64_t key = MakeKey(kind, callRva, arg0, arg1);

            std::lock_guard lock(TraceMutex);
            std::uint32_t count = 0;
            if (!ShouldWrite(key, count))
                return;

            const auto semantic =
                OutRunVRHudSemantics::ClassifyCaller(callRva);

            TraceFile
                << (GetTickCount64() - StartMs) << ','
                << eventName << ','
                << "0x" << std::hex << std::setw(8) << std::setfill('0') << returnRva << ','
                << "0x" << std::setw(8) << callRva << std::dec << ','
                << semantic.area << ','
                << semantic.semantic << ','
                << OutRunVRHudSemantics::SpacePolicyName(semantic.space) << ','
                << CurrentMode() << ','
                << CurrentStage() << ','
                << arg0 << ','
                << arg1 << ','
                << arg2 << ','
                << arg3 << ','
                << arg4 << ','
                << arg5 << ','
                << arg6 << ','
                << arg7 << ','
                << count << '\n';

            if ((++TraceLines & 63ull) == 0)
                TraceFile.flush();
        }

        int __cdecl SpriteAnimDest(std::uint32_t spriteId, float x, float y,
            int a4, int a5, float alpha)
        {
            WriteEvent(EventKind::SpriteAnim, "sprani_play_ae_auth_alpha",
                _ReturnAddress(), spriteId,
                static_cast<std::uint32_t>(a4),
                x, y, static_cast<double>(a5), alpha);
            return SpriteAnimHook.unsafe_ccall<int>(
                spriteId, x, y, a4, a5, alpha);
        }

        int __cdecl ClipSpriteDest(int xstnum, int x, int y,
            std::uint32_t flags, float priority, std::uint32_t color)
        {
            WriteEvent(EventKind::ClipSprite, "put_clip_sprite",
                _ReturnAddress(),
                static_cast<std::uint32_t>(xstnum), flags,
                static_cast<double>(x), static_cast<double>(y),
                priority, static_cast<double>(color));
            return ClipSpriteHook.unsafe_ccall<int>(
                xstnum, x, y, flags, priority, color);
        }

        bool OpenTrace()
        {
            try
            {
                const auto path =
                    Module::DllPath.parent_path() /
                    "OutRun2006Tweaks-hudtrace.csv";
                const bool empty =
                    !std::filesystem::exists(path) ||
                    std::filesystem::file_size(path) == 0;

                TraceFile.open(path, std::ios::out | std::ios::app);
                if (!TraceFile)
                    return false;

                StartMs = GetTickCount64();
                if (empty)
                {
                    TraceFile
                        << "# schema=outrun-hudtrace-v2\n"
                        << "# exe_timestamp="
                        << Util::GetModuleTimestamp(Module::ExeHandle) << "\n"
                        << "# exe_size_of_image=" << ExeSizeOfImage() << "\n"
                        << "# module_base=runtime-only; all addresses below are ASLR-safe RVAs\n"
                        << "elapsed_ms,event,return_rva,call_rva,known_area,semantic,space_policy,mode,stage,"
                           "arg0,arg1,arg2,arg3,arg4,arg5,arg6,arg7,count\n";
                    TraceFile.flush();
                }
                return true;
            }
            catch (const std::exception& e)
            {
                spdlog::warn("VR HUD INSPECTOR: failed to open trace: {}", e.what());
                return false;
            }
        }

        void ResetHooks() noexcept
        {
            ClipSpriteHook = {};
            SpriteAnimHook = {};
        }

    }

    // Feed points used by the existing texture hooks. Keeping put_sprite_ex on
    // its already-established hook avoids stacking two inline detours on the
    // same game function and preserves the real game caller return address.
    void TracePutSprite(SPRARGS* sprargs, float priority,
        const void* returnAddress)
    {
        if (!sprargs)
            return;
        WriteEvent(EventKind::PutSprite, "put_sprite_ex", returnAddress,
            sprargs->xstnum_0, sprargs->top_4,
            sprargs->left_8, sprargs->bottom_C,
            sprargs->right_10, sprargs->scaleX,
            sprargs->scaleY, priority);
    }

    void TracePutSprite2(SPRARGS2* sprargs, float priority,
        const void* returnAddress)
    {
        if (!sprargs)
            return;
        WriteEvent(EventKind::PutSprite2, "put_sprite_ex2", returnAddress,
            sprargs->xstnum_0, sprargs->child_B4 ? 1u : 0u,
            sprargs->color_4,
            sprargs->TopLeft_54.x, sprargs->TopLeft_54.y,
            sprargs->BottomRight_78.x, sprargs->BottomRight_78.y,
            priority);
    }

    namespace
    {
        class VRHudInspectorHook : public Hook
        {
        public:
            std::string_view description() override
            {
                return "OpenXRVRHudInspector";
            }

            bool validate() override
            {
                return Settings::VREnabled.get() &&
                    Settings::VRHudInspector.get();
            }

            void declare_settings() override
            {
                Settings::VRHudInspector.needs_restart();
            }

            bool apply() override
            {
                if (!OpenTrace())
                    return false;

                SpriteAnimHook = safetyhook::create_inline(
                    Game::sprani_play_ae_auth_alpha, SpriteAnimDest);
                ClipSpriteHook = safetyhook::create_inline(
                    Game::put_clip_sprite, ClipSpriteDest);

                if (!SpriteAnimHook || !ClipSpriteHook)
                {
                    spdlog::error(
                        "VR HUD INSPECTOR: one or more sprite hooks failed; disabling inspector");
                    ResetHooks();
                    return false;
                }

                spdlog::info(
                    "VR HUD INSPECTOR: semantic HUD tracing active; UIScaling-derived caller taxonomy separates screen HUD from world billboards; output=OutRun2006Tweaks-hudtrace.csv");
                return true;
            }

            static VRHudInspectorHook instance;
        };

        VRHudInspectorHook VRHudInspectorHook::instance;
    }
}