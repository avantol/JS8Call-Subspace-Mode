#include "StoragePathMigration.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace {

// Resolve AppLocalDataLocation for an arbitrary applicationName by
// temporarily swapping it on QCoreApplication, then restoring. Same
// pattern used in MultiSettings::settings_path() for the legacy
// JS8Call.ini path.
QString resolveAppLocalDataDir(QString const &appName) {
    QString const saved = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName(appName);
    QString const path = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    QCoreApplication::setApplicationName(saved);
    return path;
}

// Move src to dst if dst doesn't already exist. Returns true on
// success or when there was nothing to do.
bool moveIfAbsent(QString const &srcPath, QString const &dstPath) {
    if (!QFileInfo::exists(srcPath)) return true;
    if (QFileInfo::exists(dstPath)) return false;
    QDir().mkpath(QFileInfo(dstPath).absolutePath());
    return QFile::rename(srcPath, dstPath);
}

// If dst doesn't exist, move src to dst. If dst exists, append src
// contents to dst and remove src.
bool appendOrMove(QString const &srcPath, QString const &dstPath) {
    if (!QFileInfo::exists(srcPath)) return true;
    if (!QFileInfo::exists(dstPath)) {
        QDir().mkpath(QFileInfo(dstPath).absolutePath());
        return QFile::rename(srcPath, dstPath);
    }
    QFile in(srcPath);
    if (!in.open(QIODevice::ReadOnly)) return false;
    QFile out(dstPath);
    if (!out.open(QIODevice::Append)) return false;
    constexpr qint64 BUF = 64 * 1024;
    while (!in.atEnd()) {
        QByteArray const chunk = in.read(BUF);
        if (chunk.isEmpty()) break;
        if (out.write(chunk) != chunk.size()) return false;
    }
    in.close();
    out.close();
    return QFile::remove(srcPath);
}

// Recursively move contents of srcDir into dstDir using no-clobber
// semantics. Empty subdirectories are pruned from the source.
void moveDirContents(QDir const &srcDir, QDir const &dstDir) {
    if (!srcDir.exists()) return;
    QDir().mkpath(dstDir.absolutePath());
    auto const entries = srcDir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (QFileInfo const &entry : entries) {
        QString const dstPath = dstDir.absoluteFilePath(entry.fileName());
        if (entry.isDir()) {
            moveDirContents(QDir(entry.absoluteFilePath()), QDir(dstPath));
            QDir(entry.absoluteFilePath()).rmdir(".");
        } else {
            moveIfAbsent(entry.absoluteFilePath(), dstPath);
        }
    }
}

bool isDirEmpty(QDir const &dir) {
    return dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot |
                         QDir::Hidden | QDir::System).isEmpty();
}

} // namespace

void StoragePathMigration::run() {
    QString const srcRoot = resolveAppLocalDataDir("Subspace Edition");
    QDir const src(srcRoot);
    if (!src.exists()) return;

    QString const markerPath = src.absoluteFilePath("merged");
    if (QFileInfo::exists(markerPath)) return;

    QString const dstRoot = resolveAppLocalDataDir("JS8Call");
    QDir().mkpath(dstRoot);
    QDir const dst(dstRoot);

    // Move-if-absent: binary state files; keep whichever the user
    // already has at the canonical location.
    QStringList const moveOnly{
        QStringLiteral("inbox.db3"),
        QStringLiteral("js8call_wisdom.dat")};
    for (QString const &name : moveOnly) {
        moveIfAbsent(src.absoluteFilePath(name),
                     dst.absoluteFilePath(name));
    }

    // Move-or-append: text logs concatenate cleanly.
    QStringList const appendable{
        QStringLiteral("ALL.TXT"),
        QStringLiteral("DIRECTED.TXT"),
        QStringLiteral("js8call_log.adi"),
        QStringLiteral("js8call.log")};
    for (QString const &name : appendable) {
        appendOrMove(src.absoluteFilePath(name),
                     dst.absoluteFilePath(name));
    }

    // save/ subdirectory: per-file no-clobber merge.
    moveDirContents(QDir(src.absoluteFilePath("save")),
                    QDir(dst.absoluteFilePath("save")));
    QDir(src.absoluteFilePath("save")).rmdir(".");

    // If nothing remains at the source, remove it. Otherwise drop a
    // sentinel so we don't repeat the work on the next launch.
    QDir reread(srcRoot);
    if (isDirEmpty(reread)) {
        reread.rmdir(".");
    } else {
        QFile m(markerPath);
        if (m.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m.write("merged\n");
            m.close();
        }
    }
}
