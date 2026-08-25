"""model.py — probability estimates for the js8reach planner.

Every number here is estimated from mined evidence, never asserted.
Where evidence is thin the estimator falls back to a measured fleet
prior, and the caller can see which by reading `Belief.why`.

Deliberate modelling choices:

* `p_copy` is a Poisson rate over the station's own UTC-hour histogram:
  P(we decode them at least once in the next W s). It intentionally
  fuses "are they transmitting" with "can we copy them", because the
  only evidence we have IS our own decodes — the SNR column is a
  truncated sample (frames we failed to decode are absent), so using it
  as a link-quality estimator would be selection bias. SNR is therefore
  used only as a margin/quality tiebreak, never as the reach estimate.

* `p_hears_us` is separate and asymmetric: it needs THEIR report of US
  (`rev_snr_*`, mined from "X: WM8Q SNR -06" style frames).

* `p_ans` is a Beta-Bernoulli posterior over OUR measured probe history
  for that station, with the fleet base rate as prior. Silence never
  proves absence (the project's standing ARQ invariant) — it just moves
  the posterior.

* `p_fwd` is relay capability, which no JS8 command can query
  (audit: no RELAY? token, relay_off() never leaves the box). Evidence
  is behavioural: a station observed transmitting "*DE*" or a "CALL>"
  head has provably relayed.
"""

from __future__ import annotations

import math
import sqlite3
from dataclasses import dataclass, field

import intel
from callsign import base, same

# Fleet priors, measured from this station's own mined history.
# P(answers | demonstrably present) = 359/440 = 82%, measured over our
# own probe history. This is CONDITIONAL on presence and is the only
# form that composes correctly with a separate p_copy term. The
# unconditional rate (17%) is mostly a measure of absence; multiplying
# it by presence counted absence twice and suppressed every live
# station's score (operator caught the weighting, 2026-08-21).
# Operational planning ignores answer history entirely (see p_ans).
# The measured conditional rate below is retained for timing research
# and for the replay simulator's latent draw.
USE_ANSWER_HISTORY = False
MEASURED_ANSWER_GIVEN_PRESENT = 0.82   # 359/440 of our own probes
PRIOR_ANSWER_RATE = 0.82
PRIOR_WEIGHT = 6.0
PRIOR_ANSWER_RATE_ENGAGED = 0.82   # same conditional rate
PRIOR_WEIGHT_ENGAGED = 4.0
PRIOR_HEARS_US = 0.60        # they hear us | we hear them (HF reciprocity)
# MEASURED, 2026-08-23, not reasoned from configuration defaults. 116
# relay requests were watched go out on the air and 43 were acted on:
# 37%, where this constant used to say 55% on the grounds that
# relay_off() defaults OFF. Per station it runs 0% to 100% -- WO7I 7 of
# 9, AC7WY 5 of 5, WB7TSQ 2 of 12, KS1DMD 0 of 6 -- so the prior is only
# the starting point and a station's own record overrides it quickly.
# 87 of 203 relay requests acted on, re-measured 2026-08-25 after the
# callsign fix exposed 75% more of them. Was 43/116 = 0.37.
PRIOR_RELAYS = 0.43
PRIOR_RELAY_WEIGHT = 2.0     # how many observations the prior is worth
SESSION_S = 900              # heard within 15 min => mid-session
UNKNOWN_PATH_P = 0.30        # no location, no history: neutral
EDGE_MAX_AGE_H = 6.0         # older link evidence claims nothing
# An "always-on responder": a station whose transmissions are almost
# entirely REPLIES. It keys only when asked, so its spontaneous rate
# carries no information about availability, and estimating its
# reachability from decode volume systematically underrates exactly the
# stations that make the best relays (operator, 2026-08-21: "ac7wy is a
# full-time tx/rx station... no recent tx except maybe HB, but still
# ready to relay"). For these, availability is measured by whether they
# ANSWER, decayed by how long since they last did.
RESPONDER_RATIO = 0.85       # replies / total transmissions
RESPONDER_MIN_FRAMES = 20    # enough traffic for the ratio to mean it
RESPONDER_HALFLIFE_D = 10.0  # confidence half-life since last answer
NORMAL_SNR_FLOOR = -24       # JS8Submode Normal rxSNRThreshold


@dataclass
class Belief:
    p: float
    why: list[str] = field(default_factory=list)

    def __mul__(self, other: "Belief") -> "Belief":
        return Belief(self.p * other.p, self.why + other.why)


def _clamp(x: float, lo: float = 0.01, hi: float = 0.99) -> float:
    return max(lo, min(hi, x))


