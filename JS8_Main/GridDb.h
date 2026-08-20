#ifndef GRID_DB_HPP__
#define GRID_DB_HPP__

/**
 * @file GridDb.h
 * @brief [TODO #164] Persistent SQLite tier of the ONE callsign->grid
 * authority.
 *
 * This is NOT a second authority: SpotMapWindow::m_gridByCall remains
 * the single in-session authority every consumer reads. GridDb only
 * (1) SEEDS it at startup with everything previously banked, and
 * (2) records each refinement the authority ACCEPTS (write-through at
 * rememberGrid's single accept point — all precision/mover decisions
 * are made there, never here).
 *
 * Sources follow the sanctioned-content rules ([gridpolicy]): on-air
 * grid-bearing text ("radio") and the MQTT/PSKReporter harvest
 * ("mqtt", usually 6-char). A JS8_NO_MQTT or genuinely offline
 * session simply runs from whatever was banked — the no-internet
 * payoff.
 *
 * File: config dir alongside JS8Call.ini, per-instance suffix rules
 * ([multiinst]): JS8Call<suffix>-grids.db.
 */

#include <QHash>
#include <QSqlDatabase>
#include <QString>

class GridDb final {
  public:
    GridDb() = default;
    ~GridDb();
    GridDb(GridDb const &) = delete;
    GridDb &operator=(GridDb const &) = delete;

    bool open(QString const &path);
    bool isOpen() const { return m_db.isOpen(); }

    // Every banked call -> grid (uppercase keys), for seeding the
    // in-session authority.
    QHash<QString, QString> loadAll() const;

    // Record an authority-accepted refinement. No precision logic
    // here — the caller already decided.
    void upsert(QString const &call, QString const &grid,
                QString const &source);

  private:
    QSqlDatabase m_db;
    QString m_connName;
};

#endif
