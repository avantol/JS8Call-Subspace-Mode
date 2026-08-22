"""callsign.py — THE authority on callsign identity.

Andy 2026-08-21, watching AL0A become AL0A/P mid-exercise: "suddenly
going /P is an edge case, BTW." By his own standing rule a recurring
edge case is a DESIGN SMELL, and an audit proved it: five modules each
rolled their own `split("/")`, while four other sites compared
callsigns literally and silently stopped matching the moment a station
went portable. Identity was a fact with no owner, refined at the edges
-- exactly the shape [[feedback-one-thing-one-place]] warns about.

So: ONE authority. Every module asks here; nobody re-derives it.

THE TWO RULES, which are different and must not be conflated:

  IDENTITY  -- "is this the same operator?"  Compare base().
              AL0A and AL0A/P are one station: same operator, same
              antenna field, same person. Activity, routing, mesh
              edges, dedup and grid lookups all key on base.

  ADDRESSING -- "what do we put on the air?"  Use the EXACT call the
              station is using RIGHT NOW, affix included. It answers
              to AL0A/P, not AL0A ([[feedback-callsign-preserve-affixes]]);
              LiveMap.literal_call() resolves that from the freshest
              sighting.

Affixes are prefixes or suffixes (VE3/W1ABC, W1ABC/P, W1ABC/QRP). The
BASE is the longest token -- taking [0] blindly turns VE3/W1ABC into
"VE3", which is a country prefix, not a station.
"""
from __future__ import annotations


def base(call: str) -> str:
    """Operator identity with affixes stripped.

    AL0A/P    -> AL0A
    VE3/W1ABC -> W1ABC   (prefix form: the base is NOT token [0])
    W1ABC/QRP -> W1ABC
    """
    c = (call or "").upper().strip()
    if "/" not in c:
        return c
    parts = [p for p in c.split("/") if p]
    if not parts:
        return c
    # The station call is the longest token; affixes (P, M, QRP, VE3,
    # 7) are short. Ties keep the earlier token, which is the plain
    # SUFFIX case (W1ABC/P -> W1ABC).
    best = parts[0]
    for p in parts[1:]:
        if len(p) > len(best):
            best = p
    return best


def same(a: str, b: str) -> bool:
    """Same operator, regardless of portable/mobile affixes."""
    return bool(a) and bool(b) and base(a) == base(b)
