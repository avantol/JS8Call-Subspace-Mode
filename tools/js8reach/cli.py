"""cli.py — js8reach: plan (and later execute) the fastest way to reach
a station or a grid, with no internet.

    python3 cli.py --call KD7WPQ
    python3 cli.py --grid DN61 --message "PSE QSY 7078"
    python3 cli.py --call AC7WY --explain      # show the evidence trail

Prints the ordered probe plan with each step's expected time, success
probability and the p/t index it was ordered on, plus E[time to
contact] for the whole plan. No transmission happens here — the live
runner is a separate, supervised step.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import actions as A            # noqa: E402
import grid as G               # noqa: E402
import intel                   # noqa: E402
import window as W             # noqa: E402
from model import Model        # noqa: E402
from planner import Planner    # noqa: E402


def fmt_t(sec: float) -> str:
    return f"{sec / 60:.1f} min" if sec >= 90 else f"{sec:.0f} s"


def show_plan(plan: list[A.Action], explain: bool,
              scored: bool = False) -> None:
    """Print the ordered plan. The scored view adds p and p/t; the
    default rules view omits them deliberately — they are a sort key,
    not a measured probability, and printing them invites more
    confidence than they deserve."""
    if scored:
        print(f"{'#':>2}  {'action':<34} {'t':>8} {'p':>6} {'p/t':>9}  kind")
    else:
        print(f"{'#':>2}  {'action':<34} {'t':>8}  kind")
    print("  " + "-" * (74 if scored else 56))
    elapsed = 0.0
    for i, a in enumerate(plan, 1):
        tag = a.kind + (" (info)" if a.info else "")
        if scored:
            print(f"{i:>2}  {a.text:<34} {fmt_t(a.t):>8} {a.p:>6.2f} "
                  f"{a.index * 1000:>8.2f}‰  {tag}")
        else:
            elapsed += a.t
            print(f"{i:>2}  {a.text:<34} {fmt_t(a.t):>8}  {tag}"
                  f"   (by +{fmt_t(elapsed)})")
        if explain:
            for w in a.why:
                print(f"      - {w}")
    print("  " + "-" * (74 if scored else 56))
    if scored:
        print(f"  E[time to contact] = "
              f"{fmt_t(Planner.expected_time(plan))}")
    else:
        print(f"  whole plan runs {fmt_t(sum(a.t for a in plan))} "
              f"if nothing answers")


def show_route(model, target: str) -> None:
    """The directional picture: which way, how far, and who is out
    that way that we can actually raise."""
    import route as _R
    tg = model.grid_of(target)
    if not (tg and model.mygrid):
        return
    brg = _R.bearing_deg(model.mygrid, tg)
    tot = _R._dist(model.mygrid, tg)
    hops = _R.forward_stations(model, target, limit=6)
    print(f"\ndirection: {target}({tg}) bears {brg:.0f} deg "
          f"({_R.compass(brg)}) at {tot:.0f} km")
    if not hops:
        print("  no station we have heard recently makes forward "
              "progress that way")
        return
    print(f"  {'call':<11}{'grid':<11}{'out':>7}{'progress':>10}"
          f"{'to go':>8}")
    for h in hops:
        print(f"  {h.call:<11}{h.grid:<11}{h.km_from_us:>6.0f}k"
              f"{h.progress:>9.0f}k{h.km_to_target:>7.0f}k")


def show_window(db, target_grid: str, now: int) -> None:
    """Propagation window for the target's region. For a long path the
    actionable answer is a TIME, which no action ranking can express."""
    import datetime as _dt
    if not target_grid or not G.valid(target_grid):
        return
    hours, contributors, totals = W.region_hours(db, target_grid)
    hour_now = _dt.datetime.fromtimestamp(now, _dt.timezone.utc).hour
    print(f"\ntime-of-day check for {target_grid[:4]} (share of what we "
          f"hear, NOT raw counts):")
    for line in W.describe(hours, contributors, hour_now, totals):
        print(f"  {line}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Plan the fastest route to a station or grid (offline)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--call", help="target callsign")
    g.add_argument("--grid", help="target Maidenhead grid")
    ap.add_argument("--message", help="traffic to deliver (enables the "
                                      "store-and-forward fallback)")
    ap.add_argument("--db", default=str(intel.DEFAULT_DB))
    ap.add_argument("--explain", action="store_true",
                    help="show the evidence behind every probability")
    ap.add_argument("--no-broadcast", action="store_true",
                    help="never use @ALLCALL sweeps")
    ap.add_argument("--route", action="store_true",
                    help="directional walk: ask the furthest station "
                         "toward the target what IT hears, then hop")
    ap.add_argument("--scored", action="store_true",
                    help="use the p/t scoring planner instead of the "
                         "deterministic rules (research)")
    ap.add_argument("--tolerance-km", type=float, default=400.0)
    ap.add_argument("--now", type=int, default=None,
                    help="plan as of this epoch (for replay)")
    args = ap.parse_args()

    now = args.now or int(time.time())
    db = intel.connect(args.db)
    mined = intel.get_meta(db, "mined_at")
    if not mined:
        print("js8reach: intel DB empty — run mine.py first",
              file=sys.stderr)
        return 1
    m = Model(db, now)
    pl = Planner(m, allow_broadcast=not args.no_broadcast,
                 message=args.message)

    def make_plan(target: str) -> list[A.Action]:
        # Deterministic rules are the DEFAULT (operator 2026-08-21, and
        # the replay evidence): they are inspectable, they agree with
        # the scored planner whenever the target was heard recently,
        # and where they differ they make the cheaper call first.
        if args.route:
            import route as _R
            p = _R.plan(m, target, message=args.message)
            if p:
                return p
            print("  (no station makes forward progress toward the "
                  "target -- falling back to the standard rules)")
        if args.scored:
            return pl.plan(target)
        import rules as R
        return R.plan(m, target, message=args.message,
                      allow_broadcast=not args.no_broadcast)

    if args.call:
        target = args.call.upper()
        st = intel.station(db, target)
        print(f"\ntarget {target}", end="")
        if st and st["last_heard"]:
            age = (now - st["last_heard"]) / 3600.0
            print(f"  (heard {st['heard_count']}x here, last "
                  f"{age:.1f}h ago, grid {st['grid'] or '?'})")
        else:
            print("  (never decoded at this station)")
        show_plan(make_plan(target), args.explain, args.scored)
        show_route(m, target)
        if st and st["grid"]:
            show_window(db, st["grid"], now)
        return 0

    # Grid case: resolve to candidate stations first.
    tgrid = args.grid.upper()
    if not G.valid(tgrid):
        print(f"js8reach: {tgrid} is not a valid grid", file=sys.stderr)
        return 1
    cands = G.candidates_near(db, m, tgrid, tolerance_km=args.tolerance_km)
    if not cands:
        print(f"\nno known station within {args.tolerance_km:.0f} km "
              f"of {tgrid}")
        return 0
    print(f"\nstations near {tgrid} (within {args.tolerance_km:.0f} km), "
          f"ranked by reachability:")
    print(f"  {'call':<10} {'grid':<9} {'km':>6} {'p':>6} {'p/t':>9}")
    for c in cands:
        print(f"  {c.call:<10} {c.grid:<9} {c.km:>6.0f} {c.p:>6.2f} "
              f"{c.index * 1000:>8.2f}‰")
    best = cands[0]
    print(f"\nbest first target: {best.call} ({best.grid}, "
          f"{best.km:.0f} km from {tgrid})")
    if args.explain:
        for w in best.why:
            print(f"    - {w}")
    show_plan(make_plan(best.call), args.explain, args.scored)
    show_window(db, tgrid, now)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
