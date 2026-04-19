/**
 * @file CrashHandler.cpp
 * @brief Windows mini-dump crash handler for JS8Call Subspace Edition.
 *
 * On crash, writes a mini-dump to the user's AppData directory and shows
 * a dialog with the file path. The mini-dump can be analyzed with the
 * matching JS8Call-debug.exe (DWARF symbols) artifact from CI.
 *
 * Hooks installed:
 *   - SetUnhandledExceptionFilter (SEH)         — access violations etc.
 *   - signal(SIGABRT)                            — abort(), assert(), Qt qFatal,
 *                                                  glibc/CRT malloc detector
 *   - signal(SIGSEGV)                            — fallback for cases SEH misses
 *   - set_terminate                              — uncaught C++ exceptions
 *   - _set_purecall_handler                      — pure virtual calls
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
#include <ctime>
#include <exception>

#include <QDebug>

#pragma comment(lib, "dbghelp.lib")

static wchar_t s_dumpPath[MAX_PATH] = {};
static volatile LONG s_dumpInProgress = 0;

// Write a minidump to the configured path. Safe to call from signal handlers
// and terminate handlers; if exceptionInfo is null we synthesize a context
// from the current registers via RtlCaptureContext so the dump still has
// useful stack frames.
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
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exceptionInfo;
        mei.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          file, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(file);

        wchar_t msg[MAX_PATH + 256];
        swprintf(msg, MAX_PATH + 256,
                 L"JS8Call has crashed (%s).\n\n"
                 L"A crash report was saved to:\n\n%s\n\n"
                 L"Please send this file to WM8Q for analysis.",
                 cause, path);
        MessageBoxW(nullptr, msg, L"JS8Call Crash Report",
                    MB_OK | MB_ICONERROR);
    } else {
        // Couldn't write the dump; at least tell the user something happened.
        wchar_t msg[MAX_PATH + 256];
        swprintf(msg, MAX_PATH + 256,
                 L"JS8Call has crashed (%s) and could not write a crash report "
                 L"to:\n\n%s\n\n"
                 L"The folder may be missing or write-protected.",
                 cause, path);
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
    // Re-raise default behavior so the process actually exits.
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
    // Resolve dump folder. Prefer %LOCALAPPDATA%\JS8Call; fall back to CWD
    // (which on Inno-Setup installs is Program Files and write-protected,
    // but at least it's deterministic).
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                         s_dumpPath) == S_OK) {
        wcscat(s_dumpPath, L"\\JS8Call");
        CreateDirectoryW(s_dumpPath, nullptr);
    } else {
        wcscpy(s_dumpPath, L".");
    }

    // Log the resolved path so the user can verify it's writable and
    // know where to look. (Goes through Qt's debug channel to the
    // diagnostic log if enabled, and to stderr otherwise.)
    qWarning() << "[CrashHandler] dump path:"
               << QString::fromWCharArray(s_dumpPath);

    SetUnhandledExceptionFilter(sehHandler);
    signal(SIGABRT, abortHandler);
    signal(SIGSEGV, segvHandler);
    std::set_terminate(terminateHandler);
    _set_purecall_handler(purecallHandler);
}

#else
// Non-Windows: no-op
void installCrashHandler() {}
#endif
