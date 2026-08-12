#pragma once

#include <vector>
#include <memory>
#include <functional>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <safetyhook.hpp>

#include <MemoryMgr.h>
#include <Patterns.h>

// Base class for hooks
class Hook
{
    friend class HookManager;

public:
    Hook();

    virtual ~Hook() = default;

    // name/description of hook, for debug logging/tracing
    virtual std::string_view description() { return ""; }

    // check if user has enabled this hook, and any prerequisites are satisfied
    virtual bool validate() { return true; }

    // applies the hook/patch
    virtual bool apply() = 0;

    // called when settings have been changed
    // returns true if settings are applied, false if a game restart is required
    virtual bool settings_changed() { return true; }

    bool active()
    {
        return is_active_;
    }

    bool error()
    {
        return has_error_;
    }

private:
    bool is_active_ = false;
    bool has_error_ = false;
};

// Static HookManager class
class HookManager
{
private:
    // Keep hooks vector inside function-local static, to ensure vector actually exists
    // before Hooks try to register themselves.
    static std::vector<Hook*>& hooks()
    {
        static std::vector<Hook*> s_hooks;
        return s_hooks;
    }

public:
    static void RegisterHook(Hook* hook)
	{
        hooks().emplace_back(hook);
    }

    static bool SettingsChanged()
    {
        bool restartNeeded = false;

        for (Hook* hook : hooks())
            if (hook)
                restartNeeded |= !hook->settings_changed();

        return restartNeeded;
    }

    static void ApplyHooks();
};
