#include "hook_mgr.hpp"

Hook::Hook()
{
	HookManager::RegisterHook(this);
}

void HookManager::ApplyHooks(bool insideDelayedThread)
{
    for (const auto& hook : s_hooks)
    {
        hook->is_active_ = false;
        if (hook->validate())
        {
            hook->is_active_ = hook->apply();
            auto desc = hook->description();
            if (!desc.empty())
                spdlog::info("{}: apply {}", desc, hook->is_active_ ? "successful" : "failed");
        }
    }
}
