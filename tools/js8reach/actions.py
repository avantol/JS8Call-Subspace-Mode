"""actions.py — the probe catalogue and its cost model.

Costs mirror the app's own timing math so the planner and the radio
agree:

  period P                 15 s at Normal        (JS8Submode.cpp:122)
  TX is boundary-locked    lateThreshold = 0     (mainwindow.cpp:3086)
                           => mean align wait P/2
  reply decision instant   B0 + (2 + replyFrames) * P
                                                 (ChunkedArq.h:276-322)
  relay hop                3 frames = 45 s each direction
                                                 (mainwindow.h:1283)

so  t = P/2 + txFrames*P + (2 + replyFrames)*P + hops*2*45

Frame counts come from the wire forms audited in Varicode.cpp: a bare
directed command is ONE frame; a command with a body carries a 16-bit
checksum and needs two; a HEARING reply listing four calls runs 3-4.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional

PERIOD_S = 15.0                  # Normal
RELAY_HOP_S = 45.0               # 3 frames per hop, one direction


def rtt(tx_frames: int, reply_frames: int, relay_hops: int = 0) -> float:
    """Expected wall-clock from 'decide to send' to 'reply decided'."""
    return (PERIOD_S / 2.0
            + tx_frames * PERIOD_S
            + (2 + reply_frames) * PERIOD_S
            + relay_hops * 2 * RELAY_HOP_S)


@dataclass
class Action:
    kind: str                    # ping | broadcast | hearing | relay | sandf
    text: str                    # exactly what goes on the air
    t: float                     # expected seconds to a decision
    p: float                     # P(this action achieves contact)
    hops: int                    # relay hops used
    why: list[str]               # evidence trail, shown to the operator
    target: Optional[str] = None
    via: Optional[str] = None
    info: bool = False           # discovery action: p is VOI-derived
    on_success: Optional[Callable] = None

    @property
    def index(self) -> float:
        """p/t — the quantity the optimal ordering sorts on."""
        return self.p / self.t if self.t > 0 else 0.0


# ---- catalogue -------------------------------------------------------
# Each builder returns an Action with its measured cost. p is filled in
# by the planner from the model.

def ping(target: str, p: float, why: list[str]) -> Action:
    """`T SNR?` — the cheapest possible proof of a two-way path: one
    TX frame, a one-frame reply, and the answer carries their copy of
    our signal while our decode carries ours of theirs."""
    return Action("ping", f"{target} SNR?", rtt(1, 1), p, 0, why,
                  target=target)


def status(target: str, p: float, why: list[str]) -> Action:
    """`T STATUS?` — reply embeds <MYIDLE> and <MYVERSION>: tells us
    whether a human is at the keyboard and whether they run our build."""
    return Action("status", f"{target} STATUS?", rtt(1, 3), p, 0, why,
                  target=target)


def broadcast_query_call(target: str, p: float, why: list[str]) -> Action:
    """`@ALLCALL QUERY CALL T?` — one transmission, N parallel answers.
    Every station that has heard T replies "YES +snr (age)" on its OWN
    offset, so the full-passband decoder reads them concurrently. This
    evaluates the entire relay pool for the price of one probe."""
    return Action("broadcast", f"@ALLCALL QUERY CALL {target}?",
                  rtt(2, 2), p, 0, why, target=target, info=True)


def hearing(neighbour: str, p: float, why: list[str]) -> Action:
    """`N HEARING?` — N's four most-recently-heard stations. For the
    grid case this is geographic gossip: N's neighbours are near N."""
    return Action("hearing", f"{neighbour} HEARING?", rtt(1, 4), p, 0,
                  why, target=neighbour, info=True)


def relay_ping(via: str, target: str, p: float, why: list[str]) -> Action:
    """`R>T SNR?` — contact through one hop. Self-diagnosing: R's
    forward is audible to us at ~110 s, proving R relays even when T
    never answers, so a failure still buys knowledge."""
    return Action("relay", f"{via}>{target} SNR?", rtt(2, 1, relay_hops=1),
                  p, 1, why, target=target, via=via)


def store_and_forward(via: str, target: str, text: str, p: float,
                      why: list[str]) -> Action:
    """`R>T MSG TO:` — hand the traffic to a station that hears T; the
    app's store-and-forward delivers when T next appears. Not contact,
    but delivery, and it costs us nothing further."""
    return Action("sandf", f"{via}>{target} MSG TO:{target} {text}",
                  rtt(2, 2, relay_hops=1), p, 1, why,
                  target=target, via=via)
