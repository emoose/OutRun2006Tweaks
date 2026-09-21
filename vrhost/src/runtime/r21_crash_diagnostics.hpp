#pragma once

// R23 crash evidence for the x64 OpenXR host.
// The minimal fault record is written and flushed before MiniDumpWriteDump is
// attempted, so an in-process dump deadlock cannot erase the exception/module
// evidence. Dump success/failure is appended afterward when control returns.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <DbgHelp.h>

#include <cstdint>
#include <cstdio>
#include <cwchar>

namespace OutRunVrR21CrashDiagnostics
{
    inline constexpr const char* BuildId = "R23-crash-text-first-20260916";

    struct DumpResult
    {
        bool success = false;
        DWORD error = ERROR_SUCCESS;
        wchar_t path[MAX_PATH]{};
    };

    inline void BuildSiblingPath(const wchar_t* fileName,
        wchar_t out[MAX_PATH]) noexcept
    {
        out[0] = L'\0';
        DWORD n = GetModuleFileNameW(nullptr, out, MAX_PATH);
        if (!n || n >= MAX_PATH)
        {
            wcsncpy_s(out, MAX_PATH, fileName, _TRUNCATE);
            return;
        }
        wchar_t* slash = wcsrchr(out, L'\\');
        if (!slash)
        {
            wcsncpy_s(out, MAX_PATH, fileName, _TRUNCATE);
            return;
        }
        *(slash + 1) = L'\0';
        wcsncat_s(out, MAX_PATH, fileName, _TRUNCATE);
    }

    inline void BuildCrashBaseName(wchar_t out[128]) noexcept
    {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        _snwprintf_s(out, 128, _TRUNCATE,
            L"outrun-vr-host-crash-%04u%02u%02u-%02u%02u%02u-%03u-pid%lu",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond),
            static_cast<unsigned>(st.wMilliseconds),
            GetCurrentProcessId());
    }

    inline void BuildFaultTextPath(const wchar_t* baseName,
        wchar_t path[MAX_PATH]) noexcept
    {
        wchar_t fileName[160]{};
        _snwprintf_s(fileName, 160, _TRUNCATE, L"%s.txt", baseName);
        BuildSiblingPath(fileName, path);
    }

    inline void WriteInitialFaultText(EXCEPTION_POINTERS* ep,
        const wchar_t* baseName) noexcept
    {
        wchar_t path[MAX_PATH]{};
        BuildFaultTextPath(baseName, path);
        HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;

        const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
        const void* exceptionAddress = er ? er->ExceptionAddress : nullptr;
        MEMORY_BASIC_INFORMATION mbi{};
        HMODULE module = nullptr;
        if (exceptionAddress &&
            VirtualQuery(exceptionAddress, &mbi, sizeof(mbi)) == sizeof(mbi))
            module = static_cast<HMODULE>(mbi.AllocationBase);

        wchar_t modulePath[MAX_PATH]{};
        if (module)
            GetModuleFileNameW(module, modulePath, MAX_PATH);

        char moduleUtf8[MAX_PATH * 3]{};
        if (modulePath[0])
            WideCharToMultiByte(CP_UTF8, 0, modulePath, -1,
                moduleUtf8, static_cast<int>(sizeof(moduleUtf8)), nullptr, nullptr);

        const std::uintptr_t address =
            reinterpret_cast<std::uintptr_t>(exceptionAddress);
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(module);
        const std::uintptr_t rva = base && address >= base ? address - base : 0;

        unsigned long long operation = ~0ull;
        unsigned long long target = 0;
        if (er && er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            er->NumberParameters >= 2)
        {
            operation = static_cast<unsigned long long>(er->ExceptionInformation[0]);
            target = static_cast<unsigned long long>(er->ExceptionInformation[1]);
        }

        char text[2560]{};
        const int len = _snprintf_s(text, sizeof(text), _TRUNCATE,
            "build=%s\r\n"
            "pid=%lu tid=%lu\r\n"
            "exception=0x%08lX\r\n"
            "exceptionAddress=0x%016llX\r\n"
            "module=%s\r\n"
            "moduleBase=0x%016llX\r\n"
            "rva=0x%llX\r\n"
            "avOperation=%llu (0=read,1=write,8=execute)\r\n"
            "avTarget=0x%016llX\r\n"
            "dumpStatus=pending\r\n",
            BuildId,
            GetCurrentProcessId(), GetCurrentThreadId(),
            er ? static_cast<unsigned long>(er->ExceptionCode) : 0ul,
            static_cast<unsigned long long>(address),
            moduleUtf8[0] ? moduleUtf8 : "<unknown>",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(rva),
            operation, target);

        if (len > 0)
        {
            DWORD written = 0;
            WriteFile(file, text, static_cast<DWORD>(len), &written, nullptr);
            // This flush intentionally happens before MiniDumpWriteDump. If the
            // dump call deadlocks, the minimum exception record is still durable.
            FlushFileBuffers(file);
        }
        CloseHandle(file);
    }

    inline DumpResult WriteMiniDump(EXCEPTION_POINTERS* ep,
        const wchar_t* baseName) noexcept
    {
        DumpResult result{};
        wchar_t fileName[160]{};
        _snwprintf_s(fileName, 160, _TRUNCATE, L"%s.dmp", baseName);
        BuildSiblingPath(fileName, result.path);

        HANDLE file = CreateFileW(result.path, GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            result.error = GetLastError();
            return result;
        }

        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
        exceptionInfo.ThreadId = GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = ep;
        exceptionInfo.ClientPointers = FALSE;

        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo |
            MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithUnloadedModules);
        SetLastError(ERROR_SUCCESS);
        const BOOL ok = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), file,
            type, ep ? &exceptionInfo : nullptr, nullptr, nullptr);
        result.success = ok != FALSE;
        result.error = result.success ? ERROR_SUCCESS : GetLastError();
        FlushFileBuffers(file);
        CloseHandle(file);
        return result;
    }

    inline void AppendDumpResult(const wchar_t* baseName,
        const DumpResult& dump) noexcept
    {
        wchar_t path[MAX_PATH]{};
        BuildFaultTextPath(baseName, path);
        HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;

        char dumpUtf8[MAX_PATH * 3]{};
        if (dump.path[0])
            WideCharToMultiByte(CP_UTF8, 0, dump.path, -1,
                dumpUtf8, static_cast<int>(sizeof(dumpUtf8)), nullptr, nullptr);

        char text[1024]{};
        const int len = _snprintf_s(text, sizeof(text), _TRUNCATE,
            "dumpStatus=completed\r\n"
            "dumpSuccess=%u\r\n"
            "dumpError=%lu\r\n"
            "dumpPath=%s\r\n",
            dump.success ? 1u : 0u,
            static_cast<unsigned long>(dump.error),
            dumpUtf8[0] ? dumpUtf8 : "<not-created>");
        if (len > 0)
        {
            DWORD written = 0;
            WriteFile(file, text, static_cast<DWORD>(len), &written, nullptr);
            FlushFileBuffers(file);
        }
        CloseHandle(file);
    }

    inline LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* ep) noexcept
    {
        wchar_t baseName[128]{};
        BuildCrashBaseName(baseName);
        WriteInitialFaultText(ep, baseName);
        const DumpResult dump = WriteMiniDump(ep, baseName);
        AppendDumpResult(baseName, dump);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    struct Installer
    {
        Installer() noexcept
        {
            SetUnhandledExceptionFilter(UnhandledExceptionFilter);
        }
    };

    inline Installer InstallOnce{};
}
