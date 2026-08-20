/**
 * @file GridDb.cpp
 * @brief See header.
 */

#include "GridDb.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

Q_LOGGING_CATEGORY(griddb_js8, "js8.griddb")

GridDb::~GridDb() {
    if (m_db.isOpen())
        m_db.close();
    m_db = QSqlDatabase{}; // release before removeDatabase
    if (!m_connName.isEmpty())
        QSqlDatabase::removeDatabase(m_connName);
}

bool GridDb::open(QString const &path) {
    // Unique connection name — a second instance ([multiinst]) in the
    // same process (tests) must not collide.
    static int serial = 0;
    m_connName = QStringLiteral("griddb-%1").arg(++serial);
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                     m_connName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        qCWarning(griddb_js8)
            << "[GRIDDB] open FAILED:" << path
            << m_db.lastError().text();
        return false;
    }
    QSqlQuery q{m_db};
    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS grids ("
            " call TEXT PRIMARY KEY,"
            " grid TEXT NOT NULL,"
            " source TEXT,"
            " first_seen INTEGER,"
            " last_seen INTEGER,"
            " count INTEGER DEFAULT 1)"))) {
        qCWarning(griddb_js8)
            << "[GRIDDB] schema FAILED:" << q.lastError().text();
        m_db.close();
        return false;
    }
    qCWarning(griddb_js8) << "[GRIDDB] open:" << path;
    return true;
}

QHash<QString, QString> GridDb::loadAll() const {
    QHash<QString, QString> out;
    if (!m_db.isOpen())
        return out;
    QSqlQuery q{m_db};
    if (!q.exec(QStringLiteral("SELECT call, grid FROM grids"))) {
        qCWarning(griddb_js8)
            << "[GRIDDB] load FAILED:" << q.lastError().text();
        return out;
    }
    while (q.next())
        out.insert(q.value(0).toString().toUpper(),
                   q.value(1).toString().toUpper()); // [gridcase]
    qCWarning(griddb_js8) << "[GRIDDB] seeded" << out.size()
                          << "grids from disk";
    return out;
}

void GridDb::upsert(QString const &call, QString const &grid,
                    QString const &source) {
    if (!m_db.isOpen() || call.isEmpty() || grid.isEmpty())
        return;
    qint64 const now =
        QDateTime::currentDateTimeUtc().toSecsSinceEpoch();
    QSqlQuery q{m_db};
    q.prepare(QStringLiteral(
        "INSERT INTO grids (call, grid, source, first_seen, last_seen,"
        " count) VALUES (?, ?, ?, ?, ?, 1)"
        " ON CONFLICT(call) DO UPDATE SET"
        " grid = excluded.grid,"
        " source = excluded.source,"
        " last_seen = excluded.last_seen,"
        " count = count + 1"));
    q.addBindValue(call.toUpper());
    q.addBindValue(grid);
    q.addBindValue(source);
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec())
        qCWarning(griddb_js8)
            << "[GRIDDB] upsert FAILED:" << call
            << q.lastError().text();
    else
        qCDebug(griddb_js8) << "[GRIDDB] upsert" << call << grid
                            << source;
}
