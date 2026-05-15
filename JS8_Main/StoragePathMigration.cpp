#include "StoragePathMigration.h"

#include "StoragePaths.h"

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

// Resolve the legacy "Subspace Edition" AppLocalDataLocation by
// temporarily swapping applicationName so we can locate any data
// that earlier branded builds left behind.
QString legacySubspaceDataDir() {
    QString const saved = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName(QStringLiteral("Subspace Edition"));
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

// Locate the first record byte in an ADIF file (immediately past the
// "<eoh>" marker and any trailing line breaks). Returns 0 when no
// header marker is present, so plain record-only files append cleanly.
qint64 adifBodyStart(QFile &f) {
    qint64 const saved = f.pos();
    f.seek(0);
    QByteArray const probe = f.read(8192);
    f.seek(saved);
    int const idx = probe.toLower().indexOf("<eoh>");
    if (idx < 0) return 0;
    qint64 pos = idx + 5;
    while (pos < probe.size() &&
           (probe[pos] == '\r' || probe[pos] == '\n'))
        ++pos;
    return pos;
}

// If dst doesn't exist, move src to dst. If dst exists, append src
// to dst then unlink src. When skipAdifHeader is true the source's
// ADIF header (text up to and including "<eoh>") is skipped so the
// destination keeps a single valid header.
bool appendOrMove(QString const &srcPath, QString const &dstPath,
                  bool skipAdifHeader = false) {
    if (!QFileInfo::exists(srcPath)) return true;
    if (!QFileInfo::exists(dstPath)) {
        QDir().mkpath(QFileInfo(dstPath).absolutePath());
        return QFile::rename(srcPath, dstPath);
    }
    bool copied = true;
    {
        QFile in(srcPath);
        if (!in.open(QIODevice::ReadOnly)) return false;
        QFile out(dstPath);
        if (!out.open(QIODevice::Append)) return false;
        if (skipAdifHeader) in.seek(adifBodyStart(in));
        constexpr qint64 BUF = 64 * 1024;
        while (!in.atEnd()) {
            QByteArray const chunk = in.read(BUF);
            if (chunk.isEmpty()) break;
            if (out.write(chunk) != chunk.size()) {
                copied = false;
                break;
            }
        }
        out.flush();
    }
    if (copied) {
        QFile::remove(srcPath);
    }
    return copied;
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
    QString const srcRoot = legacySubspaceDataDir();
    QDir const src(srcRoot);
    if (!src.exists()) return;

    QString const markerPath = src.absoluteFilePath("merged");
    if (QFileInfo::exists(markerPath)) return;

    QString const dstRoot = StoragePaths::dataLocation();
    QDir().mkpath(dstRoot);
    QDir const dst(dstRoot);

    // Move-if-absent: binary state files. Keep whichever the user
    // already has at the canonical location.
    QStringList const moveOnly{
        QStringLiteral("inbox.db3"),
        QStringLiteral("js8call_wisdom.dat")};
    for (QString const &name : moveOnly) {
        moveIfAbsent(src.absoluteFilePath(name),
                     dst.absoluteFilePath(name));
    }

    // Move-or-append: plain text logs concatenate cleanly.
    QStringList const appendablePlain{
        QStringLiteral("ALL.TXT"),
        QStringLiteral("DIRECTED.TXT"),
        QStringLiteral("js8call.log")};
    for (QString const &name : appendablePlain) {
        appendOrMove(src.absoluteFilePath(name),
                     dst.absoluteFilePath(name));
    }
    // ADIF needs its source-side header skipped or the destination
    // ends up with a header embedded mid-record.
    appendOrMove(src.absoluteFilePath("js8call_log.adi"),
                 dst.absoluteFilePath("js8call_log.adi"),
                 /*skipAdifHeader=*/true);

    // save/ subdirectory: per-file no-clobber merge.
    moveDirContents(QDir(src.absoluteFilePath("save")),
                    QDir(dst.absoluteFilePath("save")));
    QDir(src.absoluteFilePath("save")).rmdir(".");

    // If nothing remains at the source, remove it; otherwise drop a
    // sentinel so subsequent launches skip the work. Runtime no
    // longer writes here, so a one-shot is sufficient.
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
