"""forwarders.py — who actually RELAYS, remembered across sessions.

Relaying is a property of the STATION, not of the path, and nothing in
the mesh reveals it: there is no RELAY? query and no reply exposes the
setting. It can only be learned by asking a station once and watching
whether a forward appears -- which makes it expensive to learn and
therefore worth never forgetting.

The 2026-08-22 session is why this file exists. Nine stations were
asked, chosen for good geometry on both legs; two forwarded. Ranking
routes on path evidence alone treated all nine as equal, so the planner
kept recommending stations that had already declined while a known
forwarder sat unused.

THREE OUTCOMES, deliberately not two (Andy's distinction):

    forwarded   we SAW the *DE* forward. Stable, keep forever.
    declined    asked, stayed silent. NOT proof of intent -- it may be
                a default nobody chose, or simply nobody home. So it
                DECAYS: a decline ages out and the station becomes
                unknown again, because "no operator at the time" is not
                a property of the station.
    unknown     never asked.

Deliberate relay-off, an unchosen default, and an absent operator all
look identical on air. Only the first is permanent, and we cannot tell
them apart -- so the safe rule is: remember success forever, remember
failure only for a while.
"""
from __future__ import annotations

import json
import os
from pathlib import Path

from callsign import base

# Beside the other js8reach state, not in the repo.
STORE = Path(os.path.expanduser("~/.config/js8reach-forwarders.json"))

# A decline older than this stops counting. Long enough to avoid
# re-asking the same dead station all evening, short enough that a
# station which was merely unattended gets another chance tomorrow.
DECLINE_TTL_S = 12 * 3600

# Risk multipliers applied to a hop THROUGH this station.
RISK_FORWARDED = 0.55   # proven; beats a stronger-signal unknown
RISK_UNKNOWN = 1.0
RISK_DECLINED = 2.2     # not excluded -- conditions and operators change


def _load() -> dict:
    try:
        return json.loads(STORE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"forwarded": {}, "declined": {}}


def _save(d: dict) -> None:
    try:
        STORE.parent.mkdir(parents=True, exist_ok=True)
        STORE.write_text(json.dumps(d, indent=1, sort_keys=True),
                         encoding="utf-8")
    except OSError:
        pass          # a missing record must never break planning


def record_forward(call: str, when_epoch: float) -> None:
    """We saw this station forward. Permanent."""
    d = _load()
    c = base(call)
    if not c:
        return
    d.setdefault("forwarded", {})[c] = when_epoch
    d.get("declined", {}).pop(c, None)   # success overrides past silence
    _save(d)


def record_decline(call: str, when_epoch: float) -> None:
    """Asked, nothing came back. Decays -- see DECLINE_TTL_S."""
    d = _load()
    c = base(call)
    if not c or c in d.get("forwarded", {}):
        return                            # never downgrade a known one
    d.setdefault("declined", {})[c] = when_epoch
    _save(d)


def status(call: str, now_epoch: float) -> str:
    """'forwarded' | 'declined' | 'unknown'."""
    d = _load()
    c = base(call)
    if c in d.get("forwarded", {}):
        return "forwarded"
    t = d.get("declined", {}).get(c)
    if t is not None and (now_epoch - t) < DECLINE_TTL_S:
        return "declined"
    return "unknown"


def risk(call: str, now_epoch: float) -> float:
    return {"forwarded": RISK_FORWARDED,
            "declined": RISK_DECLINED}.get(status(call, now_epoch),
                                           RISK_UNKNOWN)
