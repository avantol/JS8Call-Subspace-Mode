"""search.py — INVENT A STRATEGY, TEST IT, RATE IT, CHANGE IT, REPEAT.

Andy, 2026-08-23: "given a call sign or grid, invent a search strategy.
test it, rate it (usually on time elapsed). modify the strategy, repeat
1000 times... i got the idea from training neural nets, why not though?"
and then, correcting my first attempt: "the strategy is the key, not the
weights of the probabilities. WHAT you do, not how likely a frozen
strategy can be tuned."

That correction is the design. The first version of this file searched
nine numbers attached to one fixed plan -- which could only ever make
the SAME strategy slightly better, never find a different one. So what
gets searched here is a PROGRAM: an ordered list of

        WHEN <situation>  DO <move> AIMED AT <whoever>

evaluated in a loop against a replayed piece of history. Mutation adds,
drops, reorders and rewrites those lines. Two strategies can therefore
differ in what they DO -- broadcast first and route on the answers, or
hammer directly, or buy a map, or wait for the band -- not merely in
how strongly they weigh the same evidence.

The moves and the aims are the vocabulary; every strategy is a sentence
in it, and the search writes sentences.

WHY THE POLICY IS A LOOP AND NOT A LIST.  A fixed list of transmissions
cannot react, and reacting is most of the skill: the entire value of
@ALLCALL QUERY CALL is that its answers TELL YOU who to route through.
So the world hands back an observation after every move, the policy
sees what has been learned so far, and the next line is chosen then.
A strategy that broadcasts and ignores the replies will lose to one
that broadcasts and uses them -- and that is a difference in what you
do, which is exactly what should be discoverable here.

============================ HONESTY ===============================
Each trial is half real and half modelled:

  REAL      was the station on the air (113k decodes), did a third
            party report hearing the target (edges/edge_events), how
            long every transmission takes (measured, actions.py).
  MODELLED  whether they would have ANSWERED -- a probe we never sent.

So this loop can overfit the SIMULATOR, finding a strategy that beats
a fiction. Defences, borrowed from the field the idea came from:

  FROZEN JUDGE  p_ans / p_fwd are not searchable. A strategy cannot
                tune what decides whether it succeeded.
  HOLD-OUT      trials split by DATE; the search sees only the older
                half, the winner is reported on the newer half. A gain
                that does not survive is reported as fitted noise, not
                quietly kept.
  SAME LUCK     every strategy meets the identical trials with the
                identical draws, so a score difference is a strategy
                difference and not dice.

Run:  python3 search.py --iters 1000
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import random
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import actions as A          # noqa: E402
import intel                 # noqa: E402
import invented              # noqa: E402
import sim                   # noqa: E402
from callsign import base, is_routable   # noqa: E402


class HistoryTruth:
    """Ground truth read out of the mined log: what really happened."""

    def __init__(self, db, target: str):
        self.db = db
        self.target = base(target).upper()

    # Memoised: the same (station, window) is asked over and over as
    # thousands of strategies replay the same instants, and it is the
    # single hottest query in the sweep.
    def on_air(self, call, start, end) -> bool:
        key = (call, int(start), int(end))
        hit = _AIR.get(key)
        if hit is None:
            hit = intel.sightings_between(self.db, call, key[1], key[2]) > 0
            _AIR[key] = hit
        return hit

    def target_up(self, start, end) -> bool:
        """Was the target transmitting around then?

        Not "did WE decode them" -- in every case a relay exists to
        solve, we cannot hear the target at all, so our own decodes
        would say no every time and no relay could ever succeed. The
        evidence that they were up is that SOMEBODY reported hearing
        them, which is what the mined third-party mentions are.
        """
        key = (self.target, int(start) // 900)
        got = _UP.get(key)
        if got is None:
            got = bool(self.db.execute(
                "SELECT 1 FROM edge_events WHERE heard=? "
                "AND ts BETWEEN ? AND ? LIMIT 1",
                (self.target, int(start) - 900, int(end) + 900)).fetchone())
            _UP[key] = got
        return got or self.on_air(self.target, start, end + 60)

    def audible(self, call, start, end) -> bool:
        """In the log, "we decoded them" IS "we can hear them" -- the
        record only exists because we copied it."""
        return self.on_air(call, start, end)

    def hears(self, a, b, start, end) -> bool:
        w0, w1 = int(start - 1800), int(end + 1800)
        key = (a, b, w0, w1)
        hit = _PAIR.get(key)
        if hit is None:
            hit = bool(intel.pair_heard_between(self.db, a, b, w0, w1))
            _PAIR[key] = hit
        return hit or self.on_air(b, start, end + 120)

    def neighbours(self, call, when) -> list:
        """Who that station was actually reported hearing around then --
        the real content of a HEARING? reply, out of the mined log."""
        key = (call, int(when) // 900)
        got = _NEIGH.get(key)
        if got is None:
            w0, w1 = int(when) - 3600, int(when) + 900
            got = [r["heard"] for r in self.db.execute(
                "SELECT DISTINCT heard FROM edge_events WHERE hearer=? "
                "AND ts BETWEEN ? AND ? LIMIT 4", (call, w0, w1))]
            if not got:
                got = [r["heard"] for r in self.db.execute(
                    "SELECT heard FROM edges WHERE hearer=? "
                    "ORDER BY last_when DESC LIMIT 4", (call,))]
            _NEIGH[key] = got
        return got


class Board:
    """Everything about one historical instant that does NOT depend on
    the strategy: who could relay, and the order each aim would pick
    them in. Built once per trial and shared by every strategy that
    replays it -- otherwise each play repeats a geographic scan over
    every station with a grid, which was the whole cost of the sweep.
    """

    def __init__(self, model, target: str, mycall: str):
        # Chains first: they decide who is worth having in the pool.
        self.chain = self._chains(model, target, mycall)

        near = [c for c in model.relay_candidates(target, limit=24)
                if is_routable(c) and base(c) != base(target)
                and base(c) != base(mycall)]

        # A multi-hop chain is only usable if we can reach its FIRST
        # station ourselves -- the chain covers the distance beyond
        # that. Ranking candidates by how well they hear US is
        # therefore the right filter, and without it the pool held
        # only stations near the TARGET, of which almost none started
        # a short chain (3 of 15 on the first case tried).
        extra = sorted((c for c in self.chain
                        if is_routable(c) and c not in near
                        and base(c) != base(mycall)
                        and base(c) != base(target)),
                       key=lambda c: (len(self.chain[c]),
                                      -model.p_hears_us(c).p))[:40]
        self.pool = near + extra

        hears_us = {c: model.p_hears_us(c).p for c in self.pool}
        copy = {c: model.p_copy(c, 3600.0).p for c in self.pool}
        link = {c: model.p_link(c, target).p for c in self.pool}
        fwd = {c: model.p_fwd(c).p for c in self.pool}
        self.hears_us = hears_us
        self.order = {
            "strongest": sorted(self.pool, key=lambda c: -hears_us[c]),
            "freshest":  sorted(self.pool, key=lambda c: -copy[c]),
            "nearest":   sorted(self.pool, key=lambda c: -link[c]),
            "forwarder": sorted(self.pool, key=lambda c: -fwd[c]),
            "unknown":   sorted(self.pool, key=lambda c: link[c]),
            "learned":   list(self.pool),
        }

    def _chains(self, model, target, mycall) -> dict:
        """Walk BACKWARDS from the target to find delivery chains.

        Direction matters and is easy to get backwards. To hand a
        message to somebody, THEY have to hear the one before them. So
        a chain that delivers to T is:

            R1 hears US, R2 hears R1, ... , T hears Rh

        which means the walk starts at the target and asks "who does T
        hear?", then "who do THEY hear?", outward towards us -- exactly
        the way Andy read it off the map by hand on 2026-08-22 ("work
        backwards from the target, it pops right out").

        Each step is a standing fact from the edge table: whether the
        path exists at all. Whether every station on it is awake is
        checked later, at the moment of the attempt, against real
        decodes -- the link is the slow-moving part, being on the air
        is the fast-moving part.
        """
        db = model.db
        me = base(mycall)
        target = base(target).upper()
        seen = {target}
        chains: dict = {}
        # frontier: station -> the chain from it back to the target
        frontier = {target: []}
        for _hop in range(MAX_HOPS):
            nxt: dict = {}
            for station, tail in frontier.items():
                rows = db.execute(
                    "SELECT heard FROM edges WHERE hearer=? "
                    "ORDER BY n DESC LIMIT 24", (station,)).fetchall()
                for r in rows:
                    nxt_call = r["heard"]
                    if nxt_call in seen or base(nxt_call) == me:
                        continue
                    seen.add(nxt_call)
                    nxt[nxt_call] = [nxt_call] + tail
                    if nxt_call not in chains:
                        chains[nxt_call] = [nxt_call] + tail
                if len(nxt) > 400:
                    break
            if not nxt:
                break
            frontier = nxt
        return chains


_AIR: dict = {}          # (call, start, end) -> were they on the air
_PAIR: dict = {}         # (a, b, w0, w1)      -> did a report hearing b
_NEIGH: dict = {}        # (call, quarter-hour) -> who they were hearing
_UP: dict = {}           # (call, quarter-hour) -> was anyone hearing them

CAP_S = 1800.0           # half an hour of airtime is the whole budget
PERIOD_S = 15.0


# ============================== HOW STATIONS BEHAVE, AND HOW MUCH THEY
#                                                        CHANGE THEIR MIND
#
# Andy: "keep the same stations behaving mostly the same each run. maybe
# dither the stations more as you converge, to detect sensitivity to
# instability."
#
# A station answering or forwarding is mostly a STANDING HABIT -- its
# autoreply setting, whether an operator sits there at that hour -- not
# a fresh coin flip every time we call. So its behaviour is drawn from
# its callsign and stays put; `dither` is how much of that is replaced
# by a per-run wobble:
#
#     dither 0.0   every station behaves identically in every run.
#                  Strategies are compared against a fixed world.
#     dither 1.0   every station re-decides every run. Nothing is
#                  learnable about a station, only about tactics.
#
# The search converges at low dither and is then RE-SCORED at rising
# dither. A strategy that only wins when the world holds still is
# leaning on stations it cannot really count on, and the sensitivity
# table at the end says so out loud.
#
# This also fixes a real defect in the first version: drawing from an
# RNG stream in the order a strategy happened to ask meant the SAME
# station handed different luck to different strategies, so the
# comparison was never actually paired. Keyed to the callsign, it is.

def _u(*parts) -> float:
    """A repeatable number in [0,1) for these exact parts."""
    key = "\x1f".join(str(p) for p in parts).encode()
    return int.from_bytes(hashlib.blake2b(key, digest_size=8).digest(),
                          "big") / float(1 << 64)


def disposition(call: str, kind: str, trial: int, dither: float) -> float:
    """Blend the station's standing habit with this run's wobble."""
    habit = _u(kind, call)
    today = _u(kind, call, trial)
    return (1.0 - dither) * habit + dither * today


# ======================================================= THE VOCABULARY
#
# MOVES -- what you can put on the air. Cost in seconds is measured
# (actions.py), not guessed.

# THE BUILDING BLOCKS -- the actual JS8 queries, each kept separate so
# the search can put each one at a different stage. Lumping them into
# "direct" and "broadcast" hid the whole question: SNR? / GRID? /
# STATUS? / HEARING? / QUERY CALL differ in what they cost, who they
# are aimed at, and what comes back, and finding where each belongs in
# the order is the point of the exercise.
#
#   name         on the air                cost   what it yields
MOVE_COST = {
    "snr":        A.rtt(1, 1),          #  67s  T answers => CONTACT
    "grid":       A.rtt(1, 2),          #  82s  contact + position
    # STATUS? and INFO? are NOT in the pool. Both answer with the
    # operator's own free text, unbounded in length -- Andy measured
    # his own STATUS reply at two minutes and has seen longer ones:
    # "it is NOT a useful query." QSL? is not auto-replied at all, so
    # it cannot work unattended. MSG TO: was ruled out for routing.
    "ask_grid":   A.rtt(1, 2),          #  82s  WHERE is that station?
    "ask_call":   A.rtt(2, 2),          #  97s  ONE station: do you hear T?
    "shout_call": A.rtt(2, 2),          #  97s  EVERYONE: who hears T?
    "hearing":    A.rtt(1, 4),          # 112s  X's top-4 neighbours
    "relay1":     A.rtt(2, 2, 1),       # 218s  reach T through one station
    "relay2":     A.rtt(2, 2, 2),       # 338s  ... through two
    "relay3":     A.rtt(2, 2, 3),       # 458s  ... through three
    "relay4":     A.rtt(2, 2, 4),       # 578s  ... through four
    "wait":       4 * PERIOD_S,         #  60s  let the band turn over
    "stop":       0.0,                  #   0s  decide they are not there
}
MOVES = tuple(MOVE_COST)

# Of each block's round trip, how much is US transmitting. The rest is
# waiting for them. Relays key us once; the forwarding is their airtime,
# charged to them, not to us.
TX_SECONDS = {
    "snr": PERIOD_S, "grid": PERIOD_S, "status": PERIOD_S,
    "ask_call": 2 * PERIOD_S, "shout_call": 2 * PERIOD_S,
    "hearing": PERIOD_S,
    "relay1": 2 * PERIOD_S, "relay2": 2 * PERIOD_S,
    "relay3": 2 * PERIOD_S, "relay4": 2 * PERIOD_S,
}

# HOW FAR TO CHASE A CHAIN. Every extra hop costs a flat 120 s AND
# multiplies in another station that has to be up and willing, so the
# two effects compound: at four hops only three attempts fit in the
# half-hour budget and each needs five stations to behave. Four is
# where we stop looking, but WHERE IT STOPS PAYING is left to the
# search -- we have done three hops on the air for real, so the answer
# is not obviously one.
HOPS = {"relay1": 1, "relay2": 2, "relay3": 3, "relay4": 4}
MAX_HOPS = 4

# The three that reach the TARGET directly. All of them mean contact if
# answered, so they differ only in price and in what else they tell us
# -- which is exactly the sort of thing the search should settle.
DIRECT_MOVES = ("snr", "grid")

# The ones aimed at somebody else, which therefore need an aim.
AIMED_MOVES = ("ask_grid", "ask_call", "hearing",
               "relay1", "relay2", "relay3", "relay4")

# AIMS -- how a move chooses WHO to aim at. These are the operator
# instincts that were previously in my head, made selectable so the
# search can find out which of them actually pays.
AIMS = (
    "unlocated",   # one whose position we do NOT know -- for GRID?
    "learned",     # someone a reply proved hears the target
    "strongest",   # best signal to us
    "freshest",    # heard by us most recently
    "nearest",     # closest to the target by grid
    "forwarder",   # has relayed for us before
    "unknown",     # one we know least about -- buys information
)

# SITUATIONS -- what a stage can wait for. A strategy is mostly a claim
# about WHEN each block is worth its seconds.
WHENS = (
    "always",
    "nothing_learned",     # no third-party evidence yet
    "something_learned",   # we know who hears the target
    "target_quiet",        # we have not copied the target ourselves
    "target_active",       # we HAVE copied the target recently
    "late",                # more than half the budget is gone
    "early",               # less than a third of the budget is gone
)


@dataclass(frozen=True)
class Line:
    when: str
    move: str
    aim: str

    def __str__(self) -> str:
        if self.move not in AIMED_MOVES:
            return f"when {self.when:17s} {self.move}"
        return f"when {self.when:17s} {self.move} at {self.aim}"


@dataclass(frozen=True)
class Strategy:
    lines: tuple

    def __str__(self) -> str:
        return "\n".join(f"    {i+1}. {ln}" for i, ln in enumerate(self.lines))

    def short(self) -> str:
        return " | ".join(
            f"{ln.move}" + (f":{ln.aim}" if ln.move in AIMED_MOVES else "")
            + ("" if ln.when == "always" else f"?{ln.when}")
            for ln in self.lines)


# ---------------------------------------------------------- the writers

def random_line(rng: random.Random) -> Line:
    return Line(rng.choice(WHENS), rng.choice(MOVES), rng.choice(AIMS))


def random_strategy(rng: random.Random) -> Strategy:
    return Strategy(tuple(random_line(rng)
                          for _ in range(rng.randint(1, 5))))


def mutate(s: Strategy, rng: random.Random) -> Strategy:
    """Rewrite the PROGRAM: add a line, drop one, move one, or change
    one word of one line. Editing the text is what lets the search
    arrive at a strategy nobody wrote down."""
    lines = list(s.lines)
    what = rng.choice(["add", "drop", "swap", "edit", "edit", "edit"])
    if what == "add" or not lines:
        lines.insert(rng.randint(0, len(lines)), random_line(rng))
    elif what == "drop" and len(lines) > 1:
        lines.pop(rng.randrange(len(lines)))
    elif what == "swap" and len(lines) > 1:
        i = rng.randrange(len(lines) - 1)
        lines[i], lines[i + 1] = lines[i + 1], lines[i]
    else:
        i = rng.randrange(len(lines))
        ln = lines[i]
        field = rng.choice(["when", "move", "aim"])
        if field == "when":
            lines[i] = Line(rng.choice(WHENS), ln.move, ln.aim)
        elif field == "move":
            lines[i] = Line(ln.when, rng.choice(MOVES), ln.aim)
        else:
            lines[i] = Line(ln.when, ln.move, rng.choice(AIMS))
    return Strategy(tuple(lines[:8]))


def cross(a: Strategy, b: Strategy, rng: random.Random) -> Strategy:
    """Splice two programs. Cheap, and it combines a good opening from
    one strategy with a good fallback from another."""
    i = rng.randint(0, len(a.lines))
    j = rng.randint(0, len(b.lines))
    return Strategy(tuple((a.lines[:i] + b.lines[j:])[:8]) or a.lines)


# ============================================================= THE WORLD
#
# Replays one historical instant and answers moves with a mixture of
# real history and the frozen answer model.

class World:
    def __init__(self, truth, model, board, target: str, t0: int,
                 trial: int, dither: float):
        self.truth, self.model, self.target = truth, model, base(target).upper()
        self.board = board
        self.t0 = float(t0)
        self.t = float(t0)
        self.trial, self.dither = trial, dither
        self.willing: dict = {}
        self.forwarding: dict = {}
        self.learned: dict = {}      # call -> proved it hears the target
        self.relocated: set = set()  # told us where they are
        self.tried: dict = {}        # (what, who) -> when we last did it
        self.asked_grid: set = set()
        self.heard_of: set = set()   # named by someone else's HEARING reply
        self.asked: set = set()
        self.reached = False
        self.gave_up = False
        self.keyed = 0.0            # seconds we actually transmitted
        self.hops_used = 0
        self.moves = 0

    # ---- ground truth ------------------------------------------------
    # Delegated to a truth object so the same strategy runner can face
    # replayed history OR an invented band without knowing which. The
    # four questions below are ALL a strategy can ever learn about the
    # world; keeping them in one place is what makes the invented band
    # a drop-in rather than a parallel copy of this file.
    def _on_air(self, call, start, end):
        """Is that station active right now? Says NOTHING about whether
        we can hear it -- a relay in the middle of a chain need not be
        audible to us at all."""
        return self.truth.on_air(call, start, end)

    def _audible(self, call, start, end):
        """Can WE copy that station? Needed wherever a reply has to
        reach US: a direct call, a question we asked, and the last leg
        home from a relay chain. Conflating this with "is it active"
        let a direct call succeed against a target we cannot hear,
        which is the defining feature of every relay case -- so plain
        SNR? won the invented band outright and the search learned
        nothing."""
        return self.truth.audible(call, start, end)

    def _target_up(self, start, end) -> bool:
        return self.truth.target_up(start, end)

    def _hears_target(self, call, start, end) -> bool:
        return self.truth.hears(call, self.target, start, end)

    def _hears(self, a, b, start, end) -> bool:
        return self.truth.hears(a, b, start, end)

    def _neighbours_of(self, call, when) -> list:
        return self.truth.neighbours(call, when)

    # ---- how a station behaves ---------------------------------------
    def _answers(self, call) -> bool:
        if call not in self.willing:
            u = disposition(call, "ans", self.trial, self.dither)
            self.willing[call] = u <= self.model.p_ans(call).p
        return self.willing[call]

    def _forwards(self, call) -> bool:
        if call not in self.forwarding:
            u = disposition(call, "fwd", self.trial, self.dither)
            self.forwarding[call] = u <= self.model.p_fwd(call).p
        return self.forwarding[call]

    # ---- who to aim at -----------------------------------------------
    def pool(self) -> list:
        return self.board.pool

    def aim(self, how: str, want_chain: int = 0):
        if how == "unlocated":
            for c in self.board.pool:
                if c not in self.asked_grid and not self.model.grid_of(c):
                    return c
            return None
        if how == "learned":
            known = [c for c in self.board.order["learned"]
                     if c in self.learned and c not in self.asked]
            if not known:
                return None
            return max(known, key=lambda c: self.learned[c])
        order = self.board.order.get(how, self.board.pool)
        if how == "nearest" and self.relocated:
            # A station that has just told us where it is may now be
            # the best geographic candidate, and the precomputed
            # ordering was built when we did not know.
            fresh = sorted(self.relocated,
                           key=lambda c: -self.model.p_link(c, self.target).p)
            order = fresh + [c for c in order if c not in self.relocated]
        if want_chain:
            order = [c for c in order
                     if len(self.board.chain.get(c, [])) <= want_chain]
        # Stations another station named in a HEARING? reply go first:
        # that is fresh, on-air evidence they are up right now, which is
        # the whole reason the extra 15 s of HEARING? might be worth it.
        for c in order:
            if c in self.heard_of and c not in self.asked:
                return c
        for c in order:
            if c not in self.asked:
                return c
        return None

    # ---- situations --------------------------------------------------
    def holds(self, when: str) -> bool:
        spent = self.t - self.t0
        if when == "always":
            return True
        if when == "nothing_learned":
            return not self.learned
        if when == "something_learned":
            return bool(self.learned)
        if when == "target_quiet":
            return not self._on_air(self.target, self.t - 900, self.t)
        if when == "target_active":
            return self._on_air(self.target, self.t - 900, self.t)
        if when == "late":
            return spent > CAP_S * 0.5
        if when == "early":
            return spent < CAP_S / 3.0
        return True

    def do_forced(self, line: Line, who) -> bool:
        """do(), but with the station already chosen by the decider."""
        self._forced = who
        try:
            return self.do(line)
        finally:
            self._forced = None

    # ---- doing a move ------------------------------------------------
    def do(self, line: Line) -> bool:
        """Put one building block on the air. True if we reached T."""
        self.moves += 1
        m = line.move
        start = self.t
        self.t = end = start + MOVE_COST[m]
        # WHAT WE PUT ON THE AIR, as distinct from how long we waited.
        # Ranked on elapsed time alone, a stage that keys up for nothing
        # is free -- the run lasts the same half hour either way -- so
        # the search had no reason to remove one, and the best relay
        # strategy carried a repeated broadcast worth 0.0 points and 1.3
        # extra transmissions per attempt. Charging keyed seconds makes
        # a useless transmission cost something, which is also the
        # courtesy cost to everybody else on the channel.
        if m not in ("wait", "stop"):
            self.keyed += TX_SECONDS.get(m, PERIOD_S)

        if m == "wait":
            return False        # costs the clock, keys nothing

        if m == "stop":
            # GIVING UP IS A MOVE. When nobody knows whether the target
            # is even on the air, almost nothing works whatever you do
            # -- 0.27% of the time -- so what separates strategies is
            # not whether they get through but how much airtime they
            # burn finding out. Without this the runner always spent
            # the full half hour and every cold strategy scored the
            # same. With reach ranked first, a strategy that tries a
            # little and then stops still beats one that stops at once,
            # so this rewards knowing when to quit rather than quitting.
            self.gave_up = True
            return False

        # --- aimed at the target: any answer IS contact -------------
        if m in DIRECT_MOVES:
            if self._audible(self.target, start, end + 60) \
                    and self._answers(self.target):
                self.reached = True
            return self.reached

        # --- shouted at everybody -----------------------------------
        if m == "shout_call":
            # One transmission; every station that hears the target and
            # is on the air answers. THIS is what makes it worth 97 s,
            # and the policy has to choose to USE what comes back.
            for c in self.pool():
                if (self._audible(c, start, end + 60)
                        and self._hears_target(c, start, end)
                        and self._answers(c)):
                    self.learned[c] = self.board.hears_us[c]
            return False

        who = getattr(self, "_forced", None) or self.aim(line.aim,
                                                         HOPS.get(m, 0))
        if who is None:
            self.t = start + PERIOD_S      # a wasted decision still costs
            return False

        # --- where are you? -----------------------------------------
        if m == "ask_grid":
            # GRID? aimed at somebody ELSE, not at the target. Andy,
            # 2026-08-23: "I *DO* see how GRID? can help." It is the
            # only way to find out where a station that just answered
            # you actually is -- and without that, "who is near the
            # target" is guesswork about stations whose location was
            # never asked for. A quarter of relay candidates have no
            # grid we knew at the time, and they can never be picked
            # geographically until one of them is asked.
            self.asked_grid.add(who)
            if self._audible(who, start, end + 60) and self._answers(who):
                g = getattr(self.model, "true_grid_of", lambda _c: "")(who)
                if g:
                    self.model.learned_grids[who] = g
                    self.relocated.add(who)
            return False

        # --- asked of one station -----------------------------------
        if m == "ask_call":
            # "X, do you hear T?" -- one boolean from one chosen
            # station. Cheaper than HEARING? and answers the routing
            # question directly, but tells us nothing we did not ask.
            self.asked.add(who)
            if self._audible(who, start, end + 60) and self._answers(who):
                if self._hears_target(who, start, end):
                    self.learned[who] = self.board.hears_us[who]
            return False

        if m == "hearing":
            # "X, what are you hearing?" -- costs 15 s more than
            # ask_call and returns X's whole recent neighbour list, so
            # it can name stations we had no reason to ask about. That
            # extra reach is modelled, not assumed: the neighbours come
            # out of what X was ACTUALLY reported hearing at the time.
            self.asked.add(who)
            if self._audible(who, start, end + 60) and self._answers(who):
                for n in self._neighbours_of(who, start):
                    if n == self.target:
                        self.learned[who] = self.board.hears_us[who]
                    elif n in self.board.hears_us:
                        self.heard_of.add(n)
            return False

        # --- routed through a chain ---------------------------------
        if m in HOPS:
            self.asked.add(who)
            chain = self.board.chain.get(who)
            if not chain or len(chain) > HOPS[m]:
                return False        # no path that short exists from here
            # Every station on the chain has to be awake and willing to
            # pass traffic; the last one has to be one the target can
            # actually hear, which is dated evidence, not a standing
            # guess. Then the target still has to choose to answer.
            for station in chain:
                if not self._on_air(station, start, end + 60):
                    return False
                if not self._forwards(station):
                    return False
            # NO "does the last station hear the target" check here.
            # That is the WRONG WAY ROUND and it was in the first cut of
            # this: to hand a message TO the target, the TARGET has to
            # hear the last station, not the other way about. The chain
            # was already built in the delivering direction, so the link
            # is established -- what is left to check is that the target
            # was actually on the air to receive it.
            if not self._target_up(start, end):
                return False
            if not self._answers(self.target):
                return False
            # AND THE ANSWER HAS TO GET BACK. Only 13% of links work
            # both ways, so a chain that delivers is not a chain that
            # returns -- and the reply retraces the same stations, each
            # of which must hear the one before it in the OTHER
            # direction. Leaving this out counted every delivery as a
            # contact and made relaying look far better than it is.
            back = list(reversed(chain))
            prev = self.target
            for station in back:
                if not self._hears(station, prev, start, end):
                    return False
                prev = station
            # The last leg home is ours: we have to be able to COPY the
            # first station, not merely know it was active.
            if not self._audible(chain[0], start, end + 60):
                return False
            self.reached = True
            self.hops_used = len(chain)
            return True

        return False


def play(truth, model, board, target: str, t0: int, s: Strategy,
         trial: int, dither: float) -> tuple[bool, float]:
    """Run one strategy against one situation, real or invented."""
    w = World(truth, model, board, target, t0, trial, dither)
    guard = 0
    # STAGES IN ORDER, then round again. Not first-match: under
    # first-match a leading "when always" line made every later line
    # unreachable, so adding a stage after it changed nothing, the
    # sweep measured no improvement, and the search could never grow
    # past one line. It is also the wrong reading of the question --
    # "what if i did THIS at this stage rather than THAT" is about a
    # sequence of stages, and a stage whose situation does not hold is
    # simply skipped, costing nothing.
    while w.t - w.t0 < CAP_S and guard < 60 and not w.gave_up:
        guard += 1
        did_something = False
        for ln in s.lines:
            if w.t - w.t0 >= CAP_S or w.gave_up:
                break
            if not w.holds(ln.when):
                continue           # skip this stage, free
            did_something = True
            if w.do(ln):
                return True, w.t - w.t0
        if not did_something:
            w.t += PERIOD_S        # nothing applied: wait a period
    return False, min(w.t - w.t0, CAP_S)


# ============================================================== TRIALS

@dataclass
class Trial:
    target: str
    t0: int
    luck: int


_BAND_TIMES: list = []


def _band_active_times(db) -> list:
    """Moments when WE were at the radio decoding SOMETHING.

    The first version of this sampler drew uniformly across the whole
    span a station was ever active, which put most instants at 3am on a
    Tuesday with the band dead -- 1% reach and no signal to search. The
    operator is never in that situation. He is sitting at a working
    radio with traffic coming in, and he wants one particular station.
    Sampling from times we were decoding anything reproduces THAT, and
    leaves the question of whether the target in particular is up --
    which is the actual uncertainty a strategy has to handle.
    """
    global _BAND_TIMES
    if not _BAND_TIMES:
        _BAND_TIMES = [r[0] for r in db.execute(
            "SELECT DISTINCT ts / 1800 * 1800 FROM sightings ORDER BY 1")]
    return _BAND_TIMES


def cold_instants(db, target, n, rng) -> list:
    """Instants chosen WITHOUT regard to whether the target is up.

    The other two samplers both hand the strategy a target that was
    about to be reachable -- one directly, one through a third party.
    Neither is the situation the operator is in when he says "reach
    K3FHP": the band is open, traffic is flowing, and nobody knows
    whether K3FHP in particular is on the air, in bed, or off band. A
    strategy has to find that out cheaply, and this is the only one of
    the three cases where giving up early can be the right move.
    """
    times = _band_active_times(db)
    if not times:
        return []
    return [rng.choice(times) for _ in range(n)]


def play_decider(truth, model, board, target, t0, trial, dither,
                 max_hops=4):
    """Run the COMPUTED decision rule instead of a written program.

    Same world, same costs, same ground truth as `play` -- the only
    difference is where the next message comes from, so the two are
    directly comparable.
    """
    import decide
    w = World(truth, model, board, target, t0, trial, dither)
    d = decide.Decider(model, board, target, model.mycall, max_hops)
    KIND = {"snr": "snr", "shout": "shout_call",
            "hearing": "hearing", "grid": "ask_grid"}
    while w.t - w.t0 < CAP_S and not w.gave_up:
        mv = d.choose(w)
        w.tried[(mv.kind, mv.via or (target if mv.kind == "snr" else ""))] = w.t
        if mv.kind == "relay":
            hops = len(mv.chain)
            ln = Line("always", f"relay{hops}", "learned")
            start = w.t
            w.moves += 1
            w.t = end = start + MOVE_COST[f"relay{hops}"]
            w.asked.add(mv.via)
            w.tried[("relay", mv.via)] = start
            ok = True
            for st in mv.chain:
                if not w._on_air(st, start, end + 60) or not w._forwards(st):
                    ok = False
                    break
            if ok and w._target_up(start, end) and w._answers(w.target):
                prev = w.target
                for st in reversed(mv.chain):
                    if not w._hears(st, prev, start, end):
                        ok = False
                        break
                    prev = st
                if ok and w._audible(mv.chain[0], start, end + 60):
                    return True, w.t - w.t0
            continue
        ln = Line("always", KIND[mv.kind], "learned")
        if mv.via:
            w._forced_aim = mv.via
        if w.do_forced(ln, mv.via):
            return True, w.t - w.t0
    return False, min(w.t - w.t0, CAP_S)


def make_trials(db, rng, n_targets, per_target, relay_cases,
                cold=False) -> list:
    rows = db.execute(
        "SELECT call FROM stations WHERE heard_count > 20 "
        "ORDER BY heard_count DESC LIMIT 400").fetchall()
    calls = [r["call"] for r in rows if is_routable(r["call"])]
    rng.shuffle(calls)
    out, seen = [], set()
    for c in calls:
        if len(seen) >= n_targets:
            break
        got = (cold_instants(db, c, per_target, rng) if cold
               else sim.pick_instants(db, c, per_target, rng,
                                      relay_cases=relay_cases))
        if got:
            seen.add(c)
            out += [Trial(c, ts, rng.randrange(1 << 30)) for ts in got]
    return out


_MADE_UP: dict = {}


def made_up_trials(n: int, first: int = 0, relay_only: bool = False,
                   cold: bool = False) -> list:
    """n invented situations. Built once and reused, so every strategy
    meets the identical bands -- the same paired comparison the
    historical trials get."""
    out = []
    for seed in range(first, first + n):
        key = (seed, relay_only, cold)
        got = _MADE_UP.get(key)
        if got is None:
            got = invented.trial(seed, relay_only=relay_only, cold=cold)
            _MADE_UP[key] = got
        if got:
            out.append((seed, got))
    return out


def score_of(secs: float, reach: float) -> float:
    """ONE number to rank by: reach first, elapsed time only to break
    ties. Lower is better.

    Andy chose this on 2026-08-23, after the search turned up a
    strategy that reached MORE targets (16 of 256 against 15) while
    scoring worse on plain elapsed time, because its contacts took
    1105 s each against 746 s. Ranking on time alone had quietly
    decided that the extra contact was not worth the wait -- a
    judgement that was living inside CAP_S, a number I picked, rather
    than being anybody's decision.

    A contact you never make is worth nothing, so reach leads. Time
    still matters, but only between strategies that get through as
    often as each other.
    """
    return -round(reach, 4) * 1e6 + secs


def rate_made_up(s: Strategy, trials: list, dither: float = 0.0) -> tuple:
    """Score a strategy on invented bands. Unlimited supply, and no
    particular station to memorise -- every trial draws a new band."""
    total, hits = 0.0, 0
    for seed, (_band, target, t0, model, board, truth) in trials:
        ok, secs = play(truth, model, board, target, t0, s, seed, dither)
        total += secs
        hits += 1 if ok else 0
    n = max(1, len(trials))
    return total / n, hits / n


def rate(db, s: Strategy, trials: list, mycall: str,
         cache: dict, dither: float = 0.0) -> tuple[float, float]:
    """Mean seconds ELAPSED, and reach rate.

    Elapsed, not keyed: Andy set the metric ("that's exactly what we
    are supposed to be ranking strategies on"). Keyed seconds are still
    counted, and reported, because they are the courtesy cost -- but
    they do not decide the ranking.

    Time is what was ACTUALLY spent, including on failures, rather than
    an automatic half hour -- because with reach ranked first, a
    strategy can no longer look good by reaching nobody quickly, and
    charging real time is what lets giving up cheaply count in its
    favour when the target was never there.
    """
    total, hits = 0.0, 0
    for tr in trials:
        key = (tr.target, tr.t0)
        got = cache.get(key)
        if got is None:
            model = sim.HorizonModel(db, tr.t0)
            model.mycall = mycall
            got = (model, Board(model, tr.target, mycall),
                   HistoryTruth(db, tr.target))
            cache[key] = got
        model, board, truth = got
        ok, secs = play(truth, model, board, tr.target, tr.t0, s, tr.luck,
                        dither)
        total += secs
        hits += 1 if ok else 0
    n = max(1, len(trials))
    return total / n, hits / n


# ============================================================== SEARCH

# What I would have written by hand -- the thing to beat.
HAND = Strategy((
    Line("target_active", "snr", "learned"),
    Line("nothing_learned", "shout_call", "learned"),
    Line("something_learned", "relay1", "learned"),
    Line("always", "relay1", "nearest"),
))

DIRECT_ONLY = Strategy((Line("always", "snr", "learned"),))


def all_lines() -> list:
    """Every line the vocabulary can express -- 105 of them.

    Small enough to try EXHAUSTIVELY at each stage, which is what Andy
    asked for: "what if i did THIS at this stage rather than THAT which
    i just tried. run thru the combinations." Random mutation only ever
    samples this set; sweeping it means a better line at a given stage
    cannot be missed by luck.
    """
    out = []
    for when in WHENS:
        for mv in MOVES:
            if mv in AIMED_MOVES:
                for aim in AIMS:
                    out.append(Line(when, mv, aim))
            else:
                out.append(Line(when, mv, "learned"))   # aim is unused
    return out


def neighbours(s: Strategy, lines: list) -> list:
    """Every one-edit variation: what else could stand at each stage,
    what if a stage were added, dropped, or two stages traded places."""
    out, seen = [], {s.lines}
    n = len(s.lines)
    for i in range(n):                                  # THIS not THAT
        for ln in lines:
            cand = s.lines[:i] + (ln,) + s.lines[i + 1:]
            if cand not in seen:
                seen.add(cand)
                out.append(Strategy(cand))
    if n < 8:                                           # one stage more
        for i in range(n + 1):
            for ln in lines:
                cand = s.lines[:i] + (ln,) + s.lines[i:]
                if cand not in seen:
                    seen.add(cand)
                    out.append(Strategy(cand))
    if n > 1:                                           # one stage fewer
        for i in range(n):
            cand = s.lines[:i] + s.lines[i + 1:]
            if cand not in seen:
                seen.add(cand)
                out.append(Strategy(cand))
        for i in range(n - 1):                          # trade places
            l = list(s.lines)
            l[i], l[i + 1] = l[i + 1], l[i]
            cand = tuple(l)
            if cand not in seen:
                seen.add(cand)
                out.append(Strategy(cand))
    return out


def sweep(score, start: Strategy, dither, budget, verbose=True) -> tuple:
    """Keep taking the single best available edit until none helps.

    `score(strategy, dither) -> (seconds, reach)` is passed in rather
    than hard-wired, which is what lets the identical search run against
    replayed history or an invented band.
    """
    lines = all_lines()
    best = start
    best_secs, best_reach = score(best, dither)
    best_score = score_of(best_secs, best_reach)
    spent = 1
    round_no = 0
    while spent < budget:
        round_no += 1
        improved = None
        for cand in neighbours(best, lines):
            if spent >= budget:
                break
            secs, reach = score(cand, dither)
            spent += 1
            ranked = score_of(secs, reach)
            if ranked < best_score - 0.5:
                best_score, best_secs, best_reach = ranked, secs, reach
                improved = cand
        if improved is None:
            break
        best = improved
        if verbose:
            print(f"    sweep {round_no}: reach {best_reach:5.1%}  "
                  f"{best_secs:6.0f}s   {best.short()}")
    return best, best_secs, best_reach, spent


def search(score, iters, rng, verbose=True):
    """Converge at a steady world, then shake the stations and re-check.

    The ramp is the point of the dither: a strategy that survives the
    world wobbling is one whose value comes from what it DOES, not from
    a handful of stations that happened to be reliable in replay.
    """
    seeds = [HAND, DIRECT_ONLY] + [random_strategy(rng) for _ in range(4)]
    scored = [(score_of(*score(s, 0.0)), s) for s in seeds]
    scored.sort(key=lambda x: x[0])
    best = scored[0][1]
    spent = len(seeds)
    if verbose:
        secs, reach = score(best, 0.0)
        print(f"  best seed: reach {reach:5.1%}  {secs:6.0f}s   {best.short()}")

    # SEARCH IN A FIXED WORLD. Andy, 2026-08-23: "each run should
    # produce the same result UNTIL we dither the probabilities... to
    # determine the sensitivity of a promising strategy (after ALL are
    # tried) to variations."
    #
    # The first version swept at dither 0.00, then 0.25, then 0.50 --
    # which moves the world while the search is walking through it, so
    # a score from one stage cannot be compared with a score from the
    # next, and the "improvements" logged after each change of dither
    # were partly just a different world. Every strategy is now tried
    # against one identical, repeatable band. Dither comes afterwards,
    # applied to the winner, and answers a different question: does
    # this strategy still hold up when stations stop behaving the way
    # they did.
    best, sc, rc, used = sweep(score, best, 0.0, iters - spent, verbose)
    spent += used
    return best, spent


def sensitivity(score, s: Strategy) -> list:
    """How the strategy holds up as stations stop behaving predictably."""
    return [(d,) + tuple(score(s, d)) for d in (0.0, 0.25, 0.5, 0.75, 1.0)]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iters", type=int, default=1000,
                    help="strategy evaluations to spend")
    ap.add_argument("--targets", type=int, default=20)
    ap.add_argument("--per-target", type=int, default=3)
    ap.add_argument("--mycall", default="WM8Q")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--relay-cases", action="store_true",
                    help="only instants where a relay was the ONLY option")
    ap.add_argument("--cold", action="store_true",
                    help="random instants -- nobody knows if the target is up")
    ap.add_argument("--invented", type=int, default=0, metavar="N",
                    help="search on N INVENTED bands instead of replayed "
                         "history, then sit the exam on real instants")
    args = ap.parse_args()

    db = intel.connect()
    rng = random.Random(args.seed)
    real = make_trials(db, rng, args.targets, args.per_target,
                       args.relay_cases, args.cold)
    real.sort(key=lambda t: t.t0)
    kind = ("cold instants" if args.cold else
            "relay-only instants" if args.relay_cases else "general instants")
    cache: dict = {}

    if args.invented:
        # Tune on made-up bands -- unlimited, and with no particular
        # station to memorise, since every trial draws a new band. Then
        # sit the exam on real history, ALL of which is held out: not
        # one of these instants was seen while searching.
        made = made_up_trials(args.invented,
                              relay_only=args.relay_cases,
                              cold=args.cold)
        print(f"searching on {len(made)} invented bands; exam = "
              f"{len(real)} real {kind}, none of them seen while "
              f"searching\n", flush=True)
        tune_score = lambda st, d: rate_made_up(st, made, d)     # noqa: E731
        exam_score = lambda st, d: rate(db, st, real, args.mycall, cache, d)
        exam_trials, exam_name = real, "real history"
    else:
        cut = len(real) // 2
        tune, held = real[:cut], real[cut:]
        print(f"{len(real)} {kind} / {len({t.target for t in real})} targets"
              f" -- tune on {len(tune)}, hold out {len(held)}\n", flush=True)
        tune_score = lambda st, d: rate(db, st, tune, args.mycall, cache, d)
        exam_score = lambda st, d: rate(db, st, held, args.mycall, cache, d)
        exam_trials, exam_name = held, "held-out instants"

    t0 = time.time()
    best, spent = search(tune_score, args.iters, rng)
    print(f"\ntried {spent} strategies in {time.time()-t0:.0f}s", flush=True)

    print(f"\n=== EXAM: {exam_name} ({len(exam_trials)}), steady world ===")
    rows, reaches = {}, {}
    for name, s_ in (("direct only", DIRECT_ONLY), ("hand-written", HAND),
                     ("searched", best)):
        secs, reach = exam_score(s_, 0.0)
        rows[name], reaches[name] = secs, reach
        print(f"  {name:13s} {secs:6.0f}s   reach {reach:5.1%}   {s_.short()}")

    print(f"\n=== THE STRATEGY IT WROTE ===\n{best}")

    print("\n=== SENSITIVITY: stations behaving less predictably ===")
    print("  dither   searched          hand-written")
    a_ = sensitivity(exam_score, best)
    b_ = sensitivity(exam_score, HAND)
    for (d, s1, r1), (_d, s2, r2) in zip(a_, b_):
        flag = "" if r1 >= r2 else "   <-- loses its edge here"
        print(f"   {d:.2f}   {s1:6.0f}s {r1:5.1%}     "
              f"{s2:6.0f}s {r2:5.1%}{flag}")

    d_reach = reaches["searched"] - reaches["hand-written"]
    d_secs = rows["hand-written"] - rows["searched"]
    shaken_better = a_[-1][2] >= b_[-1][2]
    if d_reach > 0.001 and shaken_better:
        print(f"\n  reaches {d_reach*100:+.1f} points more of the exam it "
              f"never saw, and keeps the lead with the stations shaken "
              f"-- the gain is in what it DOES.")
    elif d_reach > 0.001:
        print(f"\n  reaches {d_reach*100:+.1f} points more, but the lead "
              f"goes away once stations stop behaving predictably: it "
              f"leans on particular stations, not on tactics.")
    elif abs(d_reach) <= 0.001 and d_secs > 1:
        print(f"\n  same reach, {d_secs:+.0f}s faster -- a real but "
              f"smaller win.")
    else:
        print(f"\n  NO GAIN SURVIVED the exam ({d_reach*100:+.1f} points, "
              f"{d_secs:+.0f}s). Fitted noise; hand-written stands.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
