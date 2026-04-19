/**
 * @file CrashHandler.cpp
 * @brief Windows mini-dump crash handler for JS8Call Subspace Edition.
 *
 * On crash, writes a mini-dump to a writable folder (preferring
 * %LOCALAPPDATA%\JS8Call) and shows a dialog with the file path. The
 * mini-dump can be analyzed with the matching JS8Call-debug.exe (DWARF
 * symbols) artifact from CI.
 *
 * Hooks installed:
 *   - SetUnhandledExceptionFilter (SEH)         — access violations etc.
 *   - signal(SIGABRT)                            — abort(), assert(), Qt qFatal,
 *                                                  glibc/CRT malloc detector
 *   - signal(SIGSEGV)                            — fallback for cases SEH misses
 *   - set_terminate                              — uncaught C++ exceptions
 *   - _set_purecall_handler                      — pure virtual calls
 *
 * Dump path resolution:
 *   The handler recomputes the dump folder on EVERY crash rather than
 *   trusting a cached static, because a crash may have corrupted nearby
 *   memory. It tries (in order):
 *     1. %LOCALAPPDATA%\JS8Call
 *     2. %TEMP%
 *     3. The current working directory
 *     4. %USERPROFILE%\Desktop
 *   The first path that accepts a CreateFileW wins. If all four fail,
 *   the failure dialog reports the GetLastError of the last attempt.
 *
 * No-op on non-Windows platforms.
 */

#include "CrashHandler.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

#include <QDebug>

#pragma comment(lib, "dbghelp.lib")

// GCC C++ runtime entry point used to terminate when a pure virtual is
// called. Declared at file scope (extern "C" in a function body is not
// legal C++). Exported by libstdc++ on MinGW. Used only by crashTest.
extern "C" void __cxa_pure_virtual();

static volatile LONG s_dumpInProgress = 0;

// Resolve a candidate dump folder. Returns true and fills out_buf on
// success. Caller provides the candidate id (0..N-1).
static bool resolveCandidateFolder(int idx, wchar_t *out_buf, size_t cap) {
    out_buf[0] = 0;
    switch (idx) {
    case 0: {
        // %LOCALAPPDATA%\JS8Call
        wchar_t base[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base)
            != S_OK)
            return false;
        if (wcslen(base) + 9 >= cap) return false;
        wcscpy_s(out_buf, cap, base);
        wcscat_s(out_buf, cap, L"\\JS8Call");
        CreateDirectoryW(out_buf, nullptr);  // ignore error; CreateFile tells us
        return true;
    }
    case 1: {
        // %TEMP%
        DWORD n = GetTempPathW(static_cast<DWORD>(cap), out_buf);
        if (n == 0 || n >= cap) return false;
        // strip trailing backslash if any so concat below is consistent
        size_t len = wcslen(out_buf);
        if (len > 0 && out_buf[len-1] == L'\\') out_buf[len-1] = 0;
        return true;
    }
    case 2: {
        // current working directory
        DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(cap), out_buf);
        return n != 0 && n < cap;
    }
    case 3: {
        // %USERPROFILE%\Desktop
        wchar_t base[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, base)
            != S_OK)
            return false;
        if (wcslen(base) >= cap) return false;
        wcscpy_s(out_buf, cap, base);
        return true;
    }
    default:
        return false;
    }
}

// Try to open the dump file in the given folder. On success, returns the
// file handle and fills out_path with the full path. On failure, returns
// INVALID_HANDLE_VALUE and out_lastError gets the last GetLastError().
static HANDLE openDumpFile(wchar_t const *folder, wchar_t const *timestamp,
                           wchar_t *out_path, size_t cap, DWORD *out_lastError) {
    swprintf_s(out_path, cap, L"%s\\JS8Call-crash-%s.dmp", folder, timestamp);
    HANDLE h = CreateFileW(out_path, GENERIC_WRITE,
                           FILE_SHARE_READ,  // allow concurrent reads
                           nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *out_lastError = GetLastError();
    }
    return h;
}

// Format a UTC timestamp like 20260419T204500Z into out (>=20 wchar_t).
static void buildTimestamp(wchar_t *out) {
    time_t now = time(nullptr);
    struct tm *t = gmtime(&now);
    swprintf_s(out, 20,
               L"%04d%02d%02dT%02d%02d%02dZ",
               t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
               t->tm_hour, t->tm_min, t->tm_sec);
}

