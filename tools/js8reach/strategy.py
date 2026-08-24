"""strategy.py — WHO DO I CALL, AND WHAT DO I ASK?

The two questions that were being answered in my head instead of in
code. plan.py answers "how do I reach a NAMED station"; candidates.py
ranks who is likely to answer. Neither asks what a transmission would
TEACH us, and neither checks what we already know -- so on 2026-08-23 I
was about to spend two minutes asking W5SDR what it hears while we
already held 99 of its edges, the freshest zero minutes old. Andy:
"you should be looking at the DB first for any hits."

So this module decides both halves together, and consults what we
already know first, because the cheapest probe is the one you do not
send.

============================ THE MODEL =============================
Rank actions by VALUE PER SECOND:

    score = P(reply) * information_gain / seconds

P(REPLY) -- what we know about the station answering:
    answered a broadcast query just now   strongest, it is awake NOW
    answered us before (history)          strong
    seen transmitting recently            weak; UNPROVEN as a
                                          discriminator -- it failed
                                          its control on 2026-08-23,
                                          1/3 against 1/3, and a
                                          rejected station answered
    never seen transmitting               weak evidence of ANYTHING;
                                          it measures our observation
                                          coverage, not the station

INFORMATION GAIN -- what the answer would add that we lack:
    @ALLCALL QUERY CALL   many answers for one transmission (13
                          responders measured), each proving the
                          responder hears US, unbiased by any choice
                          of mine. Highest gain available.
    HEARING? to a station
      we know little about  up to 4 RF-sourced edges where we have
                          none -- and RF is the one kind PSKReporter
                          cannot supply (#159, internet down).
    HEARING? to a station
      we already know     ~0. THE MAP DECIDES THIS, not a guess.
    SNR? anyone           ~0 beyond proof it replies: the dB describes
                          a path whose existence the reply already
                          demonstrated (Andy: "I don't think SNR? has
                          quite the payoff you think it does").

TERMINOLOGY: "@ALLCALL QUERY CALL" is the name of the thing. Earlier
notes call it a "sweep", which was my coinage and collides with
frequency sweeps and contest sweepstakes -- do not use it.
"""
from __future__ import annotations

import time

import forwarders
import history
from callsign import base, is_routable

# NO DATABASE HERE. The first version of this file opened
# ~/.config/JS8Call-grids.db to count what we know a station hears --
# which added a second source of truth, coupled this module to the
# app's schema, and read a file only as fresh as the last journal
# flush. The map dump already carries every hearer's HEARS list with
# per-edge SOURCE and AGE_S, so the same question is answerable from
# the LiveMap we already hold, and it is authoritative and current
# (Andy, 2026-08-23: "use the mapdump?").

# Measured on air, Normal speed (reference_js8reach): reply starts on
# the next period boundary and runs 2 frames.
T_QUERY_CALL = 97.0     # broadcast, many answers
T_HEARING = 112.0       # 3-4 reply frames
T_SNR = 70.0

# A station whose edges we already hold this many of, this recently,
# has nothing left to tell us that we would not learn by waiting.
KNOWN_ENOUGH = 12
KNOWN_FRESH_S = 30 * 60


class Knowledge:
    """How much we already know about what a station hears."""

    def __init__(self, edges, freshest_s, radio_edges):
        self.edges = edges
        self.freshest_s = freshest_s
        self.radio_edges = radio_edges

    @property
    def saturated(self) -> bool:
        return (self.edges >= KNOWN_ENOUGH
                and self.freshest_s is not None
                and self.freshest_s < KNOWN_FRESH_S)

    def gain(self) -> float:
        """0..1 -- how much a HEARING? reply would actually add."""
        if self.saturated:
            return 0.05
        if self.edges == 0:
            return 1.0
        # Falls off as we already know more, and a station we only know
        # through the internet is still worth asking on air.
        base_gain = max(0.1, 1.0 - self.edges / float(KNOWN_ENOUGH))
        if self.radio_edges == 0:
            base_gain = max(base_gain, 0.5)
        return base_gain


