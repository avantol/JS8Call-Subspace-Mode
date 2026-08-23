"""tribblenet.py — the TribbleNet routing engine.

PROBLEM STATEMENT (operator, 2026-08-21, verbatim intent):
  Find the most likely route to any STATION or GRID. Use who hears me,
  sourced from my radio or PSKR. Factor in stations I hear, and all
  PSKR "A hears B" info. Favour paths that go more in the desired
  direction; short detours might be OK. Work the problem looking
  OUTBOUND from me, and also INBOUND from the target station or its
  neighbours. Determine the most time-efficient strategy and try in
  shortest-time order. Start with an HB request if actual on-air radio
  data is stale or sparse.

Written to be portable to C++: no exotic Python, all state explicit,
every rule named and justified. This is the reference implementation of
what SpotMapWindow's mesh is FOR.

=========================== DIRECTION ==============================
The single most repeated defect in this subsystem is confusing which
end of a path a callsign names (five separate bugs on 2026-08-21). So
the vocabulary is fixed here and used nowhere loosely:

    an edge  A hears B   means  B's signal ARRIVES AT A.
    therefore             B can DELIVER to A.

  DELIVERY   ME -> ... -> T requires, at each step, that the NEXT
             station hears the PREVIOUS one.
  RETURN     T -> ... -> ME is the same relation walked from T. It is
             NOT the reverse of the delivery path: radio paths are
             routinely one-way (KI4HDU heard us at -16 while we heard
             nothing from it for 30 min).

A CONTACT needs both. A one-way delivery still has value (traffic
arrives; nobody can confirm it), and the planner says which it found.

========================= EVIDENCE KINDS ===========================
Every edge carries provenance (SpotMapWindow HeardEdge::source):
  radio    our own receiver decoded it            FIRST-HAND
  hearing  a HEARING?/QUERY CALL reply, possibly
           relayed to us                          RF, THIRD-PARTY
  mqtt     PSKReporter                            INTERNET

All three are used, per the problem statement. They are WEIGHTED, not
filtered: internet evidence is real evidence about the ionosphere, but
it says nothing about whether the pair is workable when the internet is
gone (#159), and a station that only exists in PSKR has never been
proven to key up for us.

========================== TIME MODEL ==============================
From ChunkedArq.h and measured on air (Normal, P=15 s):
  direct call + 1-frame reply      ~70 s
  each RELAY hop adds              ~60 s per direction (MEASURED)
  broadcast sweep (QUERY CALL)     ~97 s, evaluates the whole pool
  HB request + answers             ~90 s, and REPOPULATES the mesh
Ordering is by expected time, cheapest first, because probes are
strictly sequential on a half-duplex channel.
"""
from __future__ import annotations

import collections
import math
import time
from dataclasses import dataclass, field

import grid as G
import forwarders
import history
from callsign import base, is_routable, same

# ---- time constants (seconds), see TIME MODEL above ---------------
T_DIRECT = 70.0
T_HOP = 60.0          # one relay hop, one direction -- MEASURED
                      # 2026-08-22 on WM8Q>KJ7VWV>KB7ITU>KL7UT:
                      # forwards at +59 s, +60 s and the reply at
                      # +60 s after that, dead regular. The old 45
                      # was a guess and ran the clock out early.
T_SWEEP = 97.0
T_HB = 90.0

# ---- evidence weighting -------------------------------------------
# Multiplies a hop's nominal time to express RISK, not duration: a leg
# we have only ever seen on the internet is likelier to fail, and a
# failed leg costs the whole attempt. Ordering by (time x risk) is the
# adjacent-exchange argument with p folded in.
RISK = {"radio": 1.0, "hearing": 1.3, "mqtt": 1.8, "": 1.8,
        # "we hear them, so they can probably hear us" -- a prior, not
        # an observation. Ranked below every real edge on purpose.
        "reciprocal": 2.4}

# A hop that moves AWAY from the target is allowed but pays for it.
# "Short detours might be OK" (operator): a detour is charged in
# proportion to the ground it gives up, so a 100 km sidestep is nearly
# free while a 1500 km backtrack is not.
DETOUR_KM_FREE = 250.0
DETOUR_PENALTY_PER_KM = 1.0 / 900.0   # +1.0 risk per 900 km lost