// Write a minidump. Tries multiple folders. Synthesizes EXCEPTION_POINTERS
// via RtlCaptureContext when one isn't provided (signal/terminate paths).
static void writeDump(EXCEPTION_POINTERS *exceptionInfo, wchar_t const *cause) {
    // Re-entrancy guard: if we crash while writing a dump, don't loop.
    if (InterlockedExchange(&s_dumpInProgress, 1) != 0) {
        return;
    }

    // Synthesize EXCEPTION_POINTERS if not provided (signal/terminate paths).
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS synth{};
    if (!exceptionInfo) {
        context.ContextFlags = CONTEXT_FULL;
        RtlCaptureContext(&context);
        record.ExceptionCode = 0xE0000001;  // user-defined "fatal"
        record.ExceptionAddress = (PVOID)context.Rip;
        synth.ExceptionRecord = &record;
        synth.ContextRecord = &context;
        exceptionInfo = &synth;
    }

    wchar_t timestamp[20];
    buildTimestamp(timestamp);

    HANDLE file = INVALID_HANDLE_VALUE;
    wchar_t folder[MAX_PATH];
    wchar_t path[MAX_PATH];
    DWORD lastError = 0;
    int folderIdx = -1;

    for (int i = 0; i < 4; ++i) {
        if (!resolveCandidateFolder(i, folder, MAX_PATH)) continue;
        file = openDumpFile(folder, timestamp, path, MAX_PATH, &lastError);
        if (file != INVALID_HANDLE_VALUE) {
            folderIdx = i;
            break;
        }
    }

    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exceptionInfo;
        mei.ClientPointers = FALSE;

        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                    file, MiniDumpNormal, &mei, nullptr, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);

        wchar_t const *folderTag =
            (folderIdx == 0) ? L"AppData\\Local\\JS8Call" :
            (folderIdx == 1) ? L"TEMP" :
            (folderIdx == 2) ? L"current directory" :
                               L"Desktop";
        wchar_t msg[MAX_PATH + 320];
        if (ok) {
            swprintf_s(msg, MAX_PATH + 320,
                     L"JS8Call has crashed (%s).\n\n"
                     L"A crash report was saved to:\n\n%s\n\n"
                     L"(folder: %s)\n\n"
                     L"Please send this file to WM8Q for analysis.",
                     cause, path, folderTag);
        } else {
            swprintf_s(msg, MAX_PATH + 320,
                     L"JS8Call has crashed (%s).\n\n"
                     L"A dump file was created at:\n\n%s\n\n"
                     L"but MiniDumpWriteDump failed (GetLastError=%lu). "
                     L"The file may be empty or truncated.",
                     cause, path, GetLastError());
        }
        MessageBoxW(nullptr, msg, L"JS8Call Crash Report",
                    MB_OK | MB_ICONERROR);
    } else {
        // All four candidate folders failed.
        wchar_t msg[MAX_PATH + 320];
        swprintf_s(msg, MAX_PATH + 320,
                 L"JS8Call has crashed (%s) but could not write a crash "
                 L"report to any of:\n\n"
                 L"  %%LOCALAPPDATA%%\\JS8Call\n"
                 L"  %%TEMP%%\n"
                 L"  current directory\n"
                 L"  Desktop\n\n"
                 L"Last GetLastError=%lu. Antivirus or permissions may be "
                 L"blocking writes to all four locations.",
                 cause, lastError);
        MessageBoxW(nullptr, msg, L"JS8Call Crash Report",
                    MB_OK | MB_ICONERROR);
    }
}

static LONG WINAPI sehHandler(EXCEPTION_POINTERS *exceptionInfo) {
    writeDump(exceptionInfo, L"unhandled exception");
    return EXCEPTION_EXECUTE_HANDLER;
}

static void abortHandler(int) {
    writeDump(nullptr, L"abort/assert");
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

static void segvHandler(int) {
    writeDump(nullptr, L"segmentation fault");
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
}

static void terminateHandler() {
    writeDump(nullptr, L"unhandled C++ exception");
    abort();
}

static void purecallHandler() {
    writeDump(nullptr, L"pure virtual call");
    abort();
}

void installCrashHandler() {
    // Quick startup diagnostic: log which dump folder we'd use today.
    // This is informational only — the actual write happens on crash and
    // re-resolves the folder defensively.
    wchar_t folder[MAX_PATH] = {};
    if (resolveCandidateFolder(0, folder, MAX_PATH)) {
        qWarning() << "[CrashHandler] preferred dump folder:"
                   << QString::fromWCharArray(folder);
    } else {
        qWarning() << "[CrashHandler] could not resolve %LOCALAPPDATA%, "
                      "will fall back to TEMP/CWD/Desktop on crash";
    }

    SetUnhandledExceptionFilter(sehHandler);
    signal(SIGABRT, abortHandler);
    signal(SIGSEGV, segvHandler);
    std::set_terminate(terminateHandler);
    _set_purecall_handler(purecallHandler);
}

// Test entry point: deliberately crash via the requested mechanism so we
// can verify each hook fires and writes a dump. Called from main() after
// installCrashHandler() if --crash-test was passed on the command line.
void crashTest(char const *kind) {
    if (!kind) return;
    if (strcmp(kind, "abort") == 0) {
        abort();
    } else if (strcmp(kind, "segv") == 0) {
        volatile int *p = nullptr;
        *p = 0;  // SIGSEGV / EXCEPTION_ACCESS_VIOLATION
    } else if (strcmp(kind, "throw") == 0) {
        throw std::runtime_error("crash-test: uncaught C++ exception");
    } else if (strcmp(kind, "purecall") == 0) {
        // GCC's runtime equivalent of MSVC's _purecall (which MinGW
        // doesn't expose as a directly-callable symbol). Declared at
        // file scope above. Triggers std::terminate -> our hook.
        __cxa_pure_virtual();
    } else {
        qWarning() << "[CrashHandler] unknown crash-test kind:" << kind;
    }
}

#else
// Non-Windows: no-op
void installCrashHandler() {}
void crashTest(char const *) {}
#endif
