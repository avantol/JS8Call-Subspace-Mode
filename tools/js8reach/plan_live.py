"""plan_live.py — routing decided ONLY by the live map.

Operator directive, 2026-08-21: "ignore historic all.txt! don't factor
it in to your logic. if we need the last hour's info, we have that in
the map dump."

So this module takes exactly one input: `RX.GET_SPOT_MAP`. No mined
logs, no decode counts, no answer rates, no months-old edges. If a fact
is not in the map dump it does not enter the decision. The only thing
imported from elsewhere is grid arithmetic, which is geometry, not
history.

WHAT THE MAP GIVES US, and it is enough:
  * who has been heard TRANSMITTING in the window (they key up)
  * who is only known to be LISTENING (reporting others; receiver
    live, nothing heard from them so far)
  * who is hearing whom, right now, with SNR and age
  * every station's grid

THE RULES
  1. If the target has been heard transmitting -> call it directly.
     It is on the air and it keys; nothing beats asking it yourself.
  2. If the target is only known to be listening -> still call it
     (its receiver is live), but expect nothing: a silent answer
     proves nothing about the path.
  3. Relay through a station where the legs are live in the map.
     MIND THE DIRECTION -- they are different questions:
       DELIVERY  our traffic reaches the target only if the TARGET
                 HEARS THE RELAY. Evidence: the target reported
                 hearing it (target -> relay edge).
       RETURN    the target's answer reaches us only if the RELAY
                 HEARS THE TARGET (relay -> target edge).
     A relay with only the delivery leg still gets the message there;
     one with both can carry a reply. Looking only at "who hears the
     target" missed the best relay entirely (AE0YH, 2026-08-21: K2AY
     was reporting it at -13 while nobody was hearing K2AY at all).
  4. No live relay -> ask the furthest station toward the target,
     that we have heard transmitting, what it hears (HEARING?).
  5. Nothing live at all -> do not transmit. Watch (watch.py).
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import grid as G          # noqa: E402  (geometry, not history)
from callsign import base, same  # noqa: E402
from live import LiveMap  # noqa: E402


@dataclass
class Step:
    text: str                 # exactly what to put on the air
    why: list[str]
    kind: str = "ping"


def _km(a: str, b: str) -> float:
    if not (a and b and G.valid(a) and G.valid(b)):
        return float("inf")
    try:
        return G.distance_km(a, b)
    except ValueError:
        return float("inf")


def transmitting_stations(lm: LiveMap, within_s: float) -> dict:
    """call -> (age_s, snr) for stations heard TRANSMITTING. These are
    the only ones we know can key up."""
    out: dict = {}
    for s in lm.spots:
        if not (0 <= s.age_s <= within_s) or not s.call:
            continue
        prev = out.get(s.call)
        if prev is None or s.age_s < prev[0]:
            out[s.call] = (s.age_s, s.snr)
    return out


def plan(lm: LiveMap, target: str, within_s: float = 3600
         ) -> list[Step]:
    t = target.upper()
    steps: list[Step] = []
    tgrid = lm.grid_of(t)
    state = lm.is_active(t, within_s)
    tx_now = transmitting_stations(lm, within_s)
    hearers = lm.hearers_of(t, within_s)

    # --- rule 1 / 2: the target itself ---------------------------
    if state and state[0] == "transmitting":
        steps.append(Step(f"{t} SNR?", [
            f"{t} was heard TRANSMITTING {state[1] / 60:.0f} min ago "
            f"({state[2]}) -- it keys up, so call it"], "ping"))
    elif state and state[0] == "listening":
        why = [f"{t} is only known to be LISTENING so far "
               f"({state[1] / 60:.0f} min ago: {state[2]})"]
        if t not in lm.transmitters():
            # SCOPE, stated honestly: the map holds roughly an hour, so
            # this says NOTHING about "ever" -- an occasional beacon is
            # absent from it as a matter of course. An earlier version
            # claimed "never seen transmitting, any age", which was
            # true of K2AY only because a separate 5.5-month log check
            # had confirmed it; the same sentence printed for AL0A
            # would have been flatly false (41 transmissions on file,
            # including an ARQ ACK to us). A test that does not imply
            # the claim is broken at design time, however often it
            # happens to be right.
            why += [f"not seen transmitting in the map's window, only "
                    f"reporting others -- its RECEIVER is live, which "
                    f"is what a call needs",
                    "that is NOT evidence it is receive-only: this "
                    "window is about an hour, and beacons are sparser "
                    "than that. Confirming a station never transmits "
                    "needs evidence this map cannot hold."]
        else:
            why.append("worth a call -- its receiver is live -- but "
                       "silence will prove nothing")
        steps.append(Step(f"{t} SNR?", why, "ping"))
    else:
        steps.append(Step("(do not transmit)", [
            f"nothing in the last {within_s / 60:.0f} min shows {t} "
            f"transmitting OR listening",
            "watch for it instead: python3 watch.py " + t], "wait"))

    # --- rule 3: relays, by direction ----------------------------
    # DELIVERY leg: stations the TARGET is hearing right now.
    delivers = {c: (age, snr) for c, age, snr
                in lm.reports_by(t, within_s)}
    # RETURN leg: stations hearing the TARGET right now.
    returns = {c: (age, snr) for c, age, snr in hearers}

    cands = []
    for c in set(delivers) | set(returns):
        if same(c, t) or same(c, lm.my_call):
            continue
        ours = tx_now.get(c)
        if not ours:
            continue                  # we have never heard it key up
        d = delivers.get(c)
        r = returns.get(c)
        # Delivery is what gets our traffic there, so it ranks first;
        # a relay that can also carry the reply outranks one that
        # cannot.
        cands.append((0 if d else 1, 0 if r else 1,
                      -(d or r)[1], c, d, r, ours))
    cands.sort()
    for _dk, _rk, _snr, c, d, r, ours in cands[:3]:
        why = []
        if d:
            why.append(f"{t} is hearing {c} NOW: {d[1]:+d} dB, "
                       f"{d[0] / 60:.0f} min ago -- our traffic can "
                       f"REACH it this way")
        if r:
            why.append(f"{c} is hearing {t} NOW: {r[1]:+d} dB, "
                       f"{r[0] / 60:.0f} min ago -- a reply can come "
                       f"BACK this way")
        if d and not r:
            why.append("delivery leg only: nobody is currently hearing "
                       f"{t}, so an answer may not return")
        why.append(f"we heard {c} transmitting {ours[0] / 60:.0f} min "
                   f"ago at {ours[1]:+d}")
        why.append("if we hear it forward, the relay is proven even if "
                   "the target never answers")
        steps.append(Step(f"{c}>{t} SNR?", why, "relay"))
    live_relays = cands

    # --- rule 4: extend the horizon ------------------------------
    if not live_relays and tgrid:
        best, best_prog = None, 0.0
        base_km = _km(lm.my_grid, tgrid)
        for call, (age, snr) in tx_now.items():
            if same(call, t) or same(call, lm.my_call):
                continue
            g = lm.grid_of(call)
            d = _km(g, tgrid)
            if d == float("inf") or base_km == float("inf"):
                continue
            prog = base_km - d
            if prog > best_prog:
                best, best_prog = (call, age, snr, d), prog
        if best:
            call, age, snr, d = best
            steps.append(Step(f"{call} HEARING?", [
                f"nobody on the map is hearing {t} right now",
                f"{call} is the furthest station toward it that we "
                f"have heard transmitting ({best_prog:.0f} km of "
                f"progress, {d:.0f} km still to run, heard "
                f"{age / 60:.0f} min ago at {snr:+d})",
                "ask what IT hears -- that reaches past our horizon"],
                "hearing"))
    return steps


def _main() -> int:
    import argparse
    ap = argparse.ArgumentParser(
        description="Plan a route using ONLY the live spot map")
    ap.add_argument("target")
    ap.add_argument("--within-min", type=float, default=60.0)
    args = ap.parse_args()
    lm = LiveMap.fetch()
    print(f"live map: band {lm.band}, {len(lm.spots)} spots, "
          f"me {lm.my_call}/{lm.my_grid}  "
          f"(no historic data used)")
    steps = plan(lm, args.target, args.within_min * 60)
    print()
    for i, s in enumerate(steps, 1):
        print(f"{i}. {s.text}")
        for w in s.why:
            print(f"     - {w}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
