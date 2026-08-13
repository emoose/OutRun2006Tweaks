#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <cstring>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <safetyhook.hpp>

#include <MemoryMgr.h>
#include <Patterns.h>

// A byte patch that can be toggled back to default.
class TogglePatch
{
    uint8_t* address_ = nullptr;
    std::vector<uint8_t> original_;
    std::vector<uint8_t> patched_;
    bool applied_ = false;

public:
    TogglePatch() = default;

    TogglePatch(void* address, std::vector<uint8_t> patched)
    {
        init(address, std::move(patched));
    }

    // Replaces the instructions at the address with a run of nops.
    static TogglePatch nop(void* address, size_t size)
    {
        return TogglePatch(address, std::vector<uint8_t>(size, 0x90));
    }

    // For patching a pointer or an immediate rather than instructions.
    template <typename T>
    static TogglePatch value(void* address, const T& v)
    {
        std::vector<uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &v, sizeof(T));
        return TogglePatch(address, std::move(bytes));
    }

    void init(void* address, std::vector<uint8_t> patched)
    {
        address_ = static_cast<uint8_t*>(address);
        patched_ = std::move(patched);
        original_.assign(address_, address_ + patched_.size());
        applied_ = false;
    }

    void set(bool enabled)
    {
        if (!address_ || enabled == applied_)
            return;

        // Written a byte at a time so the size can be decided at runtime, which
        // Memory::VP::Patch's initializer_list form can't do.
        const std::vector<uint8_t>& bytes = enabled ? patched_ : original_;
        for (size_t i = 0; i < bytes.size(); i++)
            Memory::VP::Patch<uint8_t>(address_ + i, bytes[i]);

        applied_ = enabled;
    }

    bool applied() const { return applied_; }
};

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
