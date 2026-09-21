#include "hook_mgr.hpp"
#include "plugin.hpp"

#include <cstdlib>
#include <cstring>


namespace
{
    bool IsVrHookDescription(std::string_view description) noexcept
    {
        return description.starts_with("OpenXRVR") ||
            description.starts_with("OpenXVRR");
    }

    bool VrHooksEnabledForProcess() noexcept
    {
        const char* forceDisabled = std::getenv("OUTRUN_VR_FORCE_DISABLED");
        if (forceDisabled && *forceDisabled &&
            (std::strcmp(forceDisabled, "1") == 0 ||
             _stricmp(forceDisabled, "true") == 0 ||
             _stricmp(forceDisabled, "on") == 0))
        {
            return false;
        }
        return Settings::VREnabled.get();
    }
}

Hook::Hook()
{
    HookManager::RegisterHook(this);
}

void HookManager::ApplyHooks()
{
    for (const auto& hook : hooks())
    {
        hook->declare_settings();

        hook->is_active_.store(false, std::memory_order_release);
        hook->has_error_.store(false, std::memory_order_release);

        const auto desc = hook->description();
        if (IsVrHookDescription(desc) && !VrHooksEnabledForProcess())
        {
            if (!desc.empty())
                spdlog::info("{}: skipped because VR is disabled for this process", desc);
            continue;
        }

        if (hook->validate())
        {
            const bool active = hook->apply();
            hook->is_active_.store(active, std::memory_order_release);
            hook->has_error_.store(!active, std::memory_order_release);

            const auto desc = hook->description();
            if (!desc.empty())
            {
                spdlog::log(active ? spdlog::level::info : spdlog::level::err,
                    "{}: apply {}", desc, active ? "successful" : "failed");
            }
        }
    }
}

void HookManager::ReportAsyncResult(std::string_view description, bool active)
{
    if (description.empty())
        return;

    for (Hook* hook : hooks())
    {
        if (!hook || hook->description() != description)
            continue;
        hook->is_active_.store(active, std::memory_order_release);
        hook->has_error_.store(!active, std::memory_order_release);
        spdlog::log(active ? spdlog::level::info : spdlog::level::err,
            "{}: async installer {}", description,
            active ? "ready" : "failed");
        return;
    }
}
