"""decide.py — WORK OUT THE NEXT MESSAGE, RATHER THAN SEARCH FOR A PROGRAM.

Andy, 2026-08-23, on the program search: "this looks like a classic
logic problem, how is it best solved?" -- and then, having heard the
proposal: "try your proposal, let's see if we get any kind of
convergence initially before spending too much time on a flawed
approach."

WHY NOT KEEP SEARCHING PROGRAMS. The search in `search.py` writes
strategies as a list of `WHEN ... DO ...` stages and mutates them. It
spent an evening finding relentless hammering, stages that never fire,
and the same message sent three times a cycle. It has to rediscover
"call them when you can hear them" as a pattern over conditions I
happened to supply, instead of deriving it from the fact that a call
only works when they are audible. It earned its keep as a critic -- it
exposed five modelling defects, a leak, and a selector that did nothing
-- but it is a poor optimiser.

WHAT THE PROBLEM IS. At each step we choose one message. Some try for
contact; others buy information that changes who we would aim at next.
We cannot see the state -- whether the target is on the air, who hears
whom -- only infer it. So every transmission is both an attempt and an
experiment, and what we minimise is expected time to a reply.

WHY IT IS COMPUTABLE. The unknowns are few and nearly binary: for each
of ~10 candidates, does it hear the target, do we hear it, will it
forward. The messages are five. The horizon is six to eight
transmissions. That is small enough to work out the decision directly.

    ATTEMPTS are ordered by success-chance divided by time. For
    independent attempts tried until one works, that ordering is
    provably optimal -- swapping any adjacent pair makes the expected
    time worse (the argument is in the original js8reach plan).

    BUT REPEATING AN ATTEMPT IS NOT A SECOND ATTEMPT. Whether a station
    answers is fixed for the length of a run -- its autoreply setting,
    whether anyone is sitting there -- so calling again is the same
    chance re-run, not a fresh one. The only thing a repeat buys is the
    chance the station ARRIVED since we last tried, which over one 68
    second call is about 2%. Treating repeats as independent multiplied
    a cheap attempt's worth by however many times it would fit in the
    budget, and relays never got a look in: on WA4FJQ the best relay was
    twice as likely to work as a direct call and was still rejected,
    because three direct calls fit in the same 218 seconds. Andy put it
    plainly: "if nobody answers, you move on."

    PROBES are the only reason that is not the whole answer. A probe
    cannot make contact, so its worth is entirely in how much it
    improves the attempts that follow. We price it that way: run the
    attempt ordering as it stands, then again as it would be if the
    probe answered, and take the difference.

THE EXCEPTION TO "ONE MESSAGE, ONE STATION" (Andy): @ALLCALL QUERY CALL
is aimed at everybody at once. Measured on real history, it tells us
who can reach the target 29% of the time, against 0-4% for the same
question aimed at one chosen station -- for the identical 98 seconds.
That is the whole reason the probe branch exists.

REVERSE-PATH SEARCH (Andy: "don't neglect the reverse-path search").
Candidates are found by walking BACKWARDS from the target: who does the
target hear, then who do those stations hear, outward towards us.
Delivery runs the other way -- to hand a message to somebody, they have
to hear the one before them -- so the walk starts at the destination.
For a GRID target with no callsign, the walk seeds from stations known
to be in that square instead.

GEOGRAPHIC DIRECTION (Andy: "keep geograhic direction toward the target
a generally favourable choice"). Where the target's position is known,
a candidate is preferred when it lies toward the target rather than
away: same-side stations share the path we need. This is a preference,
not a rule -- HF skip means distance never rules anything out -- so it
scales a probability rather than filtering a list.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import actions as A          # noqa: E402
import grid as _g            # noqa: E402
from callsign import base    # noqa: E402


# Costs, measured (actions.py). Kept here so the decision rule and the
# replay harness cannot drift apart.
T_SNR = A.rtt(1, 1)          #  68s  at the target: any reply is contact
T_GRID = A.rtt(1, 2)         #  82s  where is that station?
T_SHOUT = A.rtt(2, 2)        #  98s  @ALLCALL: who can reach the target?
T_HEARING = A.rtt(1, 4)      # 112s  what is that station hearing?
T_RELAY = [A.rtt(2, 2, h) for h in range(5)]   # index by hops


# WHEN TO STOP WAITING, measured, per query kind. From 515 answered
# probes in the corpus plus tonight's live relays: an answer that is
# coming has usually landed by p50; at p80 the odds are 4-to-1 against;
# past p95 it is over. All seconds from OUR TX-END. Relays add the
# forward checkpoint: KD7WPQ and KO4JJW both keyed our traffic 58 s
# after TX-end, twice each side of midnight.
#
# These exist so no wait is ever hand-picked again (Andy, 2026-08-25:
# "does the algorithm state wait times?" -- it does now).
# RE-DERIVED from the fine-grained distribution (Andy, 2026-08-26:
# "you'll probably see a large majority of immediate replies"). He was
# right -- it is BIMODAL: 61% of all eventual SNR? answers land within
# 45 s (the autoreply, one period + one frame), then a thin tail of
# humans and re-decodes. Waiting 45 -> 102 s buys 15 points; idling for
# the tail is wasted air-silence. So ESCALATE means "send the next
# move now" -- the tail still gets decoded and attributed passively
# while we act, which is what abandon_s is for: stop ATTRIBUTING, not
# stop listening.
# WAITS ARE COMPUTED FROM THE EXPECTED REPLY, not tabled. The reply to
# a query has a knowable shape: a turnaround period, then N frames of
# 15 s each. N per kind:
#   snr / grid    1 frame -- the answer packs (98.4% measured bare)
#   ask_call      2 -- "YES +nn (age)": the age rides as text
#   shout         2 -- same shape, everyone answers in parallel
#   hearing       1 + the LIST as text: 4 calls ~ 3 text frames, so 4
# check  = turnaround + reply airtime (the earliest a complete answer
#          can exist); escalate = one period of grace past it; abandon
#          = 240 s flat, the human tail, attribution only.
#
# These are CEILINGS FOR SILENCE. Any COMPLETE reply -- the last frame
# carries the end-of-message bit -- ends the wait the instant it lands
# (Andy, 2026-08-26: "we short-cut the max wait in all cases whenever
# we get an end-of-msg indication, correct?" -- correct, the watchers
# act on the assembled line, and the marks only cap failure).
# THE PERIOD IS A PARAMETER, NOT A CONSTANT. Policy (Andy, 2026-08-26):
# every FIRST message to a station goes at Normal speed -- it is the
# one submode everything decodes, and a first contact must not gamble
# on the far end's settings. Higher speeds may come later for the
# first hop once a station is known, so every delay below is derived
# from `period` rather than from the number 15.
#
# The shape (matches the app's own reply deadline, ChunkedArq.h):
#   reply complete ~ period * (1 + reply_frames) after our TX-end.
#
#   There is NO turnaround period. The far end decodes our signal at
#   ~12.6 s -- BEFORE our own period ends -- and keys at the very
#   boundary our TX ends on (AC7WY, live, 2026-08-26: our frame ended
#   :55:00, its reply occupied :55:00-:15, decoded here +11 s after
#   our TX-end). The old "+2 periods" came from ALL.TXT, whose TX
#   stamps are FLOORED to the boundary and written pre-key -- a
#   constant one-period inflation dressed as a turnaround
#   (mainwindow.cpp:11228, addSecs(-(sec % TRperiod))). The extra
#   period in `escalate` covers a far end that misses its first
#   boundary; the relay forward checkpoint is 3 periods (the "58 s"
#   carried the same +15 bias).
PERIOD_NORMAL = 15.0

REPLY_FRAMES = {"snr": 1, "grid": 1, "ask_call": 2, "shout": 2,
                "hearing": 4, "relay": 1,
                # heartbeat acks key on OUR TX-end boundary, single
                # frame, all together: the verdict exists one period
                # after TX-end (11 acks in the first window, measured
                # 2026-08-25 02:29Z; and the operator caught me hand-
                # sleeping 150 s for it anyway).
                "hb": 1}


def reply_frames_for(call: str) -> int:
    """Frames the packed reply `CALL: US SNR +nn` needs -- knowable
    from the callsign alone. Standard calls (up to two prefix chars, a
    digit, up to three letters, /P or /M via the flag) pack into one
    frame; anything odder rides as text and spills into a second."""
    import re
    c = (call or "").upper()
    if re.fullmatch(r"[A-Z0-9]{1,2}[0-9][A-Z]{1,3}(?:/[PM])?", c):
        return 1
    return 2


def waits_for(kind: str, reply_from: str = "", hops: int = 0,
              period: float = PERIOD_NORMAL) -> tuple:
    """(check, verdict, abandon) seconds after our TX-end -- gating on
    the answer having STARTED, not on it being complete.

    A verdict means BUSY OR DISABLED -- the station (or, for a group
    query, every station) had its slot and did not key. The remedy is
    a fresh attempt from the top later, never a longer wait now.

    RX.ACTIVITY delivers every frame the moment it decodes
    (processRxActivity.cpp:25), and every responder keys at OUR TX-end
    boundary, so the FIRST FRAME of any reply -- however long the whole
    reply is -- is due one period after TX-end. That makes the silence
    verdict UNIFORM: one period, plus decode margin, plus one more
    period only where the app documents a late slot (the HB 25%
    interval switch). A HEARING? list no longer buys its sender 78 s
    of silence; it gets the same ~21 s as everything else, and the
    long wait runs only once a reply is provably inbound
    (completion_secs below).
    """
    check = period + 3               # first frame of the answer due
    verdict = period + 6             # nothing started: BUSY OR DISABLED
    if kind == "hb":
        # One extra slot -- NOT an RNG: the ack reply path enqueues
        # immediately but at PriorityLow+1 (mainwindow.cpp:6700), so
        # any hotter traffic in the responder's queue displaces it by
        # a slot. (The 25% addSecs(15) at mainwindow.cpp:5825 is the
        # OWN-heartbeat scheduler, a different path.)
        verdict = 2 * period + 6
    abandon = 20 * period if kind == "shout" else 16 * period
    return (check, verdict, abandon)


def completion_secs(kind: str, reply_from: str = "",
                    period: float = PERIOD_NORMAL) -> float:
    """Once a reply has STARTED, how long until it should be complete
    and assembled: its own airtime plus margin."""
    frames = REPLY_FRAMES.get(kind, 2)
    frames += reply_frames_for(reply_from) - 1 if reply_from else 0
    return period * frames + 6


class Move:
    """One message, ready to send."""

    def __init__(self, kind, cost, via=None, chain=None, why="",
                 factors=None, reply_from=""):
        self.kind = kind          # snr | shout | hearing | grid | relay
        self.cost = cost
        self.via = via
        self.chain = chain or []
        self.why = why
        # [explain] Every quantity that went into choosing this message,
        # each as (name, raw value, 0-10). Andy, 2026-08-24: "what would
        # really help would be an explanation of the factors that caused
        # the decision to call a specific station... so i can correlate
        # the logic with the result." A decision that cannot be read is
        # a decision that cannot be argued with, and every wrong number
        # found tonight was found by someone reading one.
        self.factors = factors or []
        self.waits = waits_for(kind, reply_from, len(self.chain))

    def wire(self, me, target):
        if self.kind == "snr":
            return f"{me}: {target} SNR?"
        if self.kind == "shout":
            return f"{me}: @ALLCALL QUERY CALL {target}?"
        if self.kind == "hearing":
            return f"{me}: {self.via} HEARING?"
        if self.kind == "grid":
            return f"{me}: {self.via} GRID?"
        if self.kind == "relay":
            return f"{me}: {'>'.join(self.chain)}>{target} SNR?"
        return self.kind

    def __repr__(self):
        return f"<{self.kind} {self.via or ''} {self.cost:.0f}s>"


def explain(mv, target: str, me: str = "WM8Q") -> str:
    """The decision, factor by factor, for a human to check."""
    c, e, a = mv.waits
    lines = [f"  SEND   {mv.wire(me, target)}",
             f"  COST   {mv.cost:.0f} s   "
             f"check +{c}s / escalate +{e}s / abandon +{a}s"
             + ("   (the +58s forward is the checkpoint)"
                if mv.kind == "relay" else "")]
    if mv.factors:
        width = max(len(n) for n, _r, _s in mv.factors)
        lines.append("")
        for name, raw, score in mv.factors:
            # A FULL BAR MUST NOT MEAN "NO IDEA". `toward()` returns 1.0
            # when a grid is unknown -- deliberately, so an unlocated
            # relay is not silently ruled out -- and that rendered
            # identically to "lies straight toward the target". Same for
            # a saved-seconds figure that pins at the top: the bar stops
            # discriminating exactly where the decision is closest.
            # Unknowns get "?" so they read as absent evidence, not as
            # strong evidence.
            if raw in ("unknown", "?", ""):
                bar = "?" * 10
            elif score >= 9.95:
                bar = "*" * 10 + "+"
            else:
                bar = "*" * int(round(score))
            lines.append(f"  {name:<{width}}  {score:4.1f}  "
                         f"{bar:<11}  {raw}")
    return "\n".join(lines)


def _bearing(a: str, b: str) -> float:
    """Degrees from a to b, both maidenhead."""
    la1, lo1 = _g.to_latlon(a)
    la2, lo2 = _g.to_latlon(b)
    p1, p2 = math.radians(la1), math.radians(la2)
    dl = math.radians(lo2 - lo1)
    y = math.sin(dl) * math.cos(p2)
    x = (math.cos(p1) * math.sin(p2)
         - math.sin(p1) * math.cos(p2) * math.cos(dl))
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def toward(my_grid: str, cand_grid: str, target_grid: str) -> float:
    """1.0 if the candidate lies straight toward the target from us,
    falling to about 0.45 if it lies the opposite way.

    A preference, never a filter: skip means the wrong-looking station
    sometimes has the only open path, and ruling it out would repeat
    the mistake that put four western stations at the top of a plan to
    reach Maine.
    """
    for gd in (my_grid, cand_grid, target_grid):
        if not (gd and _g.valid(gd)):
            return 1.0
    try:
        want = _bearing(my_grid, target_grid)
        got = _bearing(my_grid, cand_grid)
    except (ValueError, ZeroDivisionError):
        return 1.0
    off = abs((want - got + 180.0) % 360.0 - 180.0)      # 0..180
    return 0.45 + 0.55 * (1.0 + math.cos(math.radians(off))) / 2.0


# What a link is worth when somebody has just TOLD us it works. This is
# not a multiplier: an answered QUERY CALL is a first-hand dated report
# at age zero, which is the same thing the corpus measures at 0.28 for
# reports inside the hour. The old code multiplied whatever guess
# preceded it by 3.0, so an unobserved link at the 0.12 floor came out
# at 0.36 -- scored HIGHER than the best real evidence we have.
FRESH_LINK = 0.28


def best_routes(d, pool, max_hops=4, width=240) -> dict:
    """(see below) -- `pool` is now the set of stations WE CAN RAISE,
    which constrains the FIRST hop only; the walk itself ranges over the
    whole graph."""
    """Most-PROBABLE delivery route to the target for every candidate.

    Andy, 2026-08-23: "have you considered a Parallel Path Tree? the
    Dijkstra algorithm?" -- and it is the right tool. The chain builder
    it replaces was a breadth-first walk that kept the FEWEST-HOP route
    and, within a hop level, whichever one the database happened to
    return first. That is why the second-best relay looked 150 times
    worse than the best: it was not the genuine second-best route, it
    was an arbitrary one. Two strong hops beat one weak hop, and the
    breadth-first walk could never see it.

    Weighting each link with the negative log of its probability makes
    the sum along a path the negative log of the product, so ordinary
    shortest-path gives the most-probable route. Run backwards from the
    target -- delivery needs the RECEIVER to hear the sender, so the
    walk starts at the destination -- it yields the best route to every
    candidate at once: the parallel tree, not a single chain.

    Each hop carries BOTH directions, because the reply retraces the
    same stations: the link out, the link home, whether that station is
    awake, and whether it forwards at all.
    """
    import heapq
    T = d.target
    INF = float("inf")
    # distance to a station = -log(chance the rest of the route works)
    dist = {T: -math.log(max(1e-9, d.ans(T)))}
    route = {T: []}
    seen = set()
    heap = [(dist[T], T)]
    while heap:
        cost, node = heapq.heappop(heap)
        if node in seen:
            continue
        seen.add(node)
        if len(route[node]) >= max_hops:
            continue
        # WALK THE SHORTLIST, NOT THE WHOLE GRAPH -- measured, against
        # my own argument. Widening this to every station reachable
        # backwards was defensible on its face (the shortlist is 2.3% of
        # the 2,744 stations within three levels) and it cost TWO POINTS
        # of reach, 6.48% -> 4.40% over 386 replayed instants.
        #
        # And not because of the multi-hop routes it exposed. The 2x2
        # settles that: wide/4-hop and wide/1-hop both score 4.40%,
        # narrow/4-hop and narrow/1-hop both score 6.48%. Hop count
        # changes NOTHING; width changes everything, for the worse.
        #
        # So the shortlist was never an arbitrary limit -- it is a
        # PRIOR, and a better one than this model's ranking. Its two
        # crude facts (observed hearing the target; geographically near
        # it) pick better one-hop relays than the decay curve, signal
        # margin and forwarding record do when handed a wider field.
        # That is worth more than the two points: it says the ranking
        # machinery currently adds less signal than two heuristics, and
        # a wider search only gives it more room to be wrong.
        for cand in pool:
            if cand in seen or cand == d.me:
                continue
            out = d.delivers_to(cand, node)   # node hears cand: cand -> node
            back = d.reverse(node, cand)   # and the reply comes back
            # DIRECTION IS A PRIOR, NOT A FACT. Geometry earns its
            # place as the only evidence for links nobody has observed
            # -- the code says so where _geo_link is defined. So once a
            # link IS observed, in either direction, the prior has done
            # its job and must step aside: AC7WY was paying a 2x
            # direction penalty on a REPORTED one-hop link to the
            # target, which is discounting a fact for looking
            # improbable (Andy pressed twice: "why did the algo not use
            # AC7WY?" -- this is why).
            ev = getattr(d.model, "has_link_evidence", None)
            observed = bool(ev and (ev(cand, node) or ev(node, cand)))
            direction = 1.0 if observed else d._toward(cand)
            live = d.copy(cand) * d.fwd(cand) * direction
            step = out * back * live
            if step <= 1e-9:
                continue
            nd = cost - math.log(step)
            if nd < dist.get(cand, INF):
                dist[cand] = nd
                route[cand] = [cand] + route[node]
                heapq.heappush(heap, (nd, cand))
        if len(seen) > width * 4:
            break
    out = {}
    reachable = set(pool)
    for call, r in route.items():
        if call == T or not r:
            continue
        # THE FIRST HOP MUST BE A STATION WE CAN ACTUALLY RAISE.
        # Widening the walk to the whole graph was right; dropping this
        # was not. The old fixed pool was doing two jobs at once --
        # limiting the search (wrong, it saw 2.3% of the graph) and
        # requiring a reachable first hop (right). Removing both let the
        # rule route through stations we have never heard, whose
        # "hears_us" is a modelled 0.60 rather than a fact, and every
        # such attempt fails on the air. Relay reach fell from 8.2% to
        # 2.6% -- level with not relaying at all.
        #
        # Intermediate hops are NOT restricted: we never talk to them
        # directly, so whether we can hear them is beside the point.
        if r[0] not in reachable:
            continue
        p = math.exp(-dist[call]) * d.hears_us(r[0])
        out[call] = (r, p)
    return out


class Decider:
    """Chooses the next message from what we currently believe."""

    def __init__(self, model, board, target, mycall, max_hops=4):
        # Beliefs are fixed for the length of one attempt -- they come
        # from history up to the horizon, not from the clock -- so each
        # one is worked out once. Without this the rule re-queried the
        # database for every candidate at every step and was far too
        # slow to compare against a replayed program.
        self._c: dict = {}
        self._routes = None
        self.model = model
        self.board = board
        self.target = base(target).upper()
        self.me = base(mycall).upper()
        self.max_hops = max_hops
        self.my_grid = model.grid_of(self.me) or ""
        self.t_grid = model.grid_of(self.target) or ""

    # ---- the pieces of a belief ---------------------------------------

    def _q(self, key, fn):
        got = self._c.get(key)
        if got is None:
            got = fn()
            self._c[key] = got
        return got

    def copy(self, c):
        return self._q(("copy", c), lambda: self.model.p_copy(c, 600.0).p)

    def fwd(self, c):
        return self._q(("fwd", c), lambda: self.model.p_fwd(c).p)

    def ans(self, c):
        return self._q(("ans", c), lambda: self.model.p_ans(c).p)

    def hears_us(self, c):
        return self._q(("hu", c), lambda: self.model.p_hears_us(c).p)

    def link(self, a, b):
        return self._q(("lk", a, b), lambda: self.model.p_link(a, b).p)

    def delivers_to(self, station, dest):
        """Chance `dest` can hear `station` -- the DELIVERY direction.

        The candidate pool is built from stations observed HEARING the
        target, because that is the evidence the logs carry. Delivery
        needs the opposite: the target must hear the relay. Scoring it
        with a plain forward lookup therefore returned the no-evidence
        floor for every candidate in the pool -- 0.12 across the board,
        which is why every relay in a dry run showed an identical
        `target hears X  1.2  *` bar and nothing could be told apart.
        (Andy spotted the uniformity, 2026-08-24.)

        Where the forward direction IS reported, use it. Otherwise we
        have evidence of the REVERSE, and reciprocity conditioned on
        how comparable the two stations are is a far better estimate
        than the floor: 0.42 against 0.12 on the first case checked.
        """
        fwd = self.link(dest, station)
        # Evidence EXISTENCE, not magnitude. The old gate (> 0.13)
        # meant a fresh but weak observation -- AC7WY hearing the
        # target at -17 computes to ~0.11 -- read as "no evidence",
        # so reciprocity was never applied and the route fell to the
        # floor. Andy's question ("why was AC7WY not considered?")
        # found it. Magnitude answers "how good"; only existence
        # answers "do we know anything".
        ev = getattr(self.model, "has_link_evidence", None)
        seen = (ev(station, dest) if ev
                else self.link(station, dest) > 0.13)
        rev = self.reverse(station, dest) if seen else 0.0
        # TAKE THE BETTER OF THE TWO, not simply the forward one.
        # Preferring any forward evidence over any reverse evidence
        # sounds right -- a direct observation of the direction we need
        # beats an inference about it -- but it discards the stronger
        # estimate when the observation is stale and the inference is
        # fresh. KV5R answered "+17, 2 hours" about AI5TS, which puts
        # the reverse at 0.47; an old forward edge scored 0.29, and the
        # 0.29 won. Both are estimates of the same quantity and both are
        # floors rather than ceilings, so the higher one carries more
        # evidence, not less.
        return max(fwd, rev)

    def reverse(self, a, b):
        """Chance b hears a, given a hears b."""
        fn = getattr(self.model, "p_reverse", None)
        if fn is None:
            return self.link(b, a)
        return self._q(("rv", a, b), lambda: fn(a, b).p)

    def gridof(self, c):
        return self._q(("g", c), lambda: self.model.grid_of(c) or "")

    def can_deliver_to(self, node: str) -> list:
        """Stations `node` can HEAR -- so each of them can hand a
        message to it. The backward walk's neighbour lookup, straight
        out of the edge table rather than a pre-selected pool."""
        got = self._c.get(("adj", node))
        if got is None:
            db = self.model.db
            horizon = getattr(self.model, "horizon", None)
            if horizon is None:
                rows = db.execute(
                    "SELECT heard FROM edges WHERE hearer=? "
                    "ORDER BY n DESC LIMIT 60", (node,))
            else:
                rows = db.execute(
                    "SELECT heard FROM edges WHERE hearer=? AND "
                    "last_when < ? ORDER BY n DESC LIMIT 60",
                    (node, horizon))
            got = [r["heard"] for r in rows]
            self._c[("adj", node)] = got
        return got

    def told_us_it_hears_target(self, call: str) -> None:
        """A QUERY CALL answer. It proves this station hears the TARGET
        -- which is the RETURN leg, not the delivery one: to hand the
        target a message, the TARGET must hear this station. So only
        that one direction is pinned to the fresh-evidence value; the
        other is left to be inferred as it always was.
        """
        self._c[("lk", call, self.target)] = FRESH_LINK
        self._routes = None          # the tree changes; rebuild it

    def p_direct(self) -> float:
        """Chance one SNR? at the target produces a reply."""
        return (self.copy(self.target) * self.hears_us(self.target)
                * self.ans(self.target))

    def p_relay(self, w, chain: list) -> float:
        """Chance a message down this chain reaches the target AND the
        answer gets back. Both legs, because the reply retraces the
        same stations and only about a fifth of links work both ways."""
        if not chain:
            return 0.0
        key = ("relay", tuple(chain))
        got = self._c.get(key)
        if got is not None:
            return got
        first = chain[0]
        p = self.hears_us(first)                     # we can reach them
        for i, station in enumerate(chain):
            p *= self.copy(station)                  # awake
            p *= self.fwd(station)                   # will forward
            nxt = chain[i + 1] if i + 1 < len(chain) else self.target
            p *= self.delivers_to(station, nxt)      # nxt hears them
            p *= self._toward(station)
        p *= self.ans(self.target)
        # THE WAY HOME IS NOT AN INDEPENDENT COIN. Given the outbound
        # link exists, the return one is conditioned on how comparable
        # the two stations are -- 55% between equals, 6% when one hears
        # fifty times what the other does. Multiplying two independent
        # 0.2s instead was 25x of the per-hop penalty on its own.
        prev = self.target                           # and the way home
        for station in reversed(chain):
            p *= self.reverse(prev, station)
            prev = station
        p *= self.copy(first)
        self._c[key] = p
        return p

    def _toward(self, call: str) -> float:
        return self._q(("tw", call),
                       lambda: toward(self.my_grid, self.gridof(call),
                                      self.t_grid))

    # ---- candidate attempts, best first --------------------------------

    # A station stays on the air about 45 minutes, so over a short
    # interval the chance one ARRIVES is small and roughly linear.
    SESSION_S = 2700.0

    def _stale(self, since_s: float) -> float:
        """Worth of repeating something already tried `since_s` ago:
        purely the chance the situation has changed underneath us."""
        return 1.0 - math.exp(-max(0.0, since_s) / self.SESSION_S)

    def attempts(self, w) -> list:
        """Every contact attempt available now, ordered by chance over
        cost. Each DISTINCT attempt appears once; one already made is
        worth only the chance things have moved since."""
        pd = self.p_direct()
        last = w.tried.get(("snr", self.target))
        if last is not None:
            # A REPEAT ONLY BUYS THE CHANCE THEY ARRIVED SINCE. And if
            # they were already on the air when we called and did not
            # answer, there is nothing to arrive -- they never left.
            # Andy, 2026-08-24: "I disagree with repeating the first
            # direct SNR", on a plan whose own `they are on the air` bar
            # read 9.5. Quite so: whether a station answers is fixed for
            # the run, so calling a station that was demonstrably
            # present and silent is asking the same question twice.
            absent = 1.0 - self.copy(self.target)
            pd *= self._stale(w.t - last) * max(0.05, absent)
        f = [("they are on the air", f"{self.copy(self.target):.2f}",
              10 * self.copy(self.target)),
             ("they can hear us", f"{self.hears_us(self.target):.2f}",
              10 * self.hears_us(self.target)),
             ("they answer when called", f"{self.ans(self.target):.2f}",
              10 * self.ans(self.target)),
             ("worth calling again yet",
              "first call" if last is None
              else f"{self._stale(w.t - last):.2f}",
              10.0 if last is None else 10 * self._stale(w.t - last))]
        out = [(pd / T_SNR,
                Move("snr", T_SNR, why="any reply from them is contact",
                     factors=f, reply_from=self.target))]
        if self._routes is None:
            self._routes = best_routes(self, self.board.pool, self.max_hops)
        for cand, (chain, p) in self._routes.items():
            last = w.tried.get(("relay", cand))
            if last is not None:
                p *= self._stale(w.t - last)
            if p <= 0.0:
                continue
            cost = T_RELAY[len(chain)]
            why = (f"{'>'.join(chain)} is the most likely route; "
                   f"{self._toward(cand):.2f} toward them")
            last = chain[-1]
            back = 1.0
            prev = self.target
            for st in reversed(chain):
                back *= self.reverse(prev, st)
                prev = st
            f = [("we can raise " + cand, f"{self.hears_us(cand):.2f}",
                  10 * self.hears_us(cand)),
                 (cand + " is on the air", f"{self.copy(cand):.2f}",
                  10 * self.copy(cand)),
                 (cand + " forwards when asked", f"{self.fwd(cand):.2f}",
                  10 * self.fwd(cand)),
                 ("target hears " + last,
                  f"{self.delivers_to(last, self.target):.2f}",
                  10 * self.delivers_to(last, self.target)),
                 ("the reply gets back", f"{back:.2f}", 10 * back),
                 ("target answers at all", f"{self.ans(self.target):.2f}",
                  10 * self.ans(self.target)),
                 ("toward the target",
                  f"{self._toward(cand):.2f}" if self.gridof(cand)
                  else "unknown",
                  10 * self._toward(cand)),
                 ("route length", f"{len(chain)} hop",
                  10.0 / len(chain))]
            out.append((p / cost, Move("relay", cost, via=cand,
                                       chain=chain, why=why, factors=f,
                                       reply_from=self.target)))
        out.sort(key=lambda x: -x[0])
        return out

    def expected_time(self, ranked: list) -> float:
        """Expected seconds if we simply work down this list until one
        of them answers."""
        total, alive = 0.0, 1.0
        for _idx, (p, mv) in enumerate(ranked):
            total += alive * mv.cost
            alive *= (1.0 - min(0.95, p * mv.cost))
        return total + alive * 600.0        # nothing left: a flat penalty

    # ---- probes, priced by what they would change ----------------------

    def probes(self, w, ranked) -> list:
        """Messages that cannot make contact, priced by how much they
        improve the attempts that follow."""
        now = self.expected_time(ranked)
        out = []

        # @ALLCALL QUERY CALL -- the one message aimed at everybody, and
        # ONCE per attempt. Andy, 2026-08-24: "don't repeat QUERY CALL at
        # all."
        #
        # The corpus does say a repeat finds new stations -- 159
        # consecutive pairs within an hour share only 46% of their
        # responders, and the second turns up about two the first
        # missed. But finding a station is not reaching the target, and
        # the honest reading of that same data is how VARIABLE the
        # answer is: 23 responders at 04:02, zero at 04:16, twelve at
        # 04:30. The zero was a channel swamped with heartbeat traffic,
        # not a quiet band. Asking again is a lottery on channel
        # conditions, and it is cheaper to stop and start over when the
        # band has actually moved.
        if ("shout", "") not in w.tried:
            q = 0.29        # measured on real relay instants
            gain = now - self._after_shout(w, ranked)
            # "seconds it would save" was CONSTANT -- 1870 to 2018
            # across every broadcast in every run, a 7% spread, so it
            # occupied a line and a bar while informing nothing (Andy
            # spotted it: "check that"). It should fall when we already
            # hold good routes, because a broadcast that improves
            # nothing saves nothing. Measured against the best route we
            # have rather than against an absolute.
            best = max((sc for sc, mv in ranked if mv.kind == "relay"),
                       default=0.0) * T_RELAY[1]
            headroom = max(0.0, 1.0 - min(1.0, best / 0.20))
            f = [("someone answers this", f"{q:.2f}", 10 * q),
                 ("how much better it could make things",
                  f"best route now {best:.3f}", 10 * headroom),
                 ("we know a route already",
                  "no" if not w.learned else f"{len(w.learned)} known",
                  0.0 if w.learned else 10.0)]
            out.append((q * gain * headroom - T_SHOUT,
                        Move("shout", T_SHOUT,
                             why="one message, every station that can "
                                 "reach them answers (29% on real data)",
                             factors=f)))

        # HEARING? at the station we know least about but can raise.
        cand = self._least_known(w)
        if cand:
            gain = now - self._after_hearing(w, ranked, cand)
            out.append((0.35 * gain - T_HEARING,
                        Move("hearing", T_HEARING, via=cand,
                             why=f"{cand} would name up to 4 stations "
                                 f"it is hearing")))

        # GRID? where a location is missing and would change the order.
        cand = self._unlocated(w)
        if cand:
            gain = now - self._after_grid(w, ranked, cand)
            out.append((0.5 * gain - T_GRID,
                        Move("grid", T_GRID, via=cand,
                             why=f"we do not know where {cand} is, so it "
                                 f"cannot be judged on direction")))
        return out

    def _after_shout(self, w, ranked) -> float:
        """If the broadcast answers, the best relay becomes one we KNOW
        can deliver -- the delivery leg stops being a guess."""
        if not ranked:
            return self.expected_time(ranked)
        boosted = []
        for p, mv in ranked:
            if mv.kind == "relay" and len(mv.chain) == 1:
                # The answer would pin ONE direction to fresh evidence.
                # Scale by how much that direction improves, rather than
                # by an invented factor.
                had = max(1e-6, self.link(mv.chain[0], self.target))
                boosted.append((min(0.95, p * (FRESH_LINK / had)), mv))
            else:
                boosted.append((p, mv))
        boosted.sort(key=lambda x: -x[0])
        return self.expected_time(boosted)

    def _after_hearing(self, w, ranked, cand) -> float:
        boosted = [(p * (2.0 if mv.via == cand else 1.0), mv)
                   for p, mv in ranked]
        boosted.sort(key=lambda x: -x[0])
        return self.expected_time(boosted)

    def _after_grid(self, w, ranked, cand) -> float:
        boosted = [(p * (1.4 if mv.via == cand else 1.0), mv)
                   for p, mv in ranked]
        boosted.sort(key=lambda x: -x[0])
        return self.expected_time(boosted)

    def _least_known(self, w):
        best, bp = None, 1e9
        for c in self.board.pool:
            if c in w.asked:
                continue
            p = self.link(c, self.target)
            if p < bp:
                best, bp = c, p
        return best

    def _unlocated(self, w):
        for c in self.board.pool[:12]:
            if c not in getattr(w, "asked_grid", ()) \
                    and not self.gridof(c):
                return c
        return None

    # ---- the decision --------------------------------------------------

    def choose(self, w) -> Move:
        """The next message.

        THERE IS NO TRADE BETWEEN CALLING AND RELAYING. Andy, 2026-08-23:
        "does it need a probability? if you can't reach them directly
        SNR?, you must relay." Quite so, and it removes the one number
        the corpus could not pin down. A direct call is the cheapest
        message we have and it needs nobody's cooperation, so it goes
        first. Once it has failed, relaying is not an alternative to be
        weighed against it -- it is the only thing left. What remains to
        be decided is which route, and that ranking is measured and
        monotone.

        The chance-over-cost index was comparing 68 seconds of calling
        against 218 seconds of relaying as though they were competing
        options. They are a sequence, not a choice.
        """
        # 1. Call them, unless we have just done so and nothing has
        #    changed since -- a repeat only catches somebody arriving.
        # ONCE. Andy, 2026-08-24: "no repeat direct SNR? to the target.
        # once is enough... it's going to be cheaper to fail fast, and
        # start over again" -- at the next request, with fresh data.
        #
        # Which is right for a reason worth writing down: whether a
        # station answers is fixed for the length of an attempt, so a
        # second call asks a question already answered. It can only pay
        # if they ARRIVED since, and the first plan this rule produced
        # re-called a station whose own bar said it was already on the
        # air. Half an hour of grinding costs more than stopping and
        # starting again when something has actually changed.
        if ("snr", self.target) not in w.tried:
            # Take the move built by attempts(), not a bare one -- it
            # carries the factor breakdown, and a decision that cannot
            # be read cannot be checked.
            for _sc, mv in self.attempts(w):
                if mv.kind == "snr":
                    return mv
            return Move("snr", T_SNR, why="cheapest message, needs nobody")

        # 2. Find out who can reach them, if we do not know yet. One
        #    message, every station that can reach them answers.
        ranked = self.attempts(w)
        for score, mv in self.probes(w, ranked):
            # A REAL MARGIN, not merely positive. The repeated broadcast
            # kept going out on scores of +3 and +14 seconds against a
            # 98-second cost -- a coin-flip margin on an arithmetic
            # estimate, for a message MEASURED to buy 0.0 points of
            # reach when repeated (2026-08-23: the repeat cost 1.3 extra
            # transmissions per attempt and gained nothing). Andy:
            # "subsequent QUERY CALL might be wasteful." Require it to
            # save at least twice what it costs before keying up again.
            if mv.kind == "shout" and score > 0:
                return mv

        # 3. Work down the routes, best first.
        for _score, mv in ranked:
            if mv.kind == "relay":
                return mv

        # 4. Nothing left to route through: buy information, or call
        #    again and hope they have come on.
        best_probe = None
        for score, mv in self.probes(w, ranked):
            if best_probe is None or score > best_probe[0]:
                best_probe = (score, mv)
        if best_probe and best_probe[0] > 0:
            return best_probe[1]
        return Move("snr", T_SNR, why="nothing else to try")
