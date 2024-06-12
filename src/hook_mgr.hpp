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
    virtual std::string_view description() = 0;

    // check if user has enabled this hook, and any prerequisites are satisfied
    virtual bool validate() = 0;

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
private:
    inline static std::vector<Hook*> s_hooks;
	
public:
    static void RegisterHook(Hook* hook)
	{
        s_hooks.emplace_back(hook);
    }

    static void ApplyHooks();
};
