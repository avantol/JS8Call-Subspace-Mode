"""livemodel.py — the same questions, asked of the LIVE band.

`decide.py` chooses the next message by asking six questions: is that
station on the air, can it hear us, will it answer, will it forward,
does this one hear that one, and where is it. Until now the only thing
that could answer them was `sim.HorizonModel`, backed by five months of
mined log. This answers the identical six from what is true right now,
so the decision rule runs unchanged against a live radio.

WHERE EACH ANSWER COMES FROM, and why (measured 2026-08-24, 40m):

    THE GRAPH -> the DATABASE, ~/.config/JS8Call-grids.db.
        374 hearers and 18,870 edges over 24 hours, against the map
        dump's 201 and 6,039 -- the dump serves the in-memory store,
        which keeps ONE HOUR. Three times the graph, and age costs
        nothing because the decay curve already discounts an old edge
        to about 12% against 28% for a fresh one. It is a FILE: no
        socket, no `TCPMaxConnections=1` eviction, and readable even
        when JS8Call is not running.

    RIGHT NOW -> the MAP DUMP, over TCP 2442.
        Which band, my call and grid, who is audible this minute, and
        ATTEMPTS -- what the map believes we are already trying, which
        exists nowhere else. Up to 45 seconds of flush lag makes the
        database useless for this and irrelevant for the graph.

    WILL IT ANSWER / WILL IT FORWARD -> the MINED CORPUS.
        These are station habits measured over months, not facts about
        this minute: 2,214 of our own probes for answering, and 116
        relay requests watched go out for forwarding (43 acted on,
        37%). Nothing in the live data measures them at all.

THIS FILE TRANSMITS NOTHING. It is the dry run: choose, explain, stop.
The class of error that cost the most on 2026-08-23/24 was the model
believing something the band does not support -- a relay through a
station we have never heard, a link the corpus never reported, a grid
we had not learned yet. All of those are visible here, on the ground,
for nothing.
"""
from __future__ import annotations

import math
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import intel                                  # noqa: E402
from callsign import base, is_routable        # noqa: E402
from sim import UNSEEN_LINK, reciprocity_for  # noqa: E402

GRIDS_DB = Path.home() / ".config" / "JS8Call-grids.db"
GRAPH_SECS = 24 * 3600          # how far back the routing graph reaches
SESSION_S = 2700.0              # a station stays on the air ~45 min


class _B:
    """A probability with its reasons, matching what decide.py expects."""

    __slots__ = ("p", "why")

    def __init__(self, p, why=None):
        self.p = p
        self.why = why or []


def norm_band(s: str, fallback: str = "") -> str:
    """"40" and "40m" are the same band; the database only knows the
    second. Andy typed `--band 40` on 2026-08-24 and every query matched
    nothing, so the tool reported "nobody has been reported hearing this
    station in 24 h" -- a confident, specific and entirely false claim,
    produced because a string did not match. Normalise here, once."""
    s = (s or "").strip().lower()
    if not s:
        return fallback
    if s.endswith("m"):
        return s
    if s.replace(".", "").isdigit():
        return s + "m"
    return s