def knowledge(call: str, lm) -> Knowledge:
    """What the LIVE MAP already holds about what this station hears."""
    c = base(call).upper()
    for h in lm.hearing:
        if base(h.get("CALL") or "").upper() != c:
            continue
        edges = h.get("HEARS") or []
        ages = [float(e.get("AGE_S", -1)) for e in edges
                if float(e.get("AGE_S", -1)) >= 0]
        radio = sum(1 for e in edges
                    if (e.get("SOURCE") or "mqtt").lower() != "mqtt")
        return Knowledge(len(edges), min(ages) if ages else None, radio)
    return Knowledge(0, None, 0)


class Action:
    def __init__(self, text, seconds, score, why, target=""):
        self.text = text
        self.seconds = seconds
        self.score = score
        self.why = why
        self.target = target

    def __repr__(self):
        return f"{self.text!r}  score={self.score:.3f} ({self.seconds:.0f}s)"


def _p_reply(call: str, now: float, answered_broadcast: bool,
             transmitting: bool) -> tuple:
    c = base(call)
    if answered_broadcast:
        return 0.9, "answered a broadcast query just now -- awake and replying"
    if history.answered_recently(c, now):
        return 0.8, "has answered us before"
    if forwarders.status(c, now) == "forwarded":
        return 0.6, "relays for us, so it is running and responsive"
    if transmitting:
        return 0.35, ("decoded transmitting recently -- WEAK, this "
                      "discriminator failed its control 2026-08-23")
    return 0.25, "no evidence either way about it answering"


def next_actions(lm, answered_broadcast=(), exclude=(), avoid=(),
                 now=None, limit=5) -> list:
    """The best things to put on the air next, best value first."""
    now = now or time.time()
    band = lm.band
    skip = {base(c) for c in exclude} | {base(lm.my_call)}
    avoid_up = {base(c) for c in avoid}
    recent = {base(c) for c in answered_broadcast}
    tx = lm.transmitters()
    out = []

    # ONE broadcast query, always a candidate. It is the only action
    # whose answers are unbiased by my choice of who to ask, and it
    # returns many for the price of one.
    unknown = [c for c, _a, _s in lm.hearers_of(lm.my_call, 3600)
               if base(c) not in skip]
    probe_for = unknown[0] if unknown else lm.my_call
    out.append(Action(
        f"@ALLCALL QUERY CALL {base(probe_for)}?", T_QUERY_CALL,
        0.9 * 1.0 / T_QUERY_CALL,
        ["one transmission, many answers -- 13 responders measured "
         "2026-08-23",
         "every responder proves it hears US, and the sample is not "
         "chosen by me, so it is the only unbiased evidence we get",
         "answers are RF-sourced, so this still works with the "
         "internet down (#159)"]))

    for call, age, snr in lm.hearers_of(lm.my_call, 3600):
        c = base(call)
        if c in skip or c in avoid_up or not is_routable(c):
            continue
        if history.reached_but_silent(c, now, hears_us=True):
            continue
        p, p_why = _p_reply(c, now, c in recent, c in tx)
        k = knowledge(c, lm)
        g = k.gain()
        why = [p_why,
               f"hears us {snr:+d} dB, {age / 60:.0f} min ago"]
        if k.saturated:
            why.append(f"we already hold {k.edges} of its edges, "
                       f"freshest {int(k.freshest_s) // 60} min -- "
                       f"asking would re-learn what we have")
        else:
            why.append(f"we hold only {k.edges} of its edges "
                       f"({k.radio_edges} RF-sourced) -- a reply "
                       f"fills a real gap")
        out.append(Action(f"{lm.literal_call(c)} HEARING?", T_HEARING,
                          p * g / T_HEARING, why, target=c))
    out.sort(key=lambda a: -a.score)
    return out[:limit]
