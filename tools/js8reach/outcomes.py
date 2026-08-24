"""outcomes.py — learn from what actually happens on the air.

forwarders.py and history.py hold what we know; nothing filled them.
All of 2026-08-22 I typed record_forward() and record_silence() by
hand after reading each result, which means the engine learned only
when I remembered to teach it -- and the moment anyone else drives it,
or I miss one, it stops learning entirely.

Everything needed is already in the decode stream. A relay forward is
visible on air as

    W0BYU: N9WCW> SNR? *DE* WM8Q

-- the relay's callsign, the destination, and OUR call as originator.
That single line proves three separate facts worth keeping: W0BYU
relays, our traffic reached toward N9WCW, and the leg WM8Q->W0BYU
works. A reply from the target proves the whole path and that its
autoreply is on.

So: wrap an attempt in a Session, feed it every directed decode, and
close it. The records fill themselves.

DELIBERATELY NOT RECORDED HERE: anything about stations we did not
address. Passive relay traffic between other operators is visible and
tempting, but "W7SUA relays for KG7RXU" says nothing about whether it
would relay for US -- blocking is per-callsign and invisible (Andy,
2026-08-22). Learn only from our own attempts.
"""
from __future__ import annotations

import re
import time

import forwarders
import history
from callsign import base

# "…> SNR? *DE* WM8Q" -- the relay marker carries the ORIGINATOR, which
# is how a forward of OUR traffic is told from any other relay on the
# band.
_DE = re.compile(r'\*DE\*\s+([A-Z0-9/]+)', re.I)


class Session:
    """One attempt: a target, and the relay chain we asked to carry it."""

    def __init__(self, my_call: str, target: str, path=None):
        self.me = base(my_call)
        # A BROADCAST HAS NO TARGET. "@ALLCALL QUERY CALL X?" is
        # addressed to everyone, so there is no station whose silence
        # means anything -- recording one wrote a history entry for a
        # station named @ALLCALL (2026-08-23). Responders are still
        # learned from individually; only the phantom target goes.
        target = (target or "").strip()
        self.target = "" if target.startswith("@") else (
            base(target) if target else "")
        # Path as sent, minus the destination: the stations being ASKED
        # to relay. Only these can be credited or debited.
        self.relays = [base(c) for c in (path or []) if base(c) != self.target]
        self.answered = False
        self.forwarded = set()

    def saw(self, frm: str, to: str, text: str) -> str:
        """Feed one directed decode. Returns what was learned, or ''."""
        f, t = base(frm), base((to or "").rstrip(">"))
        txt = text or ""

        # A FORWARD OF OURS: the *DE* originator is us. Not merely a
        # relay -- a relay working on our behalf.
        m = _DE.search(txt)
        if m and base(m.group(1)) == self.me and f and f != self.me:
            if f not in self.forwarded:
                self.forwarded.add(f)
                forwarders.record_forward(f, time.time())
                if self.target:
                    history.record_delivery(self.target, f, time.time())
                return f"{f} forwarded for us"
            return ""

        # THE TARGET ANSWERED: proves the path both ways and that its
        # autoreply is enabled.
        if self.target and f == self.target and t == self.me:
            if not self.answered:
                self.answered = True
                history.record_answer(self.target, time.time())
                return f"{f} ANSWERED"
        return ""

    def close(self) -> list:
        """Attempt over. Debit the silent, record the outcome."""
        now = time.time()
        learned = []
        for r in self.relays:
            if r not in self.forwarded:
                # "did not forward FOR US" -- never "does not relay".
                # It decays; see forwarders.DECLINE_TTL_S.
                forwarders.record_decline(r, now)
                learned.append(f"{r} did not forward")
        if self.target and not self.answered:
            history.record_silence(self.target, now)
            learned.append(f"{self.target} silent")
        return learned
