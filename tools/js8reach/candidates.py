"""candidates.py — WHO SHOULD I CALL? (the question plan.py never asked)

plan.py answers "how do I reach X". Nothing answered "which X", so on
2026-08-23 I did that part by hand with throwaway one-liners while
claiming to be testing the engine. The rule that produced 9 contacts
from 10 attempts therefore lived nowhere in the code, and none of that
night's learning could reach the planner. Andy: "are you following the
algorithm as baked into python code, or running it manually here? we
are supposed to be testing the algorithm."

So the rule goes here, where it can be run, criticised and be wrong in
public.

THE RULE, and why it beat everything else tried that night:

    rank by P(THEY ANSWER), not by P(we reach them)

Reach was almost never the constraint. Relays forwarded, targets
received us, and stayed silent -- eleven confirmed forwarders and a
long row of silent targets. What decides an answer is whether the far
end's AUTOREPLY is on, and the best available evidence for that is
that the station has been DECODED TRANSMITTING in the last few
minutes: its software is demonstrably answering things.

    1. it hears us                    necessary
    2. seen transmitting recently     the discriminator
    3. it has answered us before      strongest, keep it
    4. never seen transmitting        DEPRIORITISE, never exclude --
                                      that measures our observation
                                      coverage, not the station

KNOWN BIAS, stated because it would otherwise flatter this file:
selecting on the rule and then scoring the result measures the FILTER,
not the population. A 9-of-10 hit rate says nothing about the stations
the rule rejects. `control_sample()` exists to fix that: it returns
rule-REJECTED stations, deliberately, so the false-negative rate can
be measured instead of assumed.
"""
from __future__ import annotations

import time

import forwarders
import history
from callsign import base, is_routable

# "Recently" for the transmitting test. Long enough to survive a quiet
# minute, short enough that it still means "on the air now".
ACTIVE_SECS = 15 * 60
# Their report of us older than this is stale evidence of a live path.
HEARS_US_SECS = 60 * 60


class Candidate:
    def __init__(self, call, snr_to_us, hears_age, transmitting,
                 answered_before, score, why):
        self.call = call
        self.snr_to_us = snr_to_us
        self.hears_age = hears_age
        self.transmitting = transmitting
        self.answered_before = answered_before
        self.score = score
        self.why = why

    def __repr__(self):
        return (f"{self.call} score={self.score:.2f} "
                f"snr={self.snr_to_us:+d} age={self.hears_age/60:.0f}m")


def _score(snr, age_s, transmitting, answered, avoid):
    """Higher is better. Deliberately simple -- these weights are a
    STARTING POINT to be fitted from recorded outcomes, not physics."""
    if avoid:
        return -1.0
    s = 1.0
    s *= 2.5 if transmitting else 0.4     # the discriminator
    if answered:
        s *= 2.0                          # it has literally answered before
    # Freshness of their report of us matters more than its strength:
    # propagation moves faster than the gap between +2 and +13.
    s *= max(0.2, 1.0 - (age_s / HEARS_US_SECS))
    s *= 1.0 + (max(-25, min(20, snr)) + 25) / 90.0
    return s


def rank(lm, avoid=(), exclude=(), now=None) -> list:
    """Stations worth calling, best first."""
    now = now or time.time()
    avoid_up = {base(c) for c in avoid}
    skip = {base(c) for c in exclude} | {base(lm.my_call)}
    tx = lm.transmitters()
    out = []
    for call, age, snr in lm.hearers_of(lm.my_call, HEARS_US_SECS):
        c = base(call)
        if c in skip or not is_routable(c):
            continue
        # Already reached and silent -- calling again re-tests a leg we
        # proved. history knows; ask it rather than remembering.
        if history.reached_but_silent(c, now, hears_us=True):
            continue
        transmitting = c in tx
        answered = history.answered_recently(c, now)
        why = []
        why.append(f"hears us {snr:+d} dB, {age/60:.0f} min ago")
        why.append("decoded transmitting recently -- its software is "
                   "answering things" if transmitting else
                   "NOT seen transmitting -- may simply be poorly "
                   "observed, so ranked down rather than dropped")
        if answered:
            why.append("has answered us before")
        if forwarders.status(c, now) == "forwarded":
            why.append("also relays for us")
        out.append(Candidate(call, snr, age, transmitting, answered,
                             _score(snr, age, transmitting, answered,
                                    c in avoid_up),
                             why))
    out.sort(key=lambda x: -x.score)
    return [c for c in out if c.score >= 0]


def control_sample(lm, n=3, now=None) -> list:
    """Stations the rule REJECTS, for measuring its false negatives.

    Without these the hit rate is self-congratulatory: it only ever
    reports on stations chosen for being likely. Spend a few attempts
    here and the rule becomes testable.
    """
    now = now or time.time()
    tx = lm.transmitters()
    out = []
    for call, age, snr in lm.hearers_of(lm.my_call, HEARS_US_SECS):
        c = base(call)
        if c == base(lm.my_call) or not is_routable(c):
            continue
        if c not in tx:                   # exactly what the rule drops
            out.append(Candidate(call, snr, age, False, False, 0.0,
                                 [f"hears us {snr:+d} dB, "
                                  f"{age/60:.0f} min ago",
                                  "NOT seen transmitting -- the rule "
                                  "would skip this; called on purpose "
                                  "to measure whether the rule is right"]))
    out.sort(key=lambda x: x.hears_age)
    return out[:n]