# Radio data is "sparse" below this many DISTINCT radio-sourced edges
# in the window, or "stale" if the freshest is older than this.
SPARSE_RADIO_EDGES = 8
STALE_RADIO_SECS = 20 * 60


@dataclass
class Hop:
    frm: str
    to: str
    source: str
    age_s: float
    snr: int


@dataclass
class Route:
    path: list          # [ME, R1, ..., T]
    hops: list          # list[Hop], len == len(path)-1
    seconds: float      # expected air time for this direction
    risk: float         # product of per-hop risk multipliers
    direction: str      # "delivery" | "return"

    @property
    def relays(self) -> list:
        return self.path[1:-1]

    def as_command(self, literal_target: str | None = None) -> str:
        """The exact thing to put on the air. JS8 relay grammar is
        HOP1>HOP2>DEST payload; a direct call has no chain."""
        dest = literal_target or self.path[-1]
        chain = self.path[1:-1] + [dest]
        return ">".join(chain) + " SNR?"


class TribbleNet:
    """The live mesh as a routing graph.

    Built ONCE per plan from a LiveMap snapshot, so every query below
    sees a consistent picture (the map mutates continuously).
    """

    def __init__(self, lm, within_s: float = 3600.0):
        self.lm = lm
        self.within_s = within_s
        self.me = base(lm.my_call)
        self.my_grid = lm.my_grid
        # deliver[u] = {v: Hop}  -- u can DELIVER to v, because v hears u
        self.deliver: dict = collections.defaultdict(dict)
        self.skipped_unroutable = 0
        # One clock for the whole plan, so a forwarding record
        # cannot expire midway through a single search.
        self.now_epoch = time.time()
        self._build()

    # ---------------- construction ---------------------------------

    def _add(self, hearer: str, heard: str, source: str, age: float,
             snr: int) -> None:
        a, b = base(hearer), base(heard)
        if not a or not b or a == b:
            return
        # This graph exists to ROUTE, so only stations that can carry
        # traffic belong in it. SWL and freeband IDs, pirates, and
        # receive-only skimmer nodes all appear in the PSKR feed and
        # none of them can pass a message along (operator's call,
        # 2026-08-22). Dropped here, at the one place every edge enters,
        # rather than filtered again at each consumer.
        #
        # This does NOT say they are worthless: an SWL hearing a station
        # is real evidence about that station's propagation. Evidence
        # lives in LiveMap; routing lives here.
        if not is_routable(a) or not is_routable(b):
            self.skipped_unroutable += 1
            return
        if not (0 <= age <= self.within_s):
            return
        # b delivers to a. Freshest evidence for a pair wins, but a
        # RADIO edge is never replaced by an internet one -- provenance
        # outranks recency, because the risk weighting depends on it.
        cur = self.deliver[b].get(a)
        if cur is not None:
            better_src = RISK.get(source, 1.8) < RISK.get(cur.source, 1.8)
            if not better_src and age >= cur.age_s:
                return
            if RISK.get(source, 1.8) > RISK.get(cur.source, 1.8):
                return
        self.deliver[b][a] = Hop(b, a, source or "mqtt", age, snr)

    def _seed_reciprocity(self) -> None:
        """Stations WE HEAR become UNPROVEN first hops.

        The mesh only knows a station hears us once it reports us
        (PSKR) or answers us on air. With neither, deliver[ME] is empty
        and the search cannot leave home -- which is how a planner ends
        up saying "no route" while a perfectly good relay sits there
        being copied at -9.

        HF paths are usually near-reciprocal, so "we hear X" is real
        evidence that X can probably hear us -- evidence, not proof.
        It is added at RISK_UNPROVEN so a PROVEN first hop always wins
        when one exists, and the plan labels it honestly. This is the
        operator's "try anyway": unknown is not the same as
        contradicted (2026-08-21).

        THIRD PARTIES GET THE SAME RULE (operator, 2026-08-22: "don't
        neglect mix/match source info"). This used to run for ME alone,
        so a relay was only ever considered when the TARGET was known to
        hear it -- and "R hears the target" was thrown away. Reaching
        KK4QIG that cost a real route: the forward pairing found nothing
        and I went hunting two-hop chains, while pairing the other way
        gave six one-hop candidates including the one Andy spotted by
        eye. The inference was applied to us and withheld from everybody
        else, for no reason.

        Snapshot first: reciprocals must not breed reciprocals.
        """
        snapshot = [(u, v, h)
                    for u, vs in self.deliver.items()
                    for v, h in vs.items()]
        for u, v, h in snapshot:
            # deliver[u][v] says v hears u. The guess is the mirror:
            # u probably hears v, so v could deliver to u.
            if u in self.deliver.get(v, {}):
                continue          # real evidence already exists
            self.deliver[v][u] = Hop(v, u, "reciprocal", h.age_s, h.snr)

    def _build(self) -> None:
        # 1. The hearing store: every "A hears B" with provenance.
        for h in self.lm.hearing:
            a = h.get("CALL") or ""
            for e in h.get("HEARS") or []:
                self._add(a, e.get("CALL") or "",
                          (e.get("SOURCE") or "mqtt").lower(),
                          float(e.get("AGE_S", -1)),
                          int(e.get("SNR", -99)))
        # 2. (nothing) -- the reports_me spot pass is GONE.
        #
        # It claimed to carry "one fact the hearing store cannot state".
        # It could not: measured against the live map, it produced 47
        # "hears me" edges and the store already had all 47, ZERO
        # unique. What it did add was FALSE PROVENANCE. It tagged each
        # hop from the SPOT's pskr flag -- a per-station derived value
        # -- instead of the edge's own source, and got 45 of 47 wrong,
        # calling internet-only edges "radio".
        #
        # That is not cosmetic. _add() lets provenance outrank recency,
        # so running after step 1 meant systematically OVERWRITING
        # correct mqtt edges (risk 1.8) with fake radio ones (risk
        # 1.0) -- on the outbound first hop, the leg every route
        # depends on. Every plan was overconfident about exactly the
        # thing it should be most careful with.
        #
        # Same lesson as the heard_by edge before it: a second pass
        # that "restates" a store will disagree with it, and the
        # disagreement is the bug.
        # 3. Reciprocity, LAST so it can never displace real evidence.
        self._seed_reciprocity()

    # ---------------- geometry -------------------------------------

    def grid_of(self, call: str) -> str:
        return self.lm.grid_of(call)

    def _km(self, a: str, b: str):
        if not (a and b and G.valid(a) and G.valid(b)):
            return None
        try:
            return G.distance_km(a, b)
        except ValueError:
            return None

    def km_to(self, call: str, target_grid: str):
        return self._km(self.grid_of(call), target_grid)

    # ---------------- the search -----------------------------------

    def _dijkstra(self, start: str, target_grid: str,
                  target_call: str = "") -> dict:
        """Least (time x risk) delivery cost from `start` to everything
        reachable, biased toward `target_grid`.

        Cost is TIME multiplied by accumulated RISK, plus a detour
        charge. Dijkstra is valid because every term is non-negative
        and independent of the path taken to reach a node.
        """
        start = base(start)
        done_relays = (history.delivered_relays(target_call, self.now_epoch)
                       if target_call else set())
        best = {start: (0.0, 0.0, [start], [])}   # cost, secs, path, hops
        seen: set = set()
        frontier = [(0.0, start)]
        while frontier:
            frontier.sort()
            cost, u = frontier.pop(0)
            if u in seen:
                continue
            seen.add(u)
            _c, secs, path, hops = best[u]
            du = self.km_to(u, target_grid) if target_grid else None
            for v, hop in self.deliver.get(u, {}).items():
                if v in seen:
                    continue
                risk = RISK.get(hop.source, 1.8)
                # Whether v will FORWARD matters more than how loud it
                # is: on 2026-08-22 seven of nine well-chosen relays
                # simply did not relay, and path evidence rated them all
                # alike. Only charged when v is an intermediate hop --
                # the TARGET is not being asked to relay anything.
                if v != target_call:
                    risk *= forwarders.risk(v, self.now_epoch)
                    # A relay that has ALREADY put our traffic into
                    # this target teaches us nothing by doing it
                    # again -- the target is the variable, not the
                    # hop. Heavily penalised rather than removed, so
                    # it stays available if nothing else exists.
                    if v in done_relays:
                        risk *= 4.0
                # Direction: charge for ground GIVEN UP, free inside
                # DETOUR_KM_FREE. Unknown geometry is charged nothing --
                # missing grids must not silently rule a relay out.
                dv = self.km_to(v, target_grid) if target_grid else None
                if du is not None and dv is not None:
                    lost = max(0.0, dv - du) - DETOUR_KM_FREE
                    if lost > 0:
                        risk += lost * DETOUR_PENALTY_PER_KM
                step_secs = T_DIRECT if u == start else T_HOP
                ncost = cost + step_secs * risk
                if v not in best or ncost < best[v][0]:
                    best[v] = (ncost, secs + step_secs, path + [v],
                               hops + [hop])
                    frontier.append((ncost, v))
        return best

    def routes_to(self, target: str, target_grid: str = "",
                  max_hops: int = 3) -> tuple:
        """(delivery, return_) best Routes, either may be None.

        OUTBOUND is a search from ME: who can our traffic reach, and
        how fast. INBOUND is the same search run from the TARGET,
        because the answer has to travel T -> ... -> ME and radio paths
        are not symmetric. Running both is the operator's
        "outbound from me, and also inbound from the target".
        """
        t = base(target)
        tg = target_grid or self.grid_of(t)
        out = self._dijkstra(self.me, tg, t)
        back = self._dijkstra(t, self.my_grid, self.me)

        def mk(tbl, node, direction):
            e = tbl.get(node)
            if not e or len(e[2]) - 1 > max_hops:
                return None
            cost, secs, path, hops = e
            risk = 1.0
            for h in hops:
                risk *= RISK.get(h.source, 1.8)
            return Route(path, hops, secs, risk, direction)

        return mk(out, t, "delivery"), mk(back, self.me, "return")

    # ---------------- target resolution ----------------------------

    def stations_near_grid(self, target_grid: str, limit: int = 8,
                           radius_km: float = 400.0) -> list:
        """Stations in or near a GRID, nearest first.

        A grid is not a station: to work a square you work someone IN
        it. Everything the mesh knows about is a candidate, including
        receive-only monitors -- they cannot answer, but they prove the
        square is reachable, which is what a grid request often means.
        """
        out = []
        for call in self.lm.grids:
            d = self.km_to(call, target_grid)
            if d is not None and d <= radius_km:
                out.append((d, base(call)))
        out.sort()
        seen, res = set(), []
        for d, c in out:
            if c in seen:
                continue
            seen.add(c)
            res.append((c, d))
            if len(res) >= limit:
                break
        return res

    # ---------------- band-state gate ------------------------------

    def radio_is_thin(self) -> tuple:
        """(thin, why) -- is on-air data stale or sparse RIGHT NOW?

        The operator's rule: open with an HB request when it is. An HB
        costs ~90 s and its answers are RADIO-sourced first-hop proof,
        which is exactly the evidence a PSKR-heavy mesh lacks.
        """
        # The sharpest case first: if NOTHING is known to hear us, no
        # outbound route can exist at all and every relay below is a
        # guess. An HB is the direct cure -- its answers are exactly
        # "X hears you, at N dB", radio-sourced.
        proven_out = [h for h in self.deliver.get(self.me, {}).values()
                      if h.source != "reciprocal"]
        if not proven_out:
            return True, ("nothing in the mesh is known to hear US -- "
                          "no outbound first hop is proven")
        fresh = [h for u in self.deliver.values() for h in u.values()
                 if h.source == "radio"]
        if not fresh:
            return True, "no radio-sourced edges at all in the window"
        newest = min(h.age_s for h in fresh)
        if len(fresh) < SPARSE_RADIO_EDGES:
            return True, (f"only {len(fresh)} radio-sourced edge(s); the "
                          f"mesh is essentially all internet")
        if newest > STALE_RADIO_SECS:
            return True, (f"freshest radio evidence is "
                          f"{newest / 60:.0f} min old")
        return False, (f"{len(fresh)} radio edges, freshest "
                       f"{newest / 60:.0f} min -- on-air data is current")
