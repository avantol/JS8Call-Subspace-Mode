#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

/// Install a crash handler that writes a mini-dump on unhandled exceptions.
/// Windows only — no-op on other platforms.
/// Call once at application startup (before QApplication if possible).
void installCrashHandler();

/// Deliberately crash via the requested mechanism so the user can verify
/// each handler hook fires and writes a dump. kind is one of:
///   "abort"    — calls abort()           → SIGABRT path
///   "segv"     — null pointer write      → SEH path (sanity check)
///   "throw"    — uncaught std::exception → set_terminate path
///   "purecall" — calls _purecall()       → _set_purecall_handler path
/// Windows only — no-op on other platforms.
void crashTest(char const *kind);

#endif
