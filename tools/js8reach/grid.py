"""grid.py — Maidenhead geometry and the "nearest station to a grid"
resolver for target case (b).

Mirrors the app's Geodesic semantics (JS8_Main/Geodesic.cpp): grids are
resolved to their square/subsquare centre, distance is great-circle in
km, and — importantly — the app treats any pair involving a 4-character
grid as unresolvable below CLOSE = 120 km (Geodesic.h:78). We keep that
honesty here: `resolve()` reports coarse pairs rather than pretending
to sub-square precision the locator never carried.

Selection rule: "nearest" is a requirement, but an unreachable station
is not a contact. Candidates are therefore ranked by expected time to
contact within a distance tolerance, not by distance alone — a live
station 200 km from the target square beats a silent one inside it.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

CLOSE_KM = 120.0
EARTH_R_KM = 6371.0


def valid(grid: str) -> bool:
    g = (grid or "").upper()
    if len(g) < 4 or len(g) % 2:
        return False
    if not ("A" <= g[0] <= "R" and "A" <= g[1] <= "R"):
        return False
    if not (g[2].isdigit() and g[3].isdigit()):
        return False
    if len(g) >= 6 and not ("A" <= g[4] <= "X" and "A" <= g[5] <= "X"):
        return False
    return True


def to_latlon(grid: str) -> tuple[float, float]:
    """Centre of the smallest addressed cell. Only the first 8 chars
    are significant, matching Geodesic::gridData."""
    g = (grid or "").upper()[:8]
    if not valid(g):
        raise ValueError(f"invalid grid {grid!r}")
    lon = (ord(g[0]) - 65) * 20.0 - 180.0
    lat = (ord(g[1]) - 65) * 10.0 - 90.0
    lon += int(g[2]) * 2.0
    lat += int(g[3]) * 1.0
    if len(g) >= 6:
        lon += (ord(g[4]) - 65) * (2.0 / 24.0)
        lat += (ord(g[5]) - 65) * (1.0 / 24.0)
        if len(g) >= 8:
            lon += int(g[6]) * (2.0 / 240.0)
            lat += int(g[7]) * (1.0 / 240.0)
            lon += 1.0 / 240.0
            lat += 0.5 / 240.0
        else:
            lon += 1.0 / 24.0
            lat += 0.5 / 24.0
    else:
        lon += 1.0
        lat += 0.5
    return lat, lon


def distance_km(a: str, b: str) -> float:
    la, lo = to_latlon(a)
    lb, lob = to_latlon(b)
    p1, p2 = math.radians(la), math.radians(lb)
    dl = math.radians(lob - lo)
    x = (math.sin(p1) * math.sin(p2)
         + math.cos(p1) * math.cos(p2) * math.cos(dl))
    d = EARTH_R_KM * math.acos(max(-1.0, min(1.0, x)))
    # The app refuses sub-CLOSE resolution when either grid is a bare
    # square; report the same limitation instead of inventing precision.
    if (len(a) < 6 or len(b) < 6) and d < CLOSE_KM:
        return 0.0
    return d


@dataclass
class Candidate:
    call: str
    grid: str
    km: float
    p: float
    t: float
    why: list[str]

    @property
    def index(self) -> float:
        return self.p / self.t if self.t else 0.0


def candidates_near(db, model, target_grid: str, *, limit: int = 8,
                    tolerance_km: float = 400.0,
                    window_s: float = 3600.0) -> list[Candidate]:
    """Stations in or nearest `target_grid`, ranked by reachability.

    Distance gates membership (tolerance_km); the p/t index orders what
    survives, so we call the station we can actually raise soonest.
    """
    import intel
    out: list[Candidate] = []
    for row in intel.all_calls_with_grid(db):
        g = row["grid"]
        if not valid(g):
            continue
        try:
            km = distance_km(target_grid, g)
        except ValueError:
            continue
        if km > tolerance_km:
            continue
        b = model.p_direct(row["call"], window_s)
        import actions as A
        out.append(Candidate(row["call"], g, km, b.p, A.rtt(1, 1),
                             b.why + [f"{km:.0f} km from {target_grid}"]))
    # Nearest-first within the tolerance, but reachability decides
    # among them: sort by index, keeping distance as the tiebreak.
    out.sort(key=lambda c: (-c.index, c.km))
    return out[:limit]