class Model:
    def __init__(self, db: sqlite3.Connection, now: int):
        self.db = db
        self.now = now
        row = db.execute(
            "SELECT MIN(first_heard) a, MAX(last_heard) b FROM stations"
        ).fetchone()
        span = (row["b"] or now) - (row["a"] or now)
        self.days = max(1.0, span / 86400.0)
        self.mycall = intel.get_meta(db, "mycall", "")
        self.mygrid = intel.get_meta(db, "mygrid", "") or ""

    # ---- components ------------------------------------------------

    def p_copy(self, call: str, window_s: float) -> Belief:
        """P(we decode `call` at least once within window_s)."""
        st = intel.station(self.db, call)
        if not st or not st["last_heard"]:
            # [operator 2026-08-21] "why does it matter we've never
            # decoded a station? we're discussing *now*."
            # Correct: never having copied someone is the ABSENCE of
            # evidence, not evidence the path is bad — and for the real
            # mission the target is usually a station we have never
            # worked. So there is no penalty cliff: fall back to the
            # physical plausibility of the path (geometry), exactly as
            # for a station heard long ago.
            geo_p, geo_why = self._geo_from_me(call)
            if geo_p:
                return Belief(_clamp(geo_p),
                              [f"{call}: never decoded here (no recency "
                               f"evidence either way)", geo_why])
            return Belief(UNKNOWN_PATH_P,
                          [f"{call}: never decoded here and no grid known "
                           f"-> neutral {UNKNOWN_PATH_P:.2f}"])
        age = self.now - st["last_heard"]
        if age < 0:
            # Future-dated evidence is a clock artefact, not freshness.
            age = SESSION_S + 1
        if age <= SESSION_S:
            return Belief(0.95, [f"{call}: heard {age // 60} min ago "
                                 f"(mid-session)"])
        # Always-on responder branch (see RESPONDER_RATIO above).
        tot = (st["resp_count"] or 0) + (st["spont_count"] or 0)
        if tot >= RESPONDER_MIN_FRAMES and \
                (st["resp_count"] or 0) / tot >= RESPONDER_RATIO:
            days = age / 86400.0
            p = _clamp(0.90 * (0.5 ** (days / RESPONDER_HALFLIFE_D)),
                       lo=0.05)
            return Belief(p, [
                f"{call}: {100 * st['resp_count'] // tot}% of its traffic "
                f"is replies -> always-on responder; availability from "
                f"answer history, not decode rate ({days:.1f}d since last "
                f"heard) -> p_copy={p:.2f}"])
        # Recency is the only DIRECT evidence that they are on the air
        # and the path works. It decays fast (6 h e-folding): a decode
        # two days ago says little about right now.
        p_recent = 0.95 * math.exp(-(age / 3600.0) / 6.0)
        geo_p, geo_why = self._geo_from_me(call)
        p = max(p_recent, geo_p)
        why = [f"{call}: last heard {age // 3600}h {(age % 3600) // 60}m "
               f"ago -> recency {p_recent:.2f}"]
        if geo_why:
            why.append(geo_why)
        why.append(f"-> p_copy={_clamp(p):.2f}")
        return Belief(_clamp(p), why)

    def p_hears_us(self, call: str) -> Belief:
        st = intel.station(self.db, call)
        if st and st["rev_snr_n"]:
            age = self.now - (st["rev_last"] or 0)
            best = st["rev_snr_best"]
            margin = (best - NORMAL_SNR_FLOOR) / 12.0
            p = _clamp(0.5 + 0.4 * math.tanh(margin))
            if age > 30 * 86400:
                p *= 0.8
            return Belief(p, [f"{call}: reported our signal {st['rev_snr_n']}x, "
                              f"best {best:+d} dB -> p_hears_us={p:.2f}"])
        return Belief(PRIOR_HEARS_US,
                      [f"{call}: never reported our signal -> prior "
                       f"{PRIOR_HEARS_US:.2f}"])

    def p_ans(self, call: str) -> Belief:
        # [operator directive 2026-08-21] ALL.TXT-derived answer
        # history is NOT used for operational planning.
        #
        # Reasoning, which is sound: the mission is reaching a station
        # we have likely never worked, through relays we have likely
        # never used. For those, p_ans has no data and returns the
        # prior — and a constant factor multiplies every candidate
        # identically, so it CANNOT change a p/t ordering. Where
        # history does exist it is a handful of samples, so its only
        # real effect is to perturb the ranking on thin evidence.
        # Presence and location decide; they are what we actually
        # measure well (113k sightings vs ~440 conditional probes).
        #
        # The probe history is still mined and kept, but only for
        # TIMING RESEARCH (it is what validated the 67-112 s cost
        # model against measured 42-110 s replies) and for the
        # simulator's latent draw. Set USE_ANSWER_HISTORY = True to
        # restore it to planning.
        if not USE_ANSWER_HISTORY:
            return Belief(1.0, [f"{call}: answer history not used for "
                                f"planning (presence + location decide)"])
        return self._p_ans_from_history(call)

    def _p_ans_from_history(self, call: str) -> Belief:
        n, a, lat = intel.probe_stats(self.db, call)
        # The 17% fleet base rate is dominated by stations we have no
        # relationship with. A station that has ever sent US traffic
        # has demonstrably engaged, so it gets the working-relationship
        # prior instead — otherwise the prior swamps real evidence
        # (KD7WPQ: 4,328 decodes, 2/4 probes answered, dragged to 0.26).
        st = intel.station(self.db, call)
        engaged = bool(st and st["to_us"])
        prior = PRIOR_ANSWER_RATE_ENGAGED if engaged else PRIOR_ANSWER_RATE
        weight = PRIOR_WEIGHT_ENGAGED if engaged else PRIOR_WEIGHT
        alpha = prior * weight + a
        beta = (1 - prior) * weight + (n - a)
        p = alpha / (alpha + beta)
        if n:
            return Belief(_clamp(p),
                          [f"{call}: answered {a}/{n} of our probes"
                           + (f", median {lat:.0f}s" if lat else "")
                           + f" -> p_ans={p:.2f}"])
        return Belief(_clamp(p), [f"{call}: no probe history -> fleet prior "
                                  f"{p:.2f}"])

    def p_fwd(self, call: str) -> Belief:
        """How likely this station is to pass traffic along.

        From its OWN record where we have one: how many relay requests
        we watched go out to it, and how many it acted on. That is the
        thing we actually want to know, and it varies from 0% to 100%
        across stations -- far too wide to replace with any prior.
        """
        st = intel.station(self.db, call)
        if not st:
            return Belief(PRIOR_RELAYS, [f"{call}: unknown -> prior"])
        asked = st["relay_asked"] or 0
        done = st["relay_done"] or 0
        if asked:
            p = _clamp((PRIOR_RELAYS * PRIOR_RELAY_WEIGHT + done)
                       / (PRIOR_RELAY_WEIGHT + asked))
            return Belief(p, [f"{call}: forwarded {done} of {asked} "
                              f"requests -> p_fwd={p:.2f}"])
        seen = st["relay_seen"] or 0
        if seen:
            # Never asked in our hearing, but seen forwarding for
            # somebody: it relays, we just have no rate for it.
            p = _clamp(0.55 + 0.05 * math.log1p(seen), hi=0.85)
            return Belief(p, [f"{call}: seen forwarding {seen}x, never "
                              f"observed being asked -> p_fwd={p:.2f}"])
        return Belief(PRIOR_RELAYS,
                      [f"{call}: relay never observed -> prior "
                       f"{PRIOR_RELAYS:.2f} (undiscoverable by any query)"])

    def _geo_link(self, hearer: str, heard: str) -> tuple[float, str]:
        """Distance-based prior that A can hear B.

        Needed because observed edges are sparse and go stale fast: for
        a target we rarely work, EVERY candidate's edge evidence is
        months old and decays to the same floor, which leaves relays
        ranked purely on their own availability. That put four western
        US stations at the top of a plan to reach a Maine station
        (AL0A/FN43, operator scenario 2026-08-21) while stations 161 km
        from the target were never considered at all.

        Geometry is the only evidence available for links we have never
        observed, and it is exactly the evidence an operator uses.
        Monotonic decay with a floor, because HF skip means distance
        never rules a path out.
        """
        import grid as _g
        ga = self.grid_of(hearer)
        gb = self.grid_of(heard)
        if not (ga and gb and _g.valid(ga) and _g.valid(gb)):
            return 0.0, ""
        try:
            km = _g.distance_km(ga, gb)
        except ValueError:
            return 0.0, ""
        p = 0.15 + 0.55 * math.exp(-km / 800.0)
        return p, (f"{hearer}({ga}) is {km:.0f} km from {heard}({gb}) "
                   f"-> geometric p_link={p:.2f}")

    def _geo_from_me(self, call: str) -> tuple[float, str]:
        """Physical plausibility that WE and `call` can hear each other,
        from distance alone. Same curve as _geo_link — it is the same
        physical question."""
        import grid as _g
        g = self.grid_of(call)
        if not (self.mygrid and g and _g.valid(self.mygrid)
                and _g.valid(g)):
            return 0.0, ""
        try:
            km = _g.distance_km(self.mygrid, g)
        except ValueError:
            return 0.0, ""
        p = 0.15 + 0.55 * math.exp(-km / 800.0)
        return p, (f"{call}({g}) is {km:.0f} km from us -> geometric "
                   f"path plausibility {p:.2f}")

    def grid_of(self, call: str) -> str:
        st = intel.station(self.db, call)
        return (st["grid"] or "") if st else ""

    def p_link(self, hearer: str, heard: str) -> Belief:
        geo_p, geo_why = self._geo_link(hearer, heard)
        row = self.db.execute(
            "SELECT * FROM edges WHERE hearer=? AND heard=?",
            (hearer.upper(), heard.upper())).fetchone()
        if not row:
            if geo_p:
                return Belief(_clamp(geo_p),
                              [f"{hearer} never observed hearing {heard}; "
                               + geo_why])
            return Belief(0.10, [f"{hearer} has never been observed "
                                 f"hearing {heard}, and no grid for both"])
        age_h = (self.now - row["last_when"]) / 3600.0
        # "hearing" = named in a HEARING list; "replied"/"freetext"/
        # "relayfrom" = it demonstrably received from them.
        base = (0.85 if row["source"] in
                ("hearing", "replied", "relayfrom", "freetext")
                else 0.55)
        corrob = min(0.10, 0.02 * row["n"])
        # HARD age cutoff, not a gentle decay (operator, 2026-08-21:
        # "old history tells us nothing!"). That A heard B last week
        # says nothing about whether A hears B now -- HF paths turn
        # over in hours. Past the cutoff the edge claims NOTHING about
        # the link; it only earns the station a place on the candidate
        # list, which is what old edges are legitimately for.
        if age_h > EDGE_MAX_AGE_H:
            why = [f"{hearer}->{heard}: only {age_h / 24:.0f}d-old "
                   f"{row['source']} evidence -- too old to claim "
                   f"anything about the link now; candidate only"]
            if geo_why:
                why.append(geo_why)
            return Belief(_clamp(geo_p if geo_p else 0.10), why)
        obs = (base + corrob) * (1.0 - age_h / EDGE_MAX_AGE_H)
        p = _clamp(max(obs + 0.05, geo_p))
        why = [f"{hearer}->{heard}: {row['source']} evidence x{row['n']}, "
               f"{age_h:.1f}h old -> {obs + 0.05:.2f}"]
        if geo_why:
            why.append(geo_why + f" (using {max(obs + 0.05, geo_p):.2f})")
        return Belief(p, why)

    # ---- composites ------------------------------------------------

    def p_direct(self, target: str, window_s: float) -> Belief:
        return (self.p_copy(target, window_s)
                * self.p_hears_us(target)
                * self.p_ans(target))

    def p_via(self, relay: str, target: str, window_s: float) -> Belief:
        """Contact with `target` through one relay hop."""
        return (self.p_copy(relay, window_s)
                * self.p_hears_us(relay)
                * self.p_fwd(relay)
                * self.p_link(relay, target)
                * self.p_ans(target))

    # ---- candidate pools -------------------------------------------

    def relay_candidates(self, target: str, limit: int = 12,
                         geo_radius_km: float = 1200.0,
                         active_days: int = 30) -> list[str]:
        """Relay pool = stations observed hearing `target` UNION
        stations geographically near it.

        The geographic half is essential, not a nicety: for a target we
        rarely work, every observed edge is months stale and decays to
        the same floor, so an edges-only pool ranks candidates purely
        on their own availability. That produced a plan to reach a
        Maine station through four western US relays while stations
        161 km from the target were never considered (AL0A/FN43,
        operator scenario 2026-08-21). Proximity is the evidence an
        operator would use, and the only evidence available for links
        nobody has observed.

        Geographic candidates are restricted to stations we have
        actually heard recently — a relay we cannot raise is useless
        however well placed it is.
        """
        import grid as _g
        mybase = self.mycall.split("/")[0]
        seen, out = set(), []
        for r in intel.hearers_of(self.db, target):
            h = r["hearer"]
            if h in seen or h.split("/")[0] == mybase:
                continue
            seen.add(h)
            out.append(h)

        tgrid = self.grid_of(target)
        if tgrid and _g.valid(tgrid):
            cutoff = self.now - active_days * 86400
            for row in self.db.execute(
                    "SELECT call, grid FROM stations WHERE grid IS NOT NULL "
                    "AND grid != '' AND last_heard > ?", (cutoff,)):
                c = row["call"]
                if c in seen or base(c) == mybase or same(c, target):
                    continue
                if not _g.valid(row["grid"]):
                    continue
                try:
                    if _g.distance_km(tgrid, row["grid"]) > geo_radius_km:
                        continue
                except ValueError:
                    continue
                seen.add(c)
                out.append(c)

        out.sort(key=lambda c: -self.p_via(c, target, 3600).p)
        return out[:limit]
