#pragma once

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace OutRunVR::Ipc
{
    inline std::uint32_t BeginSeqlockWrite(volatile std::uint32_t& sequence) noexcept
    {
        LONG value = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&sequence));
        if ((value & 1) == 0)
            value = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&sequence));
        MemoryBarrier();
        return static_cast<std::uint32_t>(value);
    }

    inline std::uint32_t EndSeqlockWrite(volatile std::uint32_t& sequence) noexcept
    {
        MemoryBarrier();
        LONG value = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&sequence));
        if (value & 1)
            value = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&sequence));
        return static_cast<std::uint32_t>(value);
    }

    template <typename T>
    bool StableRead(const T* shared, T& out, int attempts = 6) noexcept
    {
        if (!shared || attempts <= 0)
            return false;

        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            const std::uint32_t before = shared->sequence;
            if (before & 1u)
                continue;

            MemoryBarrier();
            std::memcpy(&out, shared, sizeof(T));
            MemoryBarrier();

            const std::uint32_t after = shared->sequence;
            if (before == after && !(after & 1u))
                return true;
        }
        return false;
    }

    template <typename T>
    class ReadOnlyMapping
    {
    public:
        ReadOnlyMapping() = default;
        ReadOnlyMapping(const ReadOnlyMapping&) = delete;
        ReadOnlyMapping& operator=(const ReadOnlyMapping&) = delete;

        ReadOnlyMapping(ReadOnlyMapping&& other) noexcept
            : mapping_(std::exchange(other.mapping_, nullptr)),
              state_(std::exchange(other.state_, nullptr))
        {
        }

        ReadOnlyMapping& operator=(ReadOnlyMapping&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                mapping_ = std::exchange(other.mapping_, nullptr);
                state_ = std::exchange(other.state_, nullptr);
            }
            return *this;
        }

        ~ReadOnlyMapping()
        {
            Reset();
        }

        bool EnsureOpen(const wchar_t* name) noexcept
        {
            if (state_)
                return true;
            if (!name || !*name)
                return false;

            HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
            if (!mapping)
                return false;

            const T* state = static_cast<const T*>(MapViewOfFile(
                mapping, FILE_MAP_READ, 0, 0, sizeof(T)));
            if (!state)
            {
                CloseHandle(mapping);
                return false;
            }

            mapping_ = mapping;
            state_ = state;
            return true;
        }

        void Reset() noexcept
        {
            if (state_)
            {
                UnmapViewOfFile(state_);
                state_ = nullptr;
            }
            if (mapping_)
            {
                CloseHandle(mapping_);
                mapping_ = nullptr;
            }
        }

        const T* Get() const noexcept
        {
            return state_;
        }

        bool IsOpen() const noexcept
        {
            return state_ != nullptr;
        }

    private:
        HANDLE mapping_ = nullptr;
        const T* state_ = nullptr;
    };
}
