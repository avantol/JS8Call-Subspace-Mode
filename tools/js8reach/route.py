"""route.py — directional (greedy geographic) routing.

Operator's rule, 2026-08-21:

    "determine the direction of the target station by grid. determine
     who the station farthest away from us in that direction can hear.
     pick a station from that list based on who's closer, repeat"

This is greedy geographic routing, and it is a better rule than
distance-to-target alone because it is CONSTRUCTIVE: each hop is chosen
to maximise progress toward the target, and `HEARING?` at the most
advanced station reveals stations beyond our own horizon — the ones we
could never have listed from our own logs.

PROGRESS is the metric, defined as the reduction in remaining distance:

    progress(C) = dist(us, T) - dist(C, T)

A station in the wrong direction has negative progress and drops out
automatically, so no explicit bearing cone is needed. "Farthest in that
direction" is exactly "greatest progress".

THE WALK
    hop 0: us
    hop k: among stations the previous hop can hear (from its HEARING?
           reply, or from our own logs for hop 1), take the one with
           the greatest progress that we have reason to believe is
           reachable from hop k-1.
    stop when a hop can hear the target, or progress stalls.

Hop 1 is special: its candidates are stations WE can hear, which we
know from our own decodes. Every later hop needs a HEARING? probe,
because only the remote station knows who it can hear.
"""

from __future__ import annotations

from dataclasses import dataclass

import actions as A
import grid as G
import intel
from callsign import base, same


@dataclass
class Hop:
    call: str
    grid: str
    km_from_us: float
    km_to_target: float
    progress: float
    known_reachable: bool
    why: str


def _dist(a: str, b: str) -> float:
    if not (a and b and G.valid(a) and G.valid(b)):
        return float("inf")
    try:
        return G.distance_km(a, b)
    except ValueError:
        return float("inf")


def bearing_deg(a: str, b: str) -> float:
    """True bearing a -> b, for display (the routing itself uses
    progress, which encodes direction implicitly)."""
    import math
    if not (a and b and G.valid(a) and G.valid(b)):
        return float("nan")
    la, lo = G.to_latlon(a)
    lb, lob = G.to_latlon(b)
    p1, p2 = math.radians(la), math.radians(lb)
    dl = math.radians(lob - lo)
    y = math.sin(dl) * math.cos(p2)
    x = (math.cos(p1) * math.sin(p2)
         - math.sin(p1) * math.cos(p2) * math.cos(dl))
    return (math.degrees(math.atan2(y, x)) + 360.0) % 360.0


def compass(deg: float) -> str:
    if deg != deg:  # NaN
        return "?"
    pts = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
           "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]
    return pts[int((deg + 11.25) % 360 / 22.5)]


def forward_stations(model, target: str, origin: str | None = None,
                     limit: int = 8, recent_days: int = 14) -> list[Hop]:
    """Stations that make PROGRESS toward `target` from `origin`
    (default: us), best first.

    Restricted to stations we have actually decoded recently — for hop
    1 that is the definition of "we can raise it". For later hops the
    caller supplies the candidate set from a HEARING? reply instead.
    """
    tgrid = model.grid_of(target)
    ogrid = model.grid_of(origin) if origin else model.mygrid
    if not (tgrid and ogrid):
        return []
    base = _dist(ogrid, tgrid)
    if base == float("inf"):
        return []
    cutoff = model.now - recent_days * 86400
    out: list[Hop] = []
    mybase = model.mycall.split("/")[0]
    for row in model.db.execute(
            "SELECT call, grid, last_heard FROM stations "
            "WHERE grid IS NOT NULL AND grid != '' AND last_heard > ?",
            (cutoff,)):
        c = row["call"]
        if same(c, target) or base(c) == mybase or same(c, origin):
            continue
        g = row["grid"]
        d_t = _dist(g, tgrid)
        if d_t == float("inf"):
            continue
        prog = base - d_t
        if prog <= 0:
            continue                      # wrong direction
        age_h = (model.now - row["last_heard"]) / 3600.0
        out.append(Hop(c, g, _dist(ogrid, g), d_t, prog, True,
                       f"heard {age_h:.1f}h ago, {prog:.0f} km of "
                       f"progress, {d_t:.0f} km still to run"))
    out.sort(key=lambda h: -h.progress)
    return out[:limit]


def plan(model, target: str, max_hops: int = 3,
         message: str | None = None) -> list[A.Action]:
    """The directional walk as an ordered action list.

    Hop 1 is chosen from our own decodes. Because only the remote
    station knows who IT can hear, the plan asks (`HEARING?`) rather
    than assuming, and the operator/runner picks the next hop from the
    reply.
    """
    out: list[A.Action] = []
    tgrid = model.grid_of(target)
    if not (tgrid and model.mygrid):
        return out

    brg = bearing_deg(model.mygrid, tgrid)
    total = _dist(model.mygrid, tgrid)
    hops = forward_stations(model, target)
    if not hops:
        return out

    head = hops[0]
    out.append(A.hearing(
        head.call, 0.0,
        [f"target bears {brg:.0f} deg ({compass(brg)}) at {total:.0f} km",
         f"{head.call} is the furthest station that way we can raise "
         f"({head.km_from_us:.0f} km out, {head.progress:.0f} km of "
         f"progress, {head.km_to_target:.0f} km still to run)",
         "ask what IT hears -- that reaches past our own horizon"]))

    # Whatever it names, the next move is a relay through it; and if it
    # already hears the target, that IS the route.
    out.append(A.relay_ping(
        head.call, target, 0.0,
        [f"if {head.call} hears {target}, this is the whole route"]))

    for h in hops[1:max_hops]:
        out.append(A.relay_ping(
            h.call, target, 0.0,
            [f"next best forward station: {h.why}"]))

    if message:
        out.append(A.store_and_forward(
            head.call, target, message, 0.0,
            [f"hand it to {head.call}, the furthest station toward "
             f"{target} we can raise"]))
    return out
