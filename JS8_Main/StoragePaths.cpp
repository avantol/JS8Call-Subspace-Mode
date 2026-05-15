#include "StoragePaths.h"

#include <QCoreApplication>

namespace {

// RAII helper that temporarily swaps QCoreApplication::applicationName
// and restores it on scope exit. QStandardPaths consults
// applicationName when building its app-suffixed paths, so this gives
// us a deterministic way to resolve "JS8Call" paths regardless of
// what name the binary advertises for display.
class AppNamePin {
    QString saved_;

  public:
    explicit AppNamePin(QString const &name)
        : saved_(QCoreApplication::applicationName()) {
        QCoreApplication::setApplicationName(name);
    }
    ~AppNamePin() { QCoreApplication::setApplicationName(saved_); }
    AppNamePin(AppNamePin const &) = delete;
    AppNamePin &operator=(AppNamePin const &) = delete;
};

} // namespace

QString StoragePaths::dataLocation() {
    AppNamePin pin{QStringLiteral("JS8Call")};
    return QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
}

QString StoragePaths::configLocation() {
    AppNamePin pin{QStringLiteral("JS8Call")};
    return QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
}

QString StoragePaths::locateConfig(QString const &fileName) {
    AppNamePin pin{QStringLiteral("JS8Call")};
    return QStandardPaths::locate(QStandardPaths::AppConfigLocation,
                                  fileName);
}

QString StoragePaths::settingsDirectory() {
    AppNamePin pin{QStringLiteral("JS8Call")};
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
}

QString StoragePaths::pathApplicationName() {
    QString name = QCoreApplication::applicationName();
    name.replace(QStringLiteral("Subspace Edition"),
                 QStringLiteral("JS8Call"));
    return name;
}
