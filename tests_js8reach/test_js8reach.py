#!/usr/bin/env python3
"""Unit tests for js8reach: the timing model must agree with the app's
C++ constants, and the ordering must be the provably optimal one.

Run:  python3 tests_js8reach/test_js8reach.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent
                      / "tools" / "js8reach"))

import actions as A          # noqa: E402
from planner import Planner, _shared_decay  # noqa: E402

FAIL = 0


def check(cond, what):
    global FAIL
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        FAIL += 1


# ---- 1. timing model vs ChunkedArq.h -------------------------------
# replyDeadlineMsForSubmode counts boundaries: the decision instant is
# B0 + (2 + replyFrames) * P, where B0 ends our own transmission.
# Our rtt() adds the mean alignment wait (P/2) and our own TX time.
PERIOD = 15.0
for reply_frames in (1, 2, 3, 4):
    expect = PERIOD / 2 + 1 * PERIOD + (2 + reply_frames) * PERIOD
    got = A.rtt(1, reply_frames)
    check(abs(got - expect) < 1e-9,
          f"rtt(1 tx, {reply_frames} reply) = {got:.1f}s "
          f"matches B0+(2+{reply_frames})P + align")

check(abs(A.rtt(1, 1) - 67.5) < 1e-9,
      "SNR? ping costs 67.5 s (1 frame out, 1-frame reply)")
check(abs(A.rtt(2, 2) - 97.5) < 1e-9,
      "QUERY CALL sweep costs 97.5 s (body+CRC out, 2-frame reply)")
# 60 s each way, MEASURED on air 2026-08-22 across three consecutive
# hops of WM8Q>KJ7VWV>KB7ITU>KL7UT. The old 45 was a guess from the
# frame count and it under-budgeted every chain we ran.
check(abs(A.rtt(2, 1, relay_hops=1) - A.rtt(2, 1) - 120.0) < 1e-9,
      "one relay hop adds 120 s round trip (60 s each way, measured)")

# ---- 2. the index ordering is the optimal one ----------------------
# For sequential attempts until first success, E[T] is minimized by
# ordering on p/t descending. Verify against brute force.
import itertools  # noqa: E402


def et(plan):
    return Planner.expected_time(plan)


acts = [
    A.ping("A", 0.50, []),                    # t=67.5
    A.relay_ping("R1", "A", 0.40, []),        # t=187.5
    A.status("A", 0.30, []),                  # t=97.5
]
best = min(itertools.permutations(acts), key=et)
by_index = tuple(sorted(acts, key=lambda a: -a.index))
check(abs(et(best) - et(by_index)) < 1e-9,
      "p/t ordering achieves the brute-force optimum")

# The exchange argument itself: swapping two adjacent actions is worse
# exactly when the index order says so.
a1 = A.ping("A", 0.6, [])
a2 = A.status("A", 0.2, [])
check(et([a1, a2]) < et([a2, a1]),
      "higher p/t first beats the swap (adjacent exchange)")

# ---- 3. information actions carry no success mass ------------------
sweep = A.broadcast_query_call("A", 0.9, [])
check(sweep.info is True, "broadcast is marked as an information action")
solo = [A.ping("A", 0.5, [])]
check(et(solo + [sweep]) > et(solo),
      "a trailing sweep only adds cost, never success mass")

# ---- 4. shared answer-belief decay ---------------------------------
# A relay attempt after failed direct calls must NOT inherit a fresh
# answer probability: silence is evidence against answering by ANY
# route, and a relay only fixes propagation.
plan = [A.ping("T", 0.5, []), A.relay_ping("R", "T", 0.5, [])]
decayed = _shared_decay(plan)
check(decayed[1].p < plan[1].p,
      "relay after a failed direct call is discounted (shared latent)")
check(abs(decayed[0].p - plan[0].p) < 1e-9,
      "the first attempt is undiscounted")

# ---- 5. grid geometry ----------------------------------------------
import grid as G  # noqa: E402

check(G.valid("DN61") and G.valid("DM79NE33") and not G.valid("XX99"),
      "Maidenhead validation accepts real grids, rejects bad fields")
d = G.distance_km("DN61", "EM12")
check(1200 < d < 2200, f"DN61->EM12 great-circle {d:.0f} km is plausible")
check(G.distance_km("DN61", "DN61XX") == 0.0,
      "4-char grid pairs report <CLOSE rather than false precision "
      "(mirrors Geodesic CLOSE=120 km)")

# ---- 5. phantom hearing edges (TODO #167) --------------------------
# The app grows a hearing edge when A merely ADDRESSES B, so a relay
# forward of OUR OWN probe fabricates the return leg we were trying to
# measure. Reproduces the K2AY case of 2026-08-21 exactly.
from live import LiveMap, Sighting  # noqa: E402

# "K2AY heard AE0YH" is a RELATIONSHIP, so it is a hearing edge. It used
# to ride on a spot's HEARD_BY; that field is gone (Build 372) because a
# spot names one station and the store holds every pair.
phantom = LiveMap(
    band="40m", my_call="WM8Q", my_grid="DN61OK",
    grids={"K2AY": "FN13HC", "AE0YH": "EN41QN"},
    spots=[Sighting(call="AE0YH", grid="EN41QN", age_s=900, snr=-13)],
    hearing=[{"CALL": "AE0YH", "GRID": "EN41QN", "AGE_S": 175,
              # the phantom: our own relayed probe, never decoded
              "HEARS": [{"CALL": "K2AY", "GRID": "FN13HC",
                         "SNR": -99, "AGE_S": 385}]},
             {"CALL": "K2AY", "GRID": "FN13HC", "AGE_S": 900,
              # the real leg: K2AY actually decoded AE0YH
              "HEARS": [{"CALL": "AE0YH", "GRID": "EN41QN",
                         "SNR": -13, "AGE_S": 900}]}])

check([h[0] for h in phantom.hearers_of("K2AY", 3600)] == [],
      "an SNR-less hearing edge to a never-transmitting station is "
      "rejected (our own relayed call is not evidence it was heard)")
check(phantom.is_active("K2AY", 3600)[0] == "listening",
      "the phantom edge does not promote a receive-only station to "
      "'transmitting'")
# This verdict now comes from the HEARER side of the hearing store. It
# used to come from a spot's HEARD_BY, and deleting that field without
# adding the branch would have silently lost it for every station that
# is not reporting ME.
check("reported hearing AE0YH" in phantom.is_active("K2AY", 3600)[2],
      "'listening' is derived from the target appearing as a HEARER")
check([c for c, _a, _s in phantom.reports_by("K2AY", 3600)] == ["AE0YH"],
      "the real delivery leg survives: K2AY's own spot of AE0YH")

real = LiveMap(band="40m", my_call="WM8Q", my_grid="DN61OK",
               hearing=[{"CALL": "AE0YH", "AGE_S": 175,
                         "HEARS": [{"CALL": "VA3NB", "SNR": -18,
                                    "AGE_S": 385}]}])
check([h[0] for h in real.hearers_of("VA3NB", 3600)] == ["AE0YH"],
      "an edge carrying a real SNR is kept -- something was decoded")
check(real.is_active("VA3NB", 3600)[0] == "transmitting",
      "a decoded edge proves the station keyed up, even with no spot")

# ---- callsign identity: base() must pick the CALL, not the longest --
from callsign import (base, is_amateur_call,          # noqa: E402
                      is_receive_only, is_routable)

for raw, want in [("AL0A/P", "AL0A"),
                  ("VE3/W1ABC", "W1ABC"),
                  ("W1ABC/QRP", "W1ABC"),
                  ("W1ABC/7", "W1ABC"),
                  ("KH6/W1ABC", "W1ABC"),
                  ("VP2E/W1ABC", "W1ABC"),
                  ("W1ABC", "W1ABC")]:
    check(base(raw) == want, f"base({raw}) = {want}")

# The bug that prompted this: an affix LONGER than the call. The old
# longest-token rule returned "FULLERTON", so the station matched
# nothing and could not be routed to.
check(base("W6MFB/FULLERTON") == "W6MFB",
      "base() survives an affix longer than the call (W6MFB/FULLERTON)")

# Routability is TWO questions, and they are not the same one.
check(is_amateur_call("W1ABC") and is_amateur_call("AL0A/P"),
      "real calls are amateur calls, affix or not")
for junk in ("DIG647", "SWL45", "13RF1146", "109HA3651",
             "RAVEHEART", "WESSEX", "20DR3VIL", "PL6W702WI"):
    check(not is_amateur_call(junk),
          f"{junk} is not an amateur call (SWL / freeband / pirate)")

check(is_receive_only("K1RA-4") and is_receive_only("WH6HJH-RX"),
      "hyphen suffix marks a receive-only node")
check(is_amateur_call("K1RA-4") is False,
      "a receive-only node is judged on its own identity, not the operator's")
check(not is_routable("K1RA-4"),
      "a skimmer is not routable even though K1RA is a real station")
check(is_routable("W1ABC/P") and not is_routable("DIG647"),
      "is_routable admits real stations and rejects non-stations")

print()
print(f"{'ALL PASS' if not FAIL else str(FAIL) + ' FAILURE(S)'}")
sys.exit(1 if FAIL else 0)
