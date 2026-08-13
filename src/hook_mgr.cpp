#include "hook_mgr.hpp"

Hook::Hook()
{
	HookManager::RegisterHook(this);
}

void HookManager::ApplyHooks()
{
    for (const auto& hook : hooks())
    {
        hook->is_active_ = false;
        if (hook->validate())
        {
            hook->is_active_ = hook->apply();

            // Separates a hook that failed to apply from one the user turned
            // off, which the overlay's hook list shows differently.
            hook->has_error_ = !hook->is_active_;

            auto desc = hook->description();
            if (!desc.empty())
            {
                spdlog::log(hook->is_active_ ?
                    spdlog::level::info : spdlog::level::err,
                    "{}: apply {}", desc, hook->is_active_ ? "successful" : "failed");
            }
        }
    }
}
