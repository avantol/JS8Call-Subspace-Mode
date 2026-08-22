"""window.py — WHEN is the path open?

For DX the useful answer is often a TIME, not a station. A target
12,000 km away is not "unreachable" — the path simply is not open at
this hour. Ranking actions cannot express that; this can.

Evidence: the UTC-hour distribution of our own decodes of stations in
the target's REGION (not just the target, which for real DX may have
only a handful of decodes). VK2XOR is the motivating case: 4 decodes,
all on one day, 07:26-10:58Z — a grey-line opening, invisible to any
per-station average.
"""

from __future__ import annotations

import datetime as dt

import grid as G
import intel


def region_hours(db, target_grid: str, radius_km: float = 2000.0
                 ) -> tuple[list[int], list[str], list[int]]:
    """Decode counts per UTC hour for stations near `target_grid`, the
    contributing calls, and TOTAL decodes per hour from all stations.

    The totals are essential, not decoration. Raw counts measure when
    WE were on the air, not when the path was open — we decode ~7x more
    of everything at 02Z than at 17Z simply because that is when we
    operate. Only the SHARE (region / total) says anything about this
    particular path (operator: "suppose i never TRIED?", 2026-08-21).
    """
    hours = [0] * 24
    totals = [0] * 24
    for r in db.execute("SELECT CAST(strftime('%H', ts, 'unixepoch') AS INT)"
                        " h, COUNT(*) n FROM sightings GROUP BY h"):
        totals[r["h"]] = r["n"]
    contributors: list[str] = []
    for row in db.execute("SELECT call, grid FROM stations "
                          "WHERE grid IS NOT NULL AND grid != ''"):
        g = row["grid"]
        if not G.valid(g):
            continue
        try:
            if G.distance_km(target_grid, g) > radius_km:
                continue
        except ValueError:
            continue
        n = 0
        for r in db.execute("SELECT ts FROM sightings WHERE call = ?",
                            (row["call"],)):
            hours[dt.datetime.fromtimestamp(
                r["ts"], dt.timezone.utc).hour] += 1
            n += 1
        if n:
            contributors.append(f"{row['call']}({n})")
    return hours, contributors, totals


def best_windows(hours: list[int], top: int = 4) -> list[int]:
    ranked = sorted(range(24), key=lambda h: -hours[h])
    return sorted(h for h in ranked[:top] if hours[h] > 0)


def describe(hours: list[int], contributors: list[str],
             now_hour: int, totals: list[int] | None = None) -> list[str]:
    """Report the path's hour profile as a SHARE of everything we heard
    that hour. A raw count peak is our own operating schedule."""
    total = sum(hours)
    if not total:
        return ["no station in this region has ever been decoded here — "
                "nothing to schedule against"]
    out = [f"{total} decodes from {len(contributors)} station(s) in the "
           f"region: {', '.join(contributors[:6])}"]
    if not totals:
        out.append("(no hourly totals available — raw counts would "
                   "measure our operating hours, not the path)")
        return out

    shares = [(hours[h] / totals[h]) if totals[h] >= 200 else None
              for h in range(24)]
    have = [(h, sh) for h, sh in enumerate(shares) if sh is not None]
    if not have:
        out.append("too little traffic at any hour to separate the path "
                   "from our own operating pattern")
        return out
    avg = sum(sh for _, sh in have) / len(have)
    spread = max(sh for _, sh in have) - min(sh for _, sh in have)
    best = sorted(have, key=lambda x: -x[1])[:3]

    if spread < 0.15:
        out.append(f"this region is a steady {100 * avg:.0f}% of "
                   f"everything we hear, at EVERY hour we operate "
                   f"(spread {100 * spread:.0f} points) — our data shows "
                   f"NO time-of-day preference for this path")
        out.append("any apparent hourly 'peak' in raw counts is just "
                   "when we are on the air")
    else:
        span = ", ".join(f"{h:02d}Z({100 * sh:.0f}%)" for h, sh in best)
        out.append(f"strongest share of our traffic at: {span} "
                   f"(average {100 * avg:.0f}%)")
    sh_now = shares[now_hour]
    if sh_now is None:
        out.append(f"NOW is {now_hour:02d}Z — we have too few decodes at "
                   f"this hour to say anything; that is a gap in OUR "
                   f"operating, not evidence the path is shut")
    else:
        rel = 100 * sh_now / avg if avg else 0
        out.append(f"NOW is {now_hour:02d}Z — this region is "
                   f"{100 * sh_now:.0f}% of what we hear at this hour "
                   f"({rel:.0f}% of its own average)")
    return out
