"""path.py — search TribbleNet for a route, instead of picking one.

TribbleNet is the live who-hears-whom mesh (operator's name,
2026-08-21). This module treats it as a graph and searches it.

The lesson that cost an hour of airtime on 2026-08-21: I kept choosing
relays by GEOGRAPHY (W1WT was 993 km from AL0A -- perfect on a map, and
never once observed HEARING anybody) when the only thing that delivers
traffic is the DELIVERY LEG: the target must hear the relay. Two
relays forwarded correctly and neither produced an answer, because
neither was a station AL0A could hear. A search over live edges finds
that in milliseconds and costs no airtime.

DIRECTION IS THE WHOLE TRICK. A map edge means "A HEARS B". To DELIVER
we transmit along the reverse of that edge: if A heard B, then B can
probably reach A. So the delivery walk uses the REVERSED graph, and the
goal set is {X : target hears X}.

A negative result is a real result: "no chain within N hops" means stop
transmitting, and is worth far more than another hopeful relay. Say it
as "no path VISIBLE" though -- the map holds ~1 h and is emptied by a
restart (#168), so absence of evidence is thin here.
"""
from __future__ import annotations

import collections

from callsign import base


def build(lm, within_s: float = 3600) -> dict:
    """A HEARS B, from both live sources.

    Phantom-safe, but NOT by blanket-dropping SNR-less edges -- that
    was a bug (2026-08-21): our OWN decodes land in the hearing store
    with SNR -99, so the filter threw away the single most
    authoritative evidence we have and hid a complete live route
    (WM8Q>KF0DRT>AD8MM>AL0A, every leg under 8 min old). Use the same
    rule as live.hearers_of: a -99 edge counts when the HEARD station
    has been seen transmitting somewhere, because then something was
    really decoded; it is discarded only when the heard call has never
    keyed, which is the signature of "A merely addressed B" (#167).
    """
    hears: dict = collections.defaultdict(dict)
    keys_up = lm.transmitters()
    for s in lm.spots:
        if s.call and s.heard_by and 0 <= s.age_s <= within_s:
            a, b = base(s.heard_by), base(s.call)
            if a != b and (b not in hears[a] or s.age_s < hears[a][b][0]):
                hears[a][b] = (s.age_s, s.snr)
    for h in lm.hearing:
        a = base(h.get("CALL") or "")
        for e in h.get("HEARS") or []:
            b = base(e.get("CALL") or "")
            age = float(e.get("AGE_S", -1))
            snr = int(e.get("SNR", -99))
            if not (a and b and a != b and 0 <= age <= within_s):
                continue
            if snr <= -99 and b not in keys_up:
                continue          # never keyed => addressed, not heard (#167)
            if b not in hears[a] or age < hears[a][b][0]:
                hears[a][b] = (age, snr)
    return hears


def delivery_chains(lm, target: str, max_hops: int = 4,
                    within_s: float = 3600) -> list:
    """Chains us -> ... -> X where the TARGET hears X, shortest first."""
    hears = build(lm, within_s)
    t, me = base(target), base(lm.my_call)
    goals = set(hears.get(t, {}))
    if not goals:
        return []
    # A HOP MUST BE ABLE TO TRANSMIT. The map is thick with
    # PSKReporter monitors -- live receivers that never key (K2AY,
    # N9EAT, KF0LCJ, 2026-08-21). They generate excellent EVIDENCE and
    # can carry NOTHING, so routing through one is guaranteed silence:
    # KF0LCJ heard AL0A at -8 and us at -6, and no forward was ever
    # possible. Only the endpoints may be non-transmitters; every
    # intermediate hop must be in transmitters().
    keys_up = lm.transmitters()
    radj: dict = collections.defaultdict(set)
    for a, vs in hears.items():
        for b in vs:
            radj[b].add(a)        # a heard b => b can probably reach a
    q = collections.deque([(me, [me])])
    seen = {me}
    out = []
    while q:
        n, path = q.popleft()
        if len(path) > max_hops:
            continue
        for nxt in radj.get(n, ()):
            if nxt in seen:
                continue
            # nxt is an intermediate hop unless it is a goal endpoint
            if nxt not in goals and nxt not in keys_up:
                continue          # receive-only: cannot forward
            p2 = path + [nxt]
            if nxt in goals:
                out.append(p2)
            seen.add(nxt)
            q.append((nxt, p2))
    return sorted(out, key=len)


def describe(lm, target: str, max_hops: int = 4) -> str:
    t = base(target)
    hears = build(lm)
    goals = hears.get(t, {})
    lines = [f"{t} HEARS (delivery targets):"]
    if not goals:
        lines.append("   nothing on the live map -- no delivery leg "
                     "is known, so any relay is a guess")
    for b, (age, snr) in sorted(goals.items(), key=lambda kv: kv[1][0]):
        lines.append(f"   {b:9s} {snr:+4d} dB  {age / 60:5.1f} min ago")
    chains = delivery_chains(lm, target, max_hops)
    lines.append("")
    if chains:
        lines.append("delivery chains, shortest first:")
        for p in chains[:6]:
            lines.append("   " + " > ".join(p) + f"   ({len(p) - 1} hops)")
    else:
        lines.append(f"NO delivery chain within {max_hops} hops "
                     f"THROUGH STATIONS THAT PUBLISH what they hear.")
        lines.append("This is a WEAK negative, for three reasons:")
        lines.append("  1. PSKReporter uploading is OPTIONAL (Andy, "
                     "2026-08-21). A station that never reports has an "
                     "empty hearer list no matter how well it hears, "
                     "so it can never appear as a relay here -- absence "
                     "is a fact about PUBLISHING, not about receiving.")
        lines.append("  2. The map holds ~1 h and a restart empties it "
                     "(#168).")
        lines.append("  3. HEARING? replies are the cure: they make a "
                     "non-reporting station's ears visible. Probe the "
                     "best-placed silent candidates rather than "
                     "concluding no route exists.")
    return "\n".join(lines)


def _main() -> int:
    import argparse
    import sys
    from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from live import LiveMap
    ap = argparse.ArgumentParser(description="Search the live mesh")
    ap.add_argument("target")
    ap.add_argument("--max-hops", type=int, default=4)
    a = ap.parse_args()
    print(describe(LiveMap.fetch(), a.target, a.max_hops))
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
