"""intel.py — the station-intelligence store js8reach plans against.

[TODO #155/#154 family] Script-owned SQLite, built by mine.py from logs
the app writes but never reads back (DIRECTED.TXT, ALL.TXT) plus the
app's own grid bank (read-only). Deliberately NOT part of the app: no
build dependency, works against any JS8Call including legacy.

What each table answers for the planner:
  stations  — is this call ever on the air, how strong, how lately,
              does it relay, does it answer us?
  activity  — WHEN is it on the air (UTC hour histogram)?
  edges     — who hears whom (the offline mesh map)
  probes    — our own query history + measured answer latency (the
              only honest basis for p_ans)
  sightings — raw per-decode ground truth; the replay simulator scores
              plans against it ("was T actually on air after T0?")

Timestamps are UTC epoch seconds throughout.
"""

from __future__ import annotations

import sqlite3
from pathlib import Path

DEFAULT_DB = Path.home() / ".config" / "js8reach-intel.db"

SCHEMA = """
CREATE TABLE IF NOT EXISTS meta (
    key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE IF NOT EXISTS stations (
    call         TEXT PRIMARY KEY,
    first_heard  INTEGER,   -- first decode of them, epoch s
    last_heard   INTEGER,   -- last decode of them  (REAL activity, unlike
                            -- the app grid bank's last_seen — see plan)
    heard_count  INTEGER DEFAULT 0,
    snr_n        INTEGER DEFAULT 0,   -- our copy of THEM
    snr_sum      INTEGER DEFAULT 0,
    snr_min      INTEGER,
    snr_max      INTEGER,
    rev_snr_n    INTEGER DEFAULT 0,   -- THEIR copy of US (they told us)
    rev_snr_last INTEGER,
    rev_snr_best INTEGER,
    rev_last     INTEGER,
    resp_count   INTEGER DEFAULT 0,   -- frames that are REPLIES (ACK/SNR/
                                      -- YES/GRID/STATUS/INFO/HEARTBEAT SNR)
    spont_count  INTEGER DEFAULT 0,   -- unprompted (HEARTBEAT/HB/CQ/text)
    relay_seen   INTEGER DEFAULT 0,   -- observed forwards (*DE* / CALL>)
    -- Relay requests we watched go out to this station, and how many it
    -- actually acted on. MEASURED, 2026-08-23: 143 requests on the air,
    -- 46 forwarded = 32%, against the 0.55 the model had assumed from
    -- configuration defaults. Per station it runs 0% to 100%, which is
    -- far too wide a spread to replace with any single prior.
    relay_asked  INTEGER DEFAULT 0,
    relay_done   INTEGER DEFAULT 0,
    to_us        INTEGER DEFAULT 0,   -- directed frames addressed to us
    grid         TEXT
);

CREATE TABLE IF NOT EXISTS activity (
    call TEXT, hour INTEGER, n INTEGER DEFAULT 0,
    PRIMARY KEY (call, hour));

CREATE TABLE IF NOT EXISTS edges (
    hearer TEXT, heard TEXT,
    last_when INTEGER, n INTEGER DEFAULT 0,
    snr INTEGER, source TEXT,
    PRIMARY KEY (hearer, heard));

-- Third-party mentions: "X told us it hears Y at time t". This is the
-- ONLY evidence that a station was on the air when WE could not hear
-- it -- i.e. exactly the case a relay exists to solve. Without it a
-- replay simulator structurally cannot measure relay value.
CREATE TABLE IF NOT EXISTS edge_events (
    ts INTEGER, hearer TEXT, heard TEXT, source TEXT);

-- `present` = the target was demonstrably on the air near this probe
-- (we decoded it within +/-10 min). p_ans MUST be conditioned on it:
-- measured unconditionally the answer rate is 17%, but that is mostly
-- absence -- given presence it is 82%. Multiplying an unconditional
-- rate by a separate presence term double-penalizes absence.
CREATE TABLE IF NOT EXISTS probes (
    ts INTEGER, target TEXT, cmd TEXT,
    answered INTEGER DEFAULT 0, latency_s INTEGER,
    present INTEGER DEFAULT 0);

CREATE TABLE IF NOT EXISTS sightings (
    ts INTEGER, call TEXT, snr INTEGER, dial REAL, offset INTEGER);

CREATE INDEX IF NOT EXISTS idx_sight_call_ts ON sightings(call, ts);
CREATE INDEX IF NOT EXISTS idx_sight_ts      ON sightings(ts);
CREATE INDEX IF NOT EXISTS idx_edges_heard   ON edges(heard);
CREATE INDEX IF NOT EXISTS idx_probes_target ON probes(target, ts);
CREATE INDEX IF NOT EXISTS idx_ee_heard ON edge_events(heard, ts);
CREATE INDEX IF NOT EXISTS idx_ee_pair  ON edge_events(hearer, heard, ts);
"""


