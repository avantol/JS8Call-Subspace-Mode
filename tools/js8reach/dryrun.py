"""dryrun.py — what would it send, and why? Transmits NOTHING.

    python3 dryrun.py K3FHP
    python3 dryrun.py K3FHP --messages 6

Andy asked how the algorithm gets tested on the air. This is the first
of three stages, and the only one that costs nothing:

    DRY RUN      choose, print the reasons, stop.       <- this file
    SUPERVISED   same, but wait for a keypress and then transmit.
    UNATTENDED   only after enough supervised runs to trust it.

The point of doing it on the ground first is that the error which cost
the most on 2026-08-23/24 was always the same shape: the model
believing something the band does not support. A relay through a
station we have never heard. A link nobody has reported in a day. A
grid we had not learned yet. Every one of those is visible in the
factor table below, for free, before a single carrier goes out.

Read the bars, not the totals. If it wants to route through somebody
whose "we can raise" bar is 2, that is the moment to stop and ask why.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import decide                          # noqa: E402
from callsign import base              # noqa: E402
from live import LiveMap               # noqa: E402
from livemodel import LiveBoard, LiveModel   # noqa: E402


class _Pretend:
    """Stands in for the replay's World: remembers what we have already
    sent and what came back, so the rule advances instead of choosing
    the same message forever. Nothing here touches a radio."""

    def __init__(self, model, board, target):
        self.model = model
        self.board = board
        self.target = base(target).upper()
        self.t0 = model.now
        self.t = model.now
        self.tried: dict = {}
        self.asked: set = set()
        self.asked_grid: set = set()
        self.learned: dict = {}
        self.relocated: set = set()
        self.gave_up = False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("target", help="callsign to reach")
    # NOT "steps", which reads as hops. A MESSAGE is one transmission;
    # a HOP is a station inside one relay chain. They are independent:
    # one message can carry up to four hops, and a four-message plan may
    # be all direct calls with no hops at all.
    ap.add_argument("--messages", "--steps", type=int, default=5,
                    dest="messages", metavar="N",
                    help="how many transmissions to plan ahead "
                         "(NOT relay hops)")
    ap.add_argument("--band", default="")
    # CONTINUE AN ATTEMPT ALREADY UNDER WAY. Andy, 2026-08-24: "after
    # doing step 2, can i get an updated step 3 by invoking dryrun
    # again?" -- not without this: each run started from an empty slate
    # and re-planned the direct call every time.
    #
    #   --sent snr                       the direct call has gone out
    #   --sent snr,allcall               ... and the @ALLCALL QUERY CALL
    #   --sent snr,allcall,relay:KA2UGZ  ... and a relay through KA2UGZ
    #
    # "allcall", not "shout" -- that was my coinage and it leaked into
    # a flag an operator has to type. The message on the air is
    # "@ALLCALL QUERY CALL <target>?", so the flag says allcall.
    #
    # The GRAPH updates on its own: replies to a QUERY CALL land in the
    # database as fresh radio edges with the reported SNR and backdated
    # age (once #178 is verified), so a later run sees them without
    # being told. It is only what WE sent that leaves no trace the tool
    # can read -- the map's ATTEMPTS list expires -- so it is named
    # here rather than guessed.
    ap.add_argument("--sent", default="", metavar="LIST",
                    help="messages already transmitted this attempt, "
                         "comma separated: snr, grid, allcall, "
                         "relay:CALL, hearing:CALL")
    args = ap.parse_args()

    lm = LiveMap.fetch(band=args.band)
    target = base(args.target).upper()
    model = LiveModel(lm, band=args.band)
    board = LiveBoard(model, target)

    known = model.rows_on_band()
    if known == 0:
        print(f"\n  NO DATA FOR BAND {model.band!r} in the last 24 h.")
        print(f"  Bands the database actually holds: "
              f"{', '.join(model.bands_known()) or '(none)'}")
        print("  Refusing to plan -- an empty band and an unreachable "
              "station look identical from here, and saying the wrong "
              "one is worse than saying nothing.\n")
        return 1

    print(f"\n  band {model.band}   me {model.mycall}   "
          f"target {target}"
          f"{'  (grid ' + model.grid_of(target) + ')' if model.grid_of(target) else '  (no grid known)'}")
    print(f"  {len(board.pool)} candidate relays, "
          f"{len(lm.active(1800))} stations on the air in the last 30 min")
    if not board.pool:
        print(f"\n  ({known:,} edges on this band in 24 h, so the data "
              f"is there.)")
        print("\n  NOTHING TO ROUTE THROUGH -- nobody has been reported "
              "hearing this station in 24 h, and no located station is "
              "within 1200 km of it. That is a real answer, not a "
              "failure: on the air this would be direct calls only.")

    w = _Pretend(model, board, target)

    # Anything the map still has in flight, plus whatever was named.
    for a in (getattr(lm, "attempts", None) or []):
        for hop in (a.get("PATH") or []):
            h = base(hop).upper()
            if h and h != target:
                w.tried[("relay", h)] = w.t - float(a.get("AGE_S", 0) or 0)
                w.asked.add(h)
    already = []
    for tok in (t.strip().lower() for t in args.sent.split(",") if t.strip()):
        kind, _, who = tok.partition(":")
        who = base(who).upper() if who else ""
        key = {"snr": ("snr", target), "grid": ("grid", target),
               "allcall": ("shout", ""),
               "shout": ("shout", "")}.get(kind, (kind, who))
        w.tried[key] = w.t - 60.0
        if who:
            w.asked.add(who)
        already.append(tok)
    if already:
        print(f"  continuing: already sent {', '.join(already)}")

    d = decide.Decider(model, board, target, model.mycall)
    d._routes = decide.best_routes(d, board.pool)

    print(f"\n  {'-' * 62}\n  WOULD SEND, in order. Nothing is transmitted.\n"
          f"  {'-' * 62}")
    for step in range(1, args.messages + 1):
        mv = d.choose(w)
        print(f"\n  [{step}]  at +{(w.t - w.t0) / 60:.1f} min")
        print(decide.explain(mv, target, model.mycall))
        w.tried[(mv.kind,
                 mv.via or (target if mv.kind in ("snr", "grid") else ""))] = w.t
        if mv.via:
            w.asked.add(mv.via)
        w.t += mv.cost
    print(f"\n  total {(w.t - w.t0) / 60:.1f} min of the 30-minute budget\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
