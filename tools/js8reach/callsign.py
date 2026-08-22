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

import re


def _looks_like_a_call(token: str) -> bool:
    """Could this token be a station call, as opposed to an affix?

    Two properties every amateur call has and no common affix has:
    it contains a digit, and it ends in a letter.

        P, M, MM, QRP, AG, FULLERTON  -- no digit
        VE3, KH6, 7, 9                -- end in a digit

    This is deliberately loose. It only has to separate a call from the
    affixes sitting beside it, not decide whether the call is valid --
    that is is_routable()'s job.
    """
    return any(c.isdigit() for c in token) and token[-1:].isalpha()


def base(call: str) -> str:
    """Operator identity with affixes stripped.

    AL0A/P           -> AL0A
    VE3/W1ABC        -> W1ABC   (prefix form: the base is NOT token [0])
    W1ABC/QRP        -> W1ABC
    W6MFB/FULLERTON  -> W6MFB   (affix LONGER than the call)

    Pick the token that looks like a call, not the longest one. The
    longest-token rule was wrong: it assumed affixes are short, and
    silently returned "FULLERTON" for W6MFB/FULLERTON -- so that
    station matched nothing, deduped against nothing, and could not be
    routed to. Length is a hint about affixes; it is not what makes
    something a call.
    """
    c = (call or "").upper().strip()
    if "/" not in c:
        return c
    parts = [p for p in c.split("/") if p]
    if not parts:
        return c
    # Prefer tokens shaped like a call. Among several (VP2E/W1ABC), the
    # longer one is the station and the shorter is the country prefix.
    candidates = [p for p in parts if _looks_like_a_call(p)] or parts
    best = candidates[0]
    for p in candidates[1:]:
        if len(p) > len(best):
            best = p
    return best


def same(a: str, b: str) -> bool:
    """Same operator, regardless of portable/mobile affixes."""
    return bool(a) and bool(b) and base(a) == base(b)


# An amateur call: a prefix of one to three characters holding at least
# one letter, then a digit, then a suffix of one to four letters.
# Backtracking covers both 4X4ABC (prefix "4X") and 3DA0XX (prefix "3DA").
_CALL_RE = re.compile(r'^[A-Z0-9]{1,3}[0-9][A-Z]{1,4}$')


def is_amateur_call(call: str) -> bool:
    """Is this a licensed amateur call, as opposed to some other ID?

    The PSKReporter feed carries plenty of things that are not amateur
    calls: SWL and club listener IDs (DIG647, SWL45, F/SWL/PRIVAS,
    FSWL117), 11-metre and freeband IDs (13RF1146, 21GK4661, 109HA3651),
    and outright pirates (20DR3VIL, DE398WBRX, PL6W702WI -- identified
    by the operator, 2026-08-22).
    """
    b = base(call)
    return bool(b) and bool(_CALL_RE.match(b)) and any(c.isalpha()
                                                       for c in b[:-1])


def is_receive_only(call: str) -> bool:
    """Is this a receiving node that never transmits?

    A hyphen suffix marks one: K1RA-4 and KM3T-3 are RBN skimmers,
    WH6HJH-RX is a monitor. The operator behind them is real -- K1RA is
    a licensed station -- but the NODE only listens, so it can never
    carry traffic.
    """
    return "-" in (call or "")


def is_routable(call: str) -> bool:
    """Can traffic be sent THROUGH this station?

    Two independent reasons a station cannot carry traffic, and they
    are not the same question:

      not an amateur call -- SWL IDs never key at all; pirates and
          freeband stations do transmit, but are not something to route
          a licensed operator's traffic through either.
      receive-only node   -- a real call, but this node only listens.

    Note what this does NOT decide: whether the station is a useful
    OBSERVER. "An SWL in JN97 heard WD4KAV" is real evidence about
    WD4KAV's propagation even though that listener can never relay.
    Routing and evidence are separate uses; this answers only routing.
    """
    return is_amateur_call(call) and not is_receive_only(call)