# Bump whenever SCHEMA changes. Every table here is DERIVED from the
# logs and rebuilt by mine.py in ~5 s, so a version mismatch simply
# drops and recreates rather than carrying migration code that would
# have to be right forever.
SCHEMA_VERSION = "3"

_TABLES = ("meta", "stations", "activity", "edges", "edge_events",
           "probes", "sightings")


def connect(path: Path | str = DEFAULT_DB) -> sqlite3.Connection:
    db = sqlite3.connect(str(path))
    db.row_factory = sqlite3.Row
    db.executescript(SCHEMA)
    have = db.execute("SELECT value FROM meta WHERE key='schema_version'"
                      ).fetchone()
    if (have["value"] if have else None) != SCHEMA_VERSION:
        for t in _TABLES:
            db.execute(f"DROP TABLE IF EXISTS {t}")
        db.executescript(SCHEMA)
        db.execute("INSERT INTO meta (key, value) VALUES "
                   "('schema_version', ?)", (SCHEMA_VERSION,))
        db.commit()
    return db


def set_meta(db: sqlite3.Connection, key: str, value: str) -> None:
    db.execute("INSERT INTO meta (key, value) VALUES (?, ?) "
               "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
               (key, value))


def get_meta(db: sqlite3.Connection, key: str,
             default: str | None = None) -> str | None:
    row = db.execute("SELECT value FROM meta WHERE key = ?",
                     (key,)).fetchone()
    return row["value"] if row else default


# ---- read helpers used by the model ---------------------------------

def station(db: sqlite3.Connection, call: str) -> sqlite3.Row | None:
    return db.execute("SELECT * FROM stations WHERE call = ?",
                      (call.upper(),)).fetchone()


def hour_histogram(db: sqlite3.Connection, call: str) -> list[int]:
    hist = [0] * 24
    for r in db.execute("SELECT hour, n FROM activity WHERE call = ?",
                        (call.upper(),)):
        hist[r["hour"]] = r["n"]
    return hist


def hearers_of(db: sqlite3.Connection, call: str) -> list[sqlite3.Row]:
    """Stations observed hearing `call` — the relay candidate pool."""
    return list(db.execute(
        "SELECT * FROM edges WHERE heard = ? ORDER BY last_when DESC",
        (call.upper(),)))


def heard_by(db: sqlite3.Connection, call: str) -> list[sqlite3.Row]:
    return list(db.execute(
        "SELECT * FROM edges WHERE hearer = ? ORDER BY last_when DESC",
        (call.upper(),)))


def probe_stats(db: sqlite3.Connection, call: str) -> tuple[int, int, float]:
    """(probes, answers, median latency s) for our queries to `call`,
    counting ONLY probes where the target was demonstrably present —
    the conditional rate is the one that composes correctly with a
    separate presence estimate."""
    rows = list(db.execute(
        "SELECT answered, latency_s FROM probes WHERE target = ? "
        "AND present = 1", (call.upper(),)))
    n = len(rows)
    ans = [r["latency_s"] for r in rows
           if r["answered"] and r["latency_s"] is not None]
    if not ans:
        return n, 0, 0.0
    ans.sort()
    return n, len(ans), float(ans[len(ans) // 2])


def all_calls_with_grid(db: sqlite3.Connection) -> list[sqlite3.Row]:
    return list(db.execute(
        "SELECT call, grid, last_heard, heard_count, snr_n, snr_sum, "
        "snr_max FROM stations WHERE grid IS NOT NULL AND grid != ''"))


def mentioned_between(db: sqlite3.Connection, call: str,
                      t0: int, t1: int) -> list[str]:
    """Stations that told us they heard `call` in [t0, t1) — third-party
    proof the target was on the air even when we could not copy it."""
    return [r["hearer"] for r in db.execute(
        "SELECT DISTINCT hearer FROM edge_events "
        "WHERE heard = ? AND ts >= ? AND ts < ?",
        (call.upper(), t0, t1))]


def pair_heard_between(db: sqlite3.Connection, hearer: str, heard: str,
                       t0: int, t1: int) -> bool:
    r = db.execute(
        "SELECT COUNT(*) n FROM edge_events WHERE hearer=? AND heard=? "
        "AND ts >= ? AND ts < ?",
        (hearer.upper(), heard.upper(), t0, t1)).fetchone()
    return int(r["n"]) > 0


def sightings_between(db: sqlite3.Connection, call: str,
                      t0: int, t1: int) -> int:
    """Ground truth for the simulator: decodes of `call` in [t0, t1)."""
    r = db.execute(
        "SELECT COUNT(*) AS n FROM sightings "
        "WHERE call = ? AND ts >= ? AND ts < ?",
        (call.upper(), t0, t1)).fetchone()
    return int(r["n"])
