#pragma once

#include <atomic>
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

    static TogglePatch nop(void* address, size_t size)
    {
        return TogglePatch(address, std::vector<uint8_t>(size, 0x90));
    }

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

        const std::vector<uint8_t>& bytes = enabled ? patched_ : original_;
        for (size_t i = 0; i < bytes.size(); i++)
            Memory::VP::Patch<uint8_t>(address_ + i, bytes[i]);

        applied_ = enabled;
    }

    bool applied() const { return applied_; }
};

class Hook
{
    friend class HookManager;

public:
    Hook();

    virtual ~Hook() = default;
    virtual std::string_view description() { return ""; }
    virtual bool validate() { return true; }
    virtual void declare_settings() {}
    virtual bool apply() = 0;

    bool active() const noexcept
    {
        return is_active_.load(std::memory_order_acquire);
    }

    bool error() const noexcept
    {
        return has_error_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> is_active_{false};
    std::atomic<bool> has_error_{false};
};

class HookManager
{
public:
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

    // Hooks whose apply() merely starts an installer worker can report their
    // eventual Ready/Failed result back to the overlay/UI. Atomics keep this
    // race-free with status readers on the game thread.
    static void ReportAsyncResult(std::string_view description, bool active);
};
