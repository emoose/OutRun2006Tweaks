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

    // Declares which settings this hook's behaviour depends on, with
    // Setting::watch for a value it baked in and has to re-do, or
    // Setting::needs_restart for one it can't pick up while the game runs.
    //
    // Called for every hook regardless of validate().
    virtual void declare_settings() {}

    // applies the hook/patch
    virtual bool apply() = 0;

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
public:
    // Keep hooks vector inside function-local static, to ensure vector actually exists
    // before Hooks try to register themselves.
    static std::vector<Hook*>& hooks()
    {
        static std::vector<Hook*> s_hooks;
        return s_hooks;
    }

    static void RegisterHook(Hook* hook)
	{
        hooks().emplace_back(hook);
    }

    static void ApplyHooks();
};