class LiveModel:
    """Answers decide.py's six questions from the live band."""

    def __init__(self, lm, mycall="", band="", intel_db=None):
        self.lm = lm
        self.mycall = base(mycall or lm.my_call).upper()
        self.band = norm_band(band, lm.band)
        self.now = time.time()
        self.horizon = int(self.now)
        self.learned_grids: dict = {}
        self._c: dict = {}
        self.db = sqlite3.connect(f"file:{GRIDS_DB}?mode=ro", uri=True)
        self.db.row_factory = sqlite3.Row
        # Habits -- answering and forwarding -- are months-long traits
        # and are not observable in one session, so they still come out
        # of the mined corpus. Absent it, priors.
        try:
            self.intel = intel_db or intel.connect()
        except Exception:
            self.intel = None

    def rows_on_band(self) -> int:
        """How much the database holds for the band we are about to
        reason about. Zero means the band is wrong, NOT that the band
        is empty -- and those must never be reported as the same thing."""
        return self.db.execute(
            "SELECT COUNT(*) n FROM edges WHERE band=? AND when_s > ?",
            (self.band, int(self.now - GRAPH_SECS))).fetchone()["n"]

    def bands_known(self) -> list:
        return [r["band"] for r in self.db.execute(
            "SELECT band, COUNT(*) n FROM edges WHERE when_s > ? "
            "GROUP BY band ORDER BY n DESC",
            (int(self.now - GRAPH_SECS),))]

    # ---- the graph, out of the database ------------------------------

    def _edge(self, hearer: str, heard: str):
        key = ("e", hearer, heard)
        got = self._c.get(key)
        if got is None:
            got = self.db.execute(
                "SELECT when_s, snr, source FROM edges WHERE band=? AND "
                "hearer=? AND heard=? AND when_s > ? "
                "ORDER BY when_s DESC LIMIT 1",
                (self.band, hearer.upper(), heard.upper(),
                 int(self.now - GRAPH_SECS))).fetchone()
            self._c[key] = got if got else False
        return got or None

    def can_deliver_to(self, node: str) -> list:
        """Stations `node` can hear, so each can hand it a message."""
        key = ("adj", node)
        got = self._c.get(key)
        if got is None:
            got = [r["heard"] for r in self.db.execute(
                "SELECT heard, MAX(when_s) w FROM edges WHERE band=? AND "
                "hearer=? AND when_s > ? GROUP BY heard "
                "ORDER BY w DESC LIMIT 60",
                (self.band, node.upper(),
                 int(self.now - GRAPH_SECS)))]
            self._c[key] = got
        return got

    def p_link(self, hearer: str, heard: str) -> _B:
        """Does `hearer` hear `heard`? Same curve the replay uses, so
        the two cannot drift: fast decay, a diurnal bump at 24 and 48
        hours, and scaled by the signal margin where we have one."""
        row = self._edge(hearer, heard)
        if not row:
            return _B(UNSEEN_LINK,
                      [f"{hearer}->{heard}: nothing reported in 24 h"])
        age_h = max(0.0, (self.now - row["when_s"]) / 3600.0)
        di = max(0.0, math.cos(2.0 * math.pi * age_h / 24.0))
        live = (0.120 + 0.100 * math.exp(-age_h / 5.0)
                + 0.080 * math.exp(-age_h / 192.0) * di)
        margin = 1.0
        if row["snr"] is not None:
            margin = min(1.0, max(0.25, (row["snr"] + 24.0) / 18.0))
        p = min(0.95, live * margin)
        return _B(p, [f"{hearer}->{heard}: {row['source']}, "
                      f"{age_h:.1f} h old, snr {row['snr']} -> {p:.2f}"])

    def out_degree(self, call: str) -> int:
        key = ("deg", call)
        got = self._c.get(key)
        if got is None:
            got = self.db.execute(
                "SELECT COUNT(DISTINCT heard) n FROM edges WHERE band=? "
                "AND hearer=? AND when_s > ?",
                (self.band, call.upper(),
                 int(self.now - GRAPH_SECS))).fetchone()["n"]
            self._c[key] = got
        return got

    def p_reverse(self, a: str, b: str) -> _B:
        """Does b hear a, GIVEN a hears b? Conditioned on how
        comparable the two are -- 55% between equals, 6% when one hears
        fifty times what the other does."""
        p = reciprocity_for(self.out_degree(a), self.out_degree(b))
        return _B(p, [f"{b}->{a}: reverse of a known link -> {p:.2f}"])

    # ---- right now, out of the map dump ------------------------------

    def p_copy(self, call: str, _window=0.0) -> _B:
        """Is that station on the air? The map dump knows this minute;
        the database is up to 45 s stale and cannot."""
        c = base(call).upper()
        age = None
        for s in self.lm.active(within_s=SESSION_S * 2):
            if base(getattr(s, "call", "")).upper() == c:
                age = getattr(s, "age_s", 0.0)
                break
        if age is None:
            row = self.db.execute(
                "SELECT any_when FROM stations WHERE band=? AND call=?",
                (self.band, c)).fetchone()
            if not row or not row["any_when"]:
                return _B(0.02, [f"{c}: not heard at all today"])
            age = self.now - row["any_when"]
        p = 0.95 if age <= 900 else max(0.05, 0.95 * math.exp(
            -(age - 900) / SESSION_S))
        return _B(p, [f"{c}: last on the air {age / 60:.0f} min ago"])

    def p_hears_us(self, call: str) -> _B:
        """Have they reported hearing US? A live report is the strongest
        thing there is; otherwise the standing edge."""
        c = base(call).upper()
        for other, age, snr in self.lm.hearers_of(self.mycall, 3600):
            if base(other).upper() == c:
                return _B(0.92, [f"{c}: reported us {snr:+d} dB "
                                 f"{age / 60:.0f} min ago"])
        return self.p_link(c, self.mycall)

    def grid_of(self, call: str) -> str:
        c = base(call).upper()
        if c in self.learned_grids:
            return self.learned_grids[c]
        g = self.lm.grid_of(c)
        if g:
            return g
        row = self.db.execute(
            "SELECT grid FROM grids WHERE call=?", (c,)).fetchone()
        return (row["grid"] or "") if row else ""

    def true_grid_of(self, call: str) -> str:
        return self.grid_of(call)

    # ---- habits, out of the mined corpus ------------------------------

    def p_ans(self, call: str) -> _B:
        """Do they answer when called? Months of our own probes -- there
        is nothing in one session's data that measures this."""
        if not self.intel:
            return _B(0.55, [f"{call}: no corpus, prior"])
        rows = list(self.intel.execute(
            "SELECT answered FROM probes WHERE target=?",
            (base(call).upper(),)))
        n = len(rows)
        a = sum(r["answered"] for r in rows)
        p = (0.55 * 3.0 + a) / (3.0 + n)
        return _B(min(0.95, max(0.05, p)),
                  [f"{call}: answered {a} of {n} probes we sent"])

    def p_fwd(self, call: str) -> _B:
        """Will they pass traffic along? From relay requests we watched
        go out to them: 116 seen, 43 acted on, 37% overall, but 0% to
        100% per station -- far too wide to replace with one prior."""
        if not self.intel:
            return _B(0.37, [f"{call}: no corpus, prior"])
        row = self.intel.execute(
            "SELECT relay_asked, relay_done, relay_seen FROM stations "
            "WHERE call=?", (base(call).upper(),)).fetchone()
        if not row:
            return _B(0.37, [f"{call}: never seen, prior 0.37"])
        asked = row["relay_asked"] or 0
        done = row["relay_done"] or 0
        if asked:
            p = (0.37 * 2.0 + done) / (2.0 + asked)
            return _B(min(0.95, p),
                      [f"{call}: forwarded {done} of {asked} requests"])
        seen = row["relay_seen"] or 0
        if seen:
            p = min(0.85, 0.55 + 0.05 * math.log1p(seen))
            return _B(p, [f"{call}: seen forwarding {seen}x"])
        return _B(0.37, [f"{call}: relay never observed, prior"])


