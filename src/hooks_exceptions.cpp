#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <filesystem>
#include <ini.h>
#include <exception.hpp>
#include <miniz.h>

#include "hook_mgr.hpp"
#include "plugin.hpp"

// miniz unfortunately doesn't include wchar versions of its functions, but fortunately does allow passing FILE* to it
mz_bool mz_zip_writer_add_file(mz_zip_archive* pZip, const char* pArchive_name, const wchar_t* pSrc_filename, const void* pComment, mz_uint16 comment_size, mz_uint level_and_flags)
{
  MZ_FILE* pSrc_file = NULL;
  mz_uint64 uncomp_size = 0;
  MZ_TIME_T file_modified_time;
  MZ_TIME_T* pFile_time = NULL;
  mz_bool status;

  memset(&file_modified_time, 0, sizeof(file_modified_time));

  _wfopen_s(&pSrc_file, pSrc_filename, L"rb");
  if (!pSrc_file)
    return false;

  _fseeki64(pSrc_file, 0, SEEK_END);
  uncomp_size = _ftelli64(pSrc_file);
  _fseeki64(pSrc_file, 0, SEEK_SET);

  status = mz_zip_writer_add_cfile(pZip, pArchive_name, pSrc_file, uncomp_size, pFile_time, pComment, comment_size, level_and_flags, NULL, 0, NULL, 0);

  fclose(pSrc_file);

  return status;
}

LONG WINAPI CustomUnhandledExceptionFilter(LPEXCEPTION_POINTERS ExceptionInfo)
{
    wchar_t     modulename[MAX_PATH];
    wchar_t     dump_filename[MAX_PATH];
    wchar_t     crash_log_filename[MAX_PATH];
    wchar_t     re4t_log_filename[MAX_PATH];
    wchar_t     save_filename[MAX_PATH];
    wchar_t     zip_filename[MAX_PATH];
    wchar_t     timestamp[128];
    wchar_t*    modulenameptr{};
    bool        bDumpSuccess;
    __time64_t  time;
    struct tm   ltime;
    HWND        hWnd;
    HANDLE      hFile;

    // Write minidump
    if (GetModuleFileNameW(GetModuleHandle(NULL), modulename, _countof(modulename)) != 0)
    {
        modulenameptr = wcsrchr(modulename, '\\');
        *modulenameptr = L'\0';
        modulenameptr += 1;
    }
    else
    {
        modulenameptr = (wchar_t*)L"err.err";
    }

    _time64(&time);
    _localtime64_s(&ltime, &time);
    wcsftime(timestamp, _countof(timestamp), L"%Y%m%d%H%M%S", &ltime);
    swprintf_s(dump_filename, L"%s\\%s\\%s.%s.dmp", modulename, L"CrashDumps", modulenameptr, timestamp);

    hFile = CreateFileW(dump_filename, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION ex;
        memset(&ex, 0, sizeof(ex));
        ex.ThreadId = GetCurrentThreadId();
        ex.ExceptionPointers = ExceptionInfo;
        ex.ClientPointers = TRUE;

        if (!(MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpWithDataSegs, &ex, NULL, NULL)))
            bDumpSuccess = false;

        CloseHandle(hFile);
    }

    // Logs exception into buffer and writes to file
    swprintf_s(crash_log_filename, L"%s\\%s\\%s.%s.log", modulename, L"CrashDumps", modulenameptr, timestamp);
    hFile = CreateFileW(crash_log_filename, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile != INVALID_HANDLE_VALUE)
    {
        auto Log = [ExceptionInfo, hFile](char* buffer, size_t size, bool reg, bool stack, bool trace)
        {
            if (LogException(buffer, size, (LPEXCEPTION_POINTERS)ExceptionInfo, reg, stack, trace))
            {
                // Write log file
                DWORD NumberOfBytesWritten = 0;
                WriteFile(hFile, buffer, strlen(buffer), &NumberOfBytesWritten, NULL);
            }
        };

        // Try to make a very descriptive exception, for that we need to malloc a huge buffer...
        if (auto buffer = (char*)malloc(max_logsize_ever))
        {
            Log(buffer, max_logsize_ever, true, true, true);
            free(buffer);
        }
        else
        {
            // Use a static buffer, no need for any allocation
            static const auto size = max_logsize_basic + max_logsize_regs + max_logsize_stackdump;
            static char static_buf[size];
            static_assert(size <= max_static_buffer, "Static buffer is too big");

            Log(buffer = static_buf, sizeof(static_buf), true, true, false);
        }
        CloseHandle(hFile);
    }

    // Copy re4_tweaks log file to CrashDumps
    {
        swprintf_s(re4t_log_filename, L"%s\\%s\\OutRun2006Tweaks.log", modulename, L"CrashDumps");

        if (std::filesystem::exists(Module::LogPath))
            std::filesystem::copy_file(Module::LogPath, re4t_log_filename);
    }

    // ZIP up the dump/log/save
    {
      swprintf_s(zip_filename, L"%s\\%s\\%s.%s.zip", modulename, L"CrashDumps", modulenameptr, timestamp);

      bool zip_created = false;

      FILE* zip_file;
      if (_wfopen_s(&zip_file, zip_filename, L"wb") == 0 && zip_file)
      {
        mz_zip_archive zip_archive;
        mz_zip_zero_struct(&zip_archive);
        if (mz_zip_writer_init_cfile(&zip_archive, zip_file, 3))
        {
          // Even if one of these fails we want all to attempt being added, so don't think we can do any assignment trick here...
          zip_created = true;

          if (!mz_zip_writer_add_file(&zip_archive, "dump.dmp", dump_filename, nullptr, 0, 3))
            zip_created = false;
          if (!mz_zip_writer_add_file(&zip_archive, "crash.log", crash_log_filename, nullptr, 0, 3))
            zip_created = false;
          if (!mz_zip_writer_add_file(&zip_archive, "OutRun2006Tweaks.log", re4t_log_filename, nullptr, 0, 3))
            zip_created = false;

          mz_zip_writer_finalize_archive(&zip_archive);
          mz_zip_writer_end(&zip_archive);
        }
        fclose(zip_file);
      }

      if (zip_created)
      {
        DeleteFileW(dump_filename);
        DeleteFileW(crash_log_filename);
        DeleteFileW(re4t_log_filename);
      }
    }

    // Exit the application
    wchar_t	error[1024];
    swprintf_s(error, L"Fatal error (0x%08X) at 0x%08X.\n\nA crash log has been saved to \"%s\".", (int)ExceptionInfo->ExceptionRecord->ExceptionCode, (int)ExceptionInfo->ExceptionRecord->ExceptionAddress, zip_filename);
    MessageBoxW(NULL, error, L"OutRun2006Tweaks", MB_ICONERROR | MB_OK);

    ShowCursor(TRUE);
    hWnd = FindWindowW(0, L"");
    SetForegroundWindow(hWnd);

    return EXCEPTION_CONTINUE_SEARCH;
}

void InitExceptionHandler()
{
    std::filesystem::path dumpPath = Module::ExePath.parent_path() / L"CrashDumps";

    if (!std::filesystem::exists(dumpPath))
        std::filesystem::create_directories(dumpPath);

    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);

    // Now stub out SetUnhandledExceptionFilter so NO ONE ELSE can set it!
    Memory::VP::Patch(&SetUnhandledExceptionFilter, { 0xC2, 0x04, 0x00 });
}
