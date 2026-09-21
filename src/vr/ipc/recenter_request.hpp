#pragma once

// Cross-process F10 recenter request channel shared by the 32-bit game hook and
// the 64-bit OpenXR host. This is intentionally independent of the v2/v3 pose
// ABI so a user-input control message cannot perturb render metadata layout.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>

namespace OutRunVR::RecenterIpc
{
    inline constexpr wchar_t MappingName[] =
        L"Local\\OutRun2006Tweaks.VR.Recenter.v1";
    inline constexpr std::uint32_t Magic = 0x5243564Fu; // 'OVCR'
    inline constexpr std::uint32_t Version = 1;

    struct State
    {
        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint32_t structSize = 0;
        volatile LONG requestId = 0;
        volatile LONG receivedId = 0;
        volatile LONG appliedId = 0;
        volatile LONG requesterPid = 0;
    };
    static_assert(sizeof(State) == 28);

    class Channel
    {
    public:
        bool Ensure() noexcept
        {
            if (state_)
                return state_->magic == Magic && state_->version == Version &&
                    state_->structSize == sizeof(State);

            HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                PAGE_READWRITE, 0, static_cast<DWORD>(sizeof(State)), MappingName);
            if (!mapping)
                return false;
            const bool created = GetLastError() != ERROR_ALREADY_EXISTS;

            auto* state = static_cast<State*>(MapViewOfFile(mapping,
                FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
            if (!state)
            {
                CloseHandle(mapping);
                return false;
            }

            if (created)
            {
                ZeroMemory(state, sizeof(State));
                state->magic = Magic;
                state->version = Version;
                state->structSize = sizeof(State);
                MemoryBarrier();
            }
            else if (state->magic != Magic || state->version != Version ||
                state->structSize != sizeof(State))
            {
                UnmapViewOfFile(state);
                CloseHandle(mapping);
                return false;
            }

            mapping_ = mapping;
            state_ = state;
            return true;
        }

        LONG Publish() noexcept
        {
            if (!Ensure())
                return 0;
            InterlockedExchange(&state_->requesterPid,
                static_cast<LONG>(GetCurrentProcessId()));
            MemoryBarrier();
            return InterlockedIncrement(&state_->requestId);
        }

        bool Pending(LONG& requestId, DWORD& requesterPid) noexcept
        {
            requestId = 0;
            requesterPid = 0;
            if (!Ensure())
                return false;
            const LONG requested = InterlockedCompareExchange(
                &state_->requestId, 0, 0);
            const LONG received = InterlockedCompareExchange(
                &state_->receivedId, 0, 0);
            if (requested == 0 || requested == received)
                return false;
            requestId = requested;
            requesterPid = static_cast<DWORD>(InterlockedCompareExchange(
                &state_->requesterPid, 0, 0));
            return true;
        }

        void MarkReceived(LONG requestId) noexcept
        {
            if (requestId != 0 && Ensure())
                InterlockedExchange(&state_->receivedId, requestId);
        }

        void MarkApplied(LONG requestId) noexcept
        {
            if (requestId != 0 && Ensure())
                InterlockedExchange(&state_->appliedId, requestId);
        }

        LONG AppliedId() noexcept
        {
            return Ensure() ?
                InterlockedCompareExchange(&state_->appliedId, 0, 0) : 0;
        }

    private:
        HANDLE mapping_ = nullptr;
        State* state_ = nullptr;
    };

    inline Channel& SharedChannel() noexcept
    {
        static Channel channel;
        return channel;
    }
}
