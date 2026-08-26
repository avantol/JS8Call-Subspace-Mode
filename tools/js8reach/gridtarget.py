"""gridtarget.py — TODO #180: grid -> best station, then the normal loop.

The step that was missing when the target was EL87 (2026-08-25): the
grid-to-station choice was improvised by hand, and the hand picked
KD2M -- a receive-only monitor that cannot answer by construction. The
disqualifying columns (rx_only, radio_when never) were in the very
query the hand ran, unread. A screen that exists in code fires every
time; attention demonstrably does not.

What this does, ahead of the existing pipeline:

  1. COLLECT   stations in/near the square, grid bank + corpus,
               honouring the 120 km floor of 4-char grids;
  2. SCREEN    rx_only or never-radio-heard => WARN and deprioritise,
               NEVER refuse (#173, operator: "there are several gates
               to that");
  3. RANK      by reachability, not distance -- a live station 200 km
               out beats a silent one at 50 (the plan's own doctrine);
  4. HAND OVER the winner to the callsign loop unchanged.

And the EL87 lesson that is not obvious: a monitor in the square that
REPORTS US is a partial success worth stating -- KD2M proved our
signal lands in EL87 at -9 even though nobody there could say so on
the air. "Delivery into the grid proven, no two-way available" is a
different answer from plain failure, and for some purposes it is the
whole answer.
"""
from __future__ import annotations

import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import grid as g                    # noqa: E402

GRID_RE = re.compile(r"^[A-R]{2}[0-9]{2}([A-X]{2})?$", re.I)


def is_grid(s: str) -> bool:
    return bool(GRID_RE.match((s or "").strip()))


class Candidate:
    __slots__ = ("call", "grid", "km", "radio_age", "any_age",
                 "rx_only", "reports_me", "snr_to_me", "warn")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))


def _age(now, when):
    return (now - when) if when else None


def collect(model, square: str, radius_km: float = 250.0) -> list:
    """Every known station in or near the square, screened and ranked."""
    now = time.time()
    seen: dict = {}
    # the live database, band-scoped: freshest view, carries the screen
    # columns
    for r in model.db.execute(
            "SELECT call, grid, any_when, radio_when, rx_only, "
            "reports_me, snr_to_me FROM stations WHERE band=? AND "
            "grid IS NOT NULL AND grid<>''", (model.band,)):
        try:
            km = g.distance_km(square, r["grid"])
        except Exception:
            continue
        if km > radius_km:
            continue
        never_radio = not r["radio_when"]
        c = Candidate(call=r["call"].upper(), grid=r["grid"], km=km,
                      radio_age=_age(now, r["radio_when"]),
                      any_age=_age(now, r["any_when"]),
                      rx_only=bool(r["rx_only"]), reports_me=bool(r["reports_me"]),
                      snr_to_me=r["snr_to_me"],
                      warn=("receive-only monitor" if r["rx_only"] else
                            "never heard on radio" if never_radio else ""))
        seen[c.call] = c
    # the corpus: five months of stations the live store has aged out
    if model.intel is not None:
        for r in model.intel.execute(
                "SELECT call, grid, last_heard FROM stations "
                "WHERE grid IS NOT NULL AND grid<>''"):
            call = r["call"].upper()
            if call in seen:
                continue
            try:
                km = g.distance_km(square, r["grid"])
            except Exception:
                continue
            if km > radius_km:
                continue
            seen[call] = Candidate(
                call=call, grid=r["grid"], km=km,
                radio_age=_age(now, r["last_heard"]), any_age=None,
                rx_only=False, reports_me=False, snr_to_me=None,
                warn="corpus only, not seen this session")

    def rank(c: Candidate):
        # reachability first, distance a tiebreak; screened stations
        # sink but survive (#173: warn, never refuse)
        cls = 4
        if c.radio_age is not None:
            cls = (0 if c.radio_age < 3600 else
                   1 if c.radio_age < 6 * 3600 else
                   2 if c.radio_age < 48 * 3600 else 3)
        elif c.any_age is not None and c.any_age < 3600:
            cls = 2
        if c.radio_age is None and c.any_age is not None:
            cls += 10
        if c.rx_only:
            cls += 20        # cannot answer by construction: sinks hardest
        return (cls, c.km, c.radio_age or c.any_age or 1e12)

    return sorted(seen.values(), key=rank)


def resolve(model, square: str, show=print) -> tuple:
    """(chosen_call_or_None, monitors_that_report_us). Prints its
    reasoning, ledger-style, so the choice can be argued with."""
    square = square.upper()[:6]
    cands = collect(model, square)
    if not cands:
        show(f"  no station known within 250 km of {square} on "
             f"{model.band}")
        return None, []
    monitors = [c for c in cands if c.rx_only and c.reports_me]
    show(f"  {square}: {len(cands)} candidates "
         f"(120 km floor applies to 4-char squares)")
    for c in cands[:8]:
        age = (f"radio {c.radio_age/60:.0f}m" if c.radio_age is not None
               and c.radio_age < 7200 else
               f"radio {c.radio_age/3600:.1f}h" if c.radio_age is not None
               else "radio never")
        w = f"   !! {c.warn}" if c.warn else ""
        show(f"    {c.call:9s} {c.grid:8s} {c.km:4.0f} km  {age}{w}")
    # WARN, NEVER REFUSE (#173). The ranking already sank the screened
    # candidates; the top of the list is the choice, warning and all.
    # Only a square whose every candidate is a receive-only monitor has
    # genuinely nobody to call -- and even that is reported as what it
    # is: delivery-in provable, two-way not.
    pick = next((c for c in cands if not c.rx_only), None)
    for m in monitors:
        show(f"  note: {m.call} (monitor, {m.km:.0f} km) reports us at "
             f"{m.snr_to_me:+d} dB -- delivery into {square} provable "
             f"even without a contact")
    if pick is None:
        show(f"  every known station in {square} is a receive-only "
             f"monitor: nothing can answer. "
             + ("Delivery-in proven; " if monitors else "")
             + "partial success at best.")
        return None, monitors
    tag = f"   (WARNED: {pick.warn})" if pick.warn else ""
    show(f"  -> {pick.call} ({pick.grid}, {pick.km:.0f} km) enters the "
         f"normal loop as the target{tag}")
    return pick.call, monitors
