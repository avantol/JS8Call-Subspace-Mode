/**
 * @file CrashHandler.cpp
 * @brief Windows mini-dump crash handler for JS8Call Subspace Edition.
 *
 * On unhandled exceptions, writes a mini-dump to the user's AppData
 * directory and shows a dialog with the file path. The mini-dump can
 * be analyzed with the matching debug symbols (.debug or unstripped
 * binary) from the CI build.
 *
 * No-op on non-Windows platforms.
 */

#include "CrashHandler.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "dbghelp.lib")

static wchar_t s_dumpPath[MAX_PATH] = {};

static LONG WINAPI crashDumpHandler(EXCEPTION_POINTERS *exceptionInfo) {
    // Build filename with timestamp
    wchar_t path[MAX_PATH];
    time_t now = time(nullptr);
    struct tm *t = gmtime(&now);
    swprintf(path, MAX_PATH,
             L"%s\\JS8Call-crash-%04d%02d%02dT%02d%02d%02dZ.dmp",
             s_dumpPath,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = exceptionInfo;
    mei.ClientPointers = FALSE;

    // MiniDumpNormal = small dump (~5-20MB) with stack traces
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                      file, MiniDumpNormal, &mei, nullptr, nullptr);
    CloseHandle(file);

    // Show a message box with the dump file path
    wchar_t msg[MAX_PATH + 200];
    swprintf(msg, MAX_PATH + 200,
             L"JS8Call has crashed. A crash report was saved to:\n\n%s\n\n"
             L"Please send this file to WM8Q for analysis.",
             path);
    MessageBoxW(nullptr, msg, L"JS8Call Crash Report",
                MB_OK | MB_ICONERROR);

    return EXCEPTION_EXECUTE_HANDLER;
}

void installCrashHandler() {
    // Default to AppData\Local\JS8Call
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                         s_dumpPath) == S_OK) {
        wcscat(s_dumpPath, L"\\JS8Call");
        CreateDirectoryW(s_dumpPath, nullptr);
    } else {
        wcscpy(s_dumpPath, L".");
    }

    SetUnhandledExceptionFilter(crashDumpHandler);
}

#else
// Non-Windows: no-op
void installCrashHandler() {}
#endif
