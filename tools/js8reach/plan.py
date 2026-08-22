#!/usr/bin/env python3
"""plan.py — the time-ordered strategy for reaching a STATION or GRID.

    python3 plan.py AL0A
    python3 plan.py --grid FN43
    python3 plan.py K2AY --max-hops 2

Consumes TribbleNet (tribblenet.py) and emits the ordered list of
things to put on the air, cheapest expected time first, because probes
are strictly sequential on a half-duplex channel.

THE ORDER, and why each step earns its place:

 0. HB REQUEST, when on-air data is stale or sparse. ~90 s, and unlike
    every other action it IMPROVES THE MAP: the answers are
    radio-sourced first-hop proof, which is the one thing a
    PSKR-dominated mesh cannot supply. Operator's rule, and today's
    mesh was 1554 internet edges to 65 radio ones -- planning on that
    without refreshing is planning on the internet's opinion.

 1. DIRECT CALL, when the target hears us (or might). 70 s, the
    cheapest action that can produce a contact, and it needs no relay
    to be right about anything.

 2. RELAY, in ascending expected time. Each route is a real path
    through the mesh with per-hop provenance, not a guess.

 3. SWEEP (@ALLCALL QUERY CALL T?), when no route is known. 97 s and
    it evaluates EVERY station at once, so it dominates unicast
    probing whenever there are 2+ candidates -- and its answers are
    RF-sourced, so it works with the internet down.

DELIVERY vs RETURN is reported separately for every plan, because a
route that gets traffic there is not the same as one that can bring an
answer back, and today five stations in a row had the first without
the second.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import grid as G                     # noqa: E402
from callsign import base            # noqa: E402
import history                      # noqa: E402
from live import LiveMap             # noqa: E402
from tribblenet import (TribbleNet, T_DIRECT, T_HB,  # noqa: E402
                        T_SWEEP)


class Step:
    def __init__(self, text, seconds, why, kind="tx"):
        self.text = text
        self.seconds = seconds
        self.why = why if isinstance(why, list) else [why]
        self.kind = kind


def _fmt_route(r, tn) -> list:
    out = []
    for h in r.hops:
        out.append(f"{h.frm} -> {h.to}: {h.to} hears {h.frm} "
                   f"({h.source}, {h.age_s / 60:.0f} min"
                   + (f", {h.snr:+d} dB" if h.snr > -99 else "") + ")")
    return out


def plan(lm, target: str = "", target_grid: str = "",
         max_hops: int = 3) -> list:
    tn = TribbleNet(lm)
    steps: list = []

    # ---- 0. Is the on-air picture good enough to plan on? ----------
    thin, why = tn.radio_is_thin()
    if thin:
        steps.append(Step(
            "@HB HEARTBEAT " + (tn.my_grid[:4] or ""), T_HB,
            [f"on-air data is thin: {why}",
             "an HB is the only probe whose answers are RADIO-sourced "
             "proof that a station hears US -- the first hop of every "
             "route below",
             "it also repopulates the mesh, so the plan after it is "
             "better than the plan before it"], kind="hb"))

    # ---- target resolution: station, or a grid to be worked --------
    resolved = base(target) if target else ""
    if not resolved:
        if not target_grid:
            return steps
        near = tn.stations_near_grid(target_grid)
        if not near:
            steps.append(Step("(nothing to send)", 0.0,
                              [f"the mesh knows no station within "
                               f"400 km of {target_grid}",
                               "watch for one, or sweep the band"],
                              kind="wait"))
            return steps
        # A grid is worked THROUGH a station in it. Try them in order of
        # distance from the square, but plan each one properly.
        best = None
        for call, d in near:
            deliv, ret = tn.routes_to(call, target_grid, max_hops)
            if deliv and (best is None or deliv.seconds < best[1].seconds):
                best = (call, deliv, ret, d)
        if best is None:
            resolved = near[0][0]
            steps.append(Step(f"{resolved} SNR?", T_DIRECT,
                              [f"nearest station to {target_grid}: "
                               f"{resolved}, {near[0][1]:.0f} km from "
                               f"the square centre",
                               "no route known -- calling it directly "
                               "is the cheapest way to find out"]))
            return steps
        resolved = best[0]
        steps.append(Step(f"(target {target_grid} -> {resolved})", 0.0,
                          [f"{resolved} is {best[3]:.0f} km from the "
                           f"square and the fastest of "
                           f"{len(near)} candidate(s)"], kind="note"))

    tg = target_grid or tn.grid_of(resolved)
    deliv, ret = tn.routes_to(resolved, tg, max_hops)
    literal = lm.literal_call(resolved)

    # ---- 0b. REACHED BUT SILENT is its own answer -------------------
    # The planner used to know only "route" and "no route". A target we
    # can demonstrably reach and which never replies is neither, and it
    # was the most common outcome on 2026-08-21/22 (N9WCW, KG4UHM/6,
    # KG6NFJ, K2AY -- every one of them had a relay forward confirmed on
    # air). Recommending another relay there spends airtime testing a
    # leg already proven to work, and the four causes of the silence --
    # relay-off, unattended, autoreply disabled, or blocking us -- are
    # all invisible from here and none of them are fixed by trying a
    # different hop.
    import time as _time
    if history.reached_but_silent(resolved, _time.time()):
        via = ", ".join(sorted(
            history.delivered_relays(resolved, _time.time())))
        steps.append(Step(
            f"(stop calling {resolved} -- watch instead)", 0.0,
            [f"our traffic HAS reached {resolved}: forward confirmed on "
             f"air via {via}",
             "it has not answered. That is the station, not the path -- "
             "another relay re-tests a leg we already proved",
             "silence here is relay-off, unattended, autoreply disabled, "
             "or blocked; none are distinguishable from our end",
             "watch for it to transmit, or retry after the band shifts"],
            kind="note"))
        return steps

    # ---- 1. direct, when it is a real option -----------------------
    direct_known = resolved in tn.deliver.get(tn.me, {})
    if direct_known:
        h = tn.deliver[tn.me][resolved]
        steps.append(Step(
            f"{literal} SNR?", T_DIRECT,
            [f"{resolved} HEARS US: {h.source}, {h.age_s / 60:.0f} min "
             f"ago" + (f", {h.snr:+d} dB" if h.snr > -99 else ""),
             "cheapest action that can produce a contact"]))
    elif deliv is None:
        steps.append(Step(
            f"{literal} SNR?", T_DIRECT,
            ["no evidence it hears us and no route found -- but a "
             "direct call is 70 s and conditions change minute to "
             "minute",
             "silence here proves nothing; it is the cheapest way to "
             "be wrong"]))

    # ---- 2. relays, ascending time ---------------------------------
    if deliv and deliv.relays:
        steps.append(Step(
            deliv.as_command(literal), deliv.seconds,
            [f"DELIVERY route, {len(deliv.relays)} hop(s), "
             f"~{deliv.seconds:.0f} s, risk x{deliv.risk:.1f}"]
            + _fmt_route(deliv, tn)))

    if ret and ret.relays:
        same_path = deliv and list(reversed(ret.path)) == deliv.path
        steps.append(Step(
            "(return path)" if same_path else
            f"(watch for {ret.path[1]} relaying the answer)",
            0.0,
            [f"RETURN route exists: "
             + " -> ".join(ret.path) + f", ~{ret.seconds:.0f} s"]
            + _fmt_route(ret, tn), kind="note"))
    elif deliv:
        steps.append(Step("(no return path known)", 0.0,
                          ["traffic can REACH the target; nothing in "
                           "the mesh can carry an answer back",
                           "expect delivery without confirmation -- "
                           "five stations behaved exactly this way on "
                           "2026-08-21"], kind="note"))

    # ---- 3. sweep, when the mesh cannot answer the question --------
    if deliv is None:
        steps.append(Step(
            f"@ALLCALL QUERY CALL {resolved}?", T_SWEEP,
            ["no delivery route in the mesh",
             "one transmission asks EVERY station in reach whether it "
             "hears the target, and the answers are RF-sourced",
             "dominates unicast probing for 2+ candidates"],
            kind="sweep"))

    steps.sort(key=lambda s: (s.kind == "note", s.seconds))
    return steps


def _main() -> int:
    import argparse
    ap = argparse.ArgumentParser(
        description="Time-ordered plan to reach a station or grid")
    ap.add_argument("target", nargs="?", default="",
                    help="callsign (omit when using --grid)")
    ap.add_argument("--grid", default="", help="target Maidenhead grid")
    ap.add_argument("--max-hops", type=int, default=3)
    a = ap.parse_args()
    if not a.target and not a.grid:
        ap.error("give a callsign or --grid")
    if a.grid and not G.valid(a.grid):
        ap.error(f"not a valid grid: {a.grid}")
    lm = LiveMap.fetch()
    tn = TribbleNet(lm)
    edges = sum(len(v) for v in tn.deliver.values())
    print(f"TribbleNet: {edges} edges, {len(tn.deliver)} nodes, "
          f"band {lm.band}, me {lm.my_call}/{lm.my_grid}")
    print()
    steps = plan(lm, a.target, a.grid, a.max_hops)
    if not steps:
        print("no plan")
        return 0
    total = 0.0
    for i, s in enumerate(steps, 1):
        if s.kind == "note":
            print(f"    . {s.text}")
        else:
            total += s.seconds
            print(f"{i}. {s.text}    [~{s.seconds:.0f} s, "
                  f"cumulative {total / 60:.1f} min]")
        for w in s.why:
            print(f"     - {w}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
