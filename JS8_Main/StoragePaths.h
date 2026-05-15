#ifndef STORAGE_PATHS_H
#define STORAGE_PATHS_H

#include <QStandardPaths>
#include <QString>

namespace StoragePaths {

// Resolve standard-location paths using the historical "JS8Call"
// applicationName regardless of the binary's display branding. This
// pins user data and configuration to their long-standing locations
// (~/.local/share/JS8Call, ~/.config/JS8Call, %APPDATA%/JS8Call) so
// rebranding the binary doesn't strand user data in a new directory.

// AppLocalDataLocation under "JS8Call" -- where data files live
// (ALL.TXT, DIRECTED.TXT, inbox.db3, js8call_log.adi, wisdom, save/).
QString dataLocation();

// AppConfigLocation under "JS8Call" -- the per-app config directory
// (currently used for hamlib_settings.json).
QString configLocation();

// Equivalent to QStandardPaths::locate(AppConfigLocation, fileName)
// with applicationName pinned to "JS8Call".
QString locateConfig(QString const &fileName);

} // namespace StoragePaths

#endif // STORAGE_PATHS_H
