#!/usr/bin/env python3
"""sweep.py — one broadcast that evaluates the whole relay pool, and
then READS THE ANSWERS AS A VERDICT ON THE TARGET.

`@ALLCALL QUERY CALL T?` costs one transmission. Every station that has
heard T answers "YES +snr (age)" on its own offset, so the full-passband
decoder reads them concurrently -- it dominates k unicast probes for any
k >= 2, and it is the only cheap way to learn the DELIVERY leg (who
hears T) that we cannot observe ourselves.

WHY THIS EXISTS AS A MODULE, and why triage runs BEFORE the relay menu:
AL0A, 2026-08-21. Five stations answered. Sorted by SNR the menu looked
great -- NT5DF -9, KF0DRT -12, AE5WX -18. Sorted by AGE it said
something entirely different: the FRESHEST sighting of AL0A anywhere on
the band was 8 HOURS old. The target was not transmitting, and no relay
fixes that. A menu that ranks on SNR while ignoring age invites exactly
the airtime we had already wasted (direct call + a relay attempt, both
silent, both fully explained by that one number). Same story as VA3NB
days earlier: seven answers, freshest 6 h, two relay attempts that
discovered what the ages already said.

SO: the verdict prints FIRST, and the relay menu is SUPPRESSED when the
target reads as absent. rules.triage() has encoded this since VA3NB --
the failure was never the rule, it was calling it too late.

SEQUENCING (the other AL0A lesson): when the target has NOT been heard
recently, sweep FIRST, before direct calls and relays. "A direct call is
cheapest per attempt" is the wrong metric when the open question is
"is the target on the air at all" -- one sweep answers that for the
entire pool at once.

    python3 sweep.py AL0A
"""
from __future__ import annotations

import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import rules  # noqa: E402
from callsign import same  # noqa: E402
from js8client import Js8Client  # noqa: E402

# Cold-probe answers are AUTOREPLIES and they are fast: measured over
# 10,539 replies, 87% arrive within 60 s and 94% within 120 s. The
# >120 s tail is 76% human free text -- people typing mid-QSO, not
# people belatedly answering a sweep. So 150 s is generous.
WINDOW_S = 150.0


def log(m: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def run(target: str, window_s: float = WINDOW_S) -> dict:
    t = target.upper()
    with Js8Client() as js8:
        sp = js8.request("MODE.GET_SPEED", reply_type="MODE.SPEED")
        speed = sp.get("params", {}).get("SPEED")
        if speed != 0:
            log(f"REFUSING to transmit: need Normal (0), radio is in "
                f"{speed}")
            return {}
        log(f"TX -> @ALLCALL QUERY CALL {t}?")
        js8.send_and_wait_complete(f"@ALLCALL QUERY CALL {t}?",
                                   timeout=240)
        log(f"TX done; collecting for {window_s:.0f} s")
        deadline = time.time() + window_s
        yes: dict = {}
        while time.time() < deadline:
            try:
                m = js8.wait_for("RX.DIRECTED",
                                 timeout=max(5, deadline - time.time()))
            except Exception:
                break
            p = m.get("params", {})
            frm = (p.get("FROM") or "").upper()
            up = (m.get("value") or "").strip().upper()
            if same(frm, t):
                log(f"  *** {t} ITSELF ANSWERED -- it is ON THE AIR")
                yes[t] = (0, "NOW")
                break
            if "YES" in up and "WM8Q" in up:
                mm = re.search(r"YES\s+([+-]?\d+)(?:\s*\(([^)]+)\))?", up)
                if mm:
                    yes[frm] = (int(mm.group(1)), mm.group(2) or "?")
                    log(f"  {frm} hears {t}: {yes[frm][0]:+d} dB, "
                        f"{yes[frm][1]} ago")
    return yes


def report(target: str, yes: dict) -> None:
    t = target.upper()
    print()
    if not yes:
        print(f"=== NOBODY ANSWERED for {t} ===")
        print("Not proof the target is absent: it means no station that "
              "can hear US also hears it.")
        return
    # VERDICT FIRST. Never a relay menu before this.
    absent, verdict = rules.triage(yes)
    print(f"=== VERDICT ON {t} ===")
    print(f"  {verdict}")
    ages = sorted(((rules.parse_age(a), c, s)
                   for c, (s, a) in yes.items()))
    print()
    print(f"  {'call':10s}{'hears it':>10s}{'age':>10s}")
    for secs, c, snr in ages:
        print(f"  {c:10s}{snr:+10d}{secs / 3600:9.1f}h")
    if absent:
        print()
        print("  RELAY MENU SUPPRESSED -- relaying cannot reach a "
              "station that is not transmitting.")
        print("  The answer here is a TIME, not a route: come back when "
              "the target is heard again")
        print(f"  (watch it for free: python3 watch.py {t}).")
        return
    print()
    print("  relays, freshest first:")
    for secs, c, snr in ages:
        print(f"    {c}>{t} SNR?   ({snr:+d} dB, {secs / 3600:.1f}h ago)")


def _main() -> int:
    import argparse
    ap = argparse.ArgumentParser(
        description="Broadcast sweep + verdict for one target")
    ap.add_argument("target")
    ap.add_argument("--window", type=float, default=WINDOW_S)
    a = ap.parse_args()
    report(a.target, run(a.target, a.window))
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
