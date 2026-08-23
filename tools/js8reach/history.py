"""history.py — what we already tried against a TARGET, and what came back.

forwarders.py answers "does this station relay". This answers a
different question that kept getting confused with it: "have we already
got traffic INTO this target, and did the target care".

Two failures on 2026-08-22 motivated it, both of which had me
recommending more radio time for no information:

  * After KE0ZDH forwarded to K2AY, the planner kept nominating KE0ZDH
    -- correctly, since it was now a proven forwarder, but pointlessly,
    because that exact delivery had already happened and the target had
    stayed silent. The relay was not the problem.
  * The planner has only ever had two outcomes, "route" and "no route".
    A target we can demonstrably REACH but which never answers is
    neither, and it is the most common outcome of the night (N9WCW,
    KG4UHM/6, KG6NFJ, K2AY). Calling it "no route" hides that delivery
    works; calling it a route invites another identical attempt.

So a target carries three facts: which relays have DELIVERED to it,
whether it has ever ANSWERED, and when we last heard nothing.

Everything expires. Propagation and operators both change, and a
target that ignored us an hour ago deserves another try tomorrow --
this file must never become a permanent blacklist.
"""
from __future__ import annotations

import json
import os
from pathlib import Path

from callsign import base

STORE = Path(os.path.expanduser("~/.config/js8reach-history.json"))

# How long a delivery proof stays interesting. Long enough to stop us
# re-running the same relay all evening, short enough that the band
# changing makes it irrelevant again.
DELIVERY_TTL_S = 2 * 3600
# How long "reached but silent" suppresses further calling.
SILENT_TTL_S = 2 * 3600


def _load() -> dict:
    try:
        return json.loads(STORE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _save(d: dict) -> None:
    try:
        STORE.parent.mkdir(parents=True, exist_ok=True)
        STORE.write_text(json.dumps(d, indent=1, sort_keys=True),
                         encoding="utf-8")
    except OSError:
        pass          # history is an optimisation, never a dependency


def _entry(d: dict, target: str) -> dict:
    return d.setdefault(base(target),
                        {"delivered": {}, "silent_at": 0.0,
                         "answered_at": 0.0})


def record_delivery(target: str, via: str, when: float) -> None:
    """A relay was HEARD forwarding our traffic to this target."""
    d = _load()
    _entry(d, target)["delivered"][base(via)] = when
    _save(d)


def record_silence(target: str, when: float) -> None:
    d = _load()
    _entry(d, target)["silent_at"] = when
    _save(d)


def record_answer(target: str, when: float) -> None:
    """The target itself replied. Clears the silence -- it works."""
    e = _entry(d := _load(), target)
    e["answered_at"] = when
    e["silent_at"] = 0.0
    _save(d)


def delivered_relays(target: str, now: float) -> set:
    """Relays already proven to put our traffic into this target."""
    e = _load().get(base(target))
    if not e:
        return set()
    return {c for c, t in e.get("delivered", {}).items()
            if now - t < DELIVERY_TTL_S}


def reached_but_silent(target: str, now: float,
                       hears_us: bool = False) -> bool:
    """Reach is PROVEN, the target has not answered, and it is recent.

    Two independent proofs of reach, and the second was missing:

      a relay was seen FORWARDING to the target   -- delivered_relays()
      the target itself REPORTS HEARING US        -- hears_us

    The second is the stronger of the two. A relay forward only shows
    our traffic left in the right direction; a first-hand "target hears
    us at +3" says it arrived. Counting only relay forwards made N7ER
    invisible to this check (2026-08-22): called twice, silent twice,
    reporting us at +3 dB the whole time, while the earlier relay proof
    had aged out -- so the planner kept recommending it.

    Caller supplies `hears_us` because the live mesh, not this file,
    knows who currently hears us.
    """
    e = _load().get(base(target))
    if not e:
        return False
    if not (hears_us or delivered_relays(target, now)):
        return False
    if e.get("answered_at", 0.0) > e.get("silent_at", 0.0):
        return False                      # it has answered since
    return (now - e.get("silent_at", 0.0)) < SILENT_TTL_S


# An answer is the strongest proof a return path exists -- stronger than
# any inferred route, because it already happened. Kept short: paths
# close, and a contact an hour ago says little about now.
ANSWERED_TTL_S = 45 * 60


def answered_recently(target: str, now: float) -> bool:
    """Did this station reply to us within the window?"""
    e = _load().get(base(target))
    if not e:
        return False
    return (now - e.get("answered_at", 0.0)) < ANSWERED_TTL_S
