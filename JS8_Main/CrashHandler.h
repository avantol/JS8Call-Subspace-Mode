#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

/// Install a crash handler that writes a mini-dump on unhandled exceptions.
/// Windows only — no-op on other platforms.
/// Call once at application startup (before QApplication if possible).
void installCrashHandler();

#endif