class LiveBoard:
    """The candidate shortlist, live.

    Deliberately the SAME two crude facts the replay uses -- observed
    hearing the target, or geographically near it. Widening this to
    every station reachable backwards was tried on 2026-08-24 and cost
    two points of reach (6.48% -> 4.40%): the shortlist is a better
    prior than the model's own ranking, which is a real limit on the
    ranking and not a reason to widen the net.
    """

    def __init__(self, model: LiveModel, target: str, limit=40,
                 geo_km=1200.0, max_hops=4):
        import grid as _g
        T = base(target).upper()
        me = model.mycall
        seen, pool = set(), []
        for r in model.db.execute(
                "SELECT hearer, MAX(when_s) w FROM edges WHERE band=? AND "
                "heard=? AND when_s > ? GROUP BY hearer ORDER BY w DESC",
                (model.band, T, int(model.now - GRAPH_SECS))):
            h = r["hearer"].upper()
            if h in seen or h == me or h == T:
                continue
            # A RECEIVE-ONLY SKIMMER CANNOT RELAY. Hyphenated suffixes
            # (K1RA-PI, K1RA-4) are RBN receivers -- they report what
            # they hear and never transmit, so they are excellent
            # evidence and useless as a relay. Two of them made it into
            # a dry-run plan on 2026-08-24 before this check existed.
            if not is_routable(h):
                continue
            seen.add(h)
            pool.append(h)
        tg = model.grid_of(T)
        if tg and _g.valid(tg):
            for r in model.db.execute(
                    "SELECT DISTINCT call FROM stations WHERE band=? AND "
                    "any_when > ?", (model.band,
                                     int(model.now - GRAPH_SECS))):
                c = r["call"].upper()
                if c in seen or c == me or c == T or not is_routable(c):
                    continue
                g = model.grid_of(c)
                if not (g and _g.valid(g)):
                    continue
                try:
                    if _g.distance_km(tg, g) > geo_km:
                        continue
                except ValueError:
                    continue
                seen.add(c)
                pool.append(c)
        self.pool = pool[:limit]
        self.chain = {}
        self.hears_us = {c: model.p_hears_us(c).p for c in self.pool}
        self.order = {"learned": list(self.pool)}
