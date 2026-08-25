"""sim.py — replay harness: would the plan actually have worked?

Method: pick a historical instant T0, hide every observation at or after
it, let the planner build a plan from the past only, then score that
plan against what really happened afterwards (the `sightings` table is
ground truth for "was that station on the air and copyable by us").

Honest about what this does and does not validate:

  VALIDATED   the activity / reachability model — whether the station we
              chose to call was actually there, and how long our probe
              ordering took to find someone who was.
  MODELLED    whether they would have ANSWERED. We never ran these
              probes historically, so answer behaviour is drawn from
              the per-station rate measured from the probes we DID run
              (mine.py `probes` table). Monte Carlo over that draw.
  LEAKS       slowly-varying station traits (relay capability, whether
              they ever reported our SNR) are not horizon-filtered.
              They change on the scale of months, not minutes; the
              time-varying terms that dominate ARE filtered.

Baselines it is scored against:
  direct-only   call the target over and over (what an operator does)
  blind-relay   try relays with no intel, in arbitrary order
  js8reach      the planner
"""

from __future__ import annotations

import argparse
import math
import random
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import actions as A          # noqa: E402
import intel                 # noqa: E402
from model import Model      # noqa: E402
from planner import Planner  # noqa: E402


# The chance a pair NOBODY has ever reported is nonetheless a live
# link. It cannot be measured directly -- observing a link requires one
# of the two stations to transmit a report about it, which they rarely
# do -- so it is fitted against real reach on replayed instants. First
# set at 0.10 by reusing the measured "still live GIVEN evidence"
# figure, which was wrong in kind; then at 0.03 reasoning that 80% of
# pairs were never reported, which was wrong in the other direction and
# cost two points of reach.
UNSEEN_LINK = 0.12


# HOW OFTEN THE PATH WORKS BOTH WAYS, given it works one way.
#
# MEASURED DIRECTIONALLY 2026-08-25, over 28,785 observed links. The
# question is P(b hears a | a hears b), and what decides it is how good
# a LISTENER b is compared with a:
#
#     b hears under 1/10 of what a hears     5.5%   (19,143 pairs)
#     b hears 1/10 to 1/2                   19.9%
#     similar, 0.5x to 2x                   31.1%
#     b hears 2x to 10x more                36.2%
#     b hears over 10x more                 42.3%
#
# Monotone from 3% to 41%, and my first version could not see any of
# it: it took the RATIO of the two degrees symmetrically, so a pair
# where the receiver hears ten times more scored the same as one where
# it hears a tenth as much -- opposite situations collapsed into one
# bucket at 2.5%. KF5YWQ answering "+14" about AI5TS was scored 0.22
# when AI5TS hears eleven times more than KF5YWQ does and the truth is
# about 0.41. The strongest report of the night ranked below a weaker
# one because of it.
#
# The physics is not symmetric and neither is this: a big-eared station
# hears everyone and is heard by few, so which end has the big ears
# decides the answer.
# Re-measured after the callsign fix, on a corpus a third larger:
# 5.5 / 19.9 / 31.1 / 36.2 / 42.3 across 39,485 links. The shape is
# unchanged -- monotone in how good a listener the receiver is -- which
# is reassuring, since the first version was derived from a corpus
# missing 24% of the traffic and 710 stations.
RECIPROCITY = ((0.1, 0.055), (0.5, 0.199), (2.0, 0.311),
               (10.0, 0.362), (float("inf"), 0.423))


def reciprocity_for(deg_sender: int, deg_receiver: int) -> float:
    """Chance the RECEIVER hears the sender, given the sender hears it.

    Arguments are ordered sender-then-receiver and the answer is NOT
    symmetric in them -- passing them the wrong way round inverts the
    result across a 13x range.
    """
    a = max(int(deg_sender), 1)
    b = max(int(deg_receiver), 1)
    ratio = b / float(a)
    for edge, p in RECIPROCITY:
        if ratio < edge:
            return p
    return RECIPROCITY[-1][1]


class HorizonModel(Model):
    """Model that can only see observations before `horizon`."""

    def __init__(self, db, horizon: int):
        super().__init__(db, horizon)
        self.horizon = horizon
        self.learned_grids: dict = {}
        self._degree: dict = {}

    def grid_of(self, call: str) -> str:
        """A grid we had ALREADY learned by the horizon -- or one a
        station has just told us in answer to GRID?.

        The base model reads the grid table with no horizon filter, so
        a replay was handing the strategy every station's location
        regardless of whether we knew it at the time. That made GRID?
        worthless by construction: the model already knew the answer to
        any question it could ask. A grid arrives when we first decode
        a station, so knowing it means having heard them before now.
        """
        c = call.upper()
        if c in self.learned_grids:
            return self.learned_grids[c]
        st = intel.station(self.db, c)
        if not st or not st["grid"]:
            return ""
        if st["first_heard"] and st["first_heard"] >= self.horizon:
            return ""            # we had not met them yet
        return st["grid"]

    def out_degree(self, call: str) -> int:
        got = self._degree.get(call)
        if got is None:
            got = self.db.execute(
                "SELECT COUNT(*) n FROM edges WHERE hearer=? AND "
                "last_when < ?", (call.upper(), self.horizon)
            ).fetchone()["n"]
            self._degree[call] = got
        return got

    def p_reverse(self, a: str, b: str):
        """Chance b hears a, GIVEN we have evidence a hears b.

        Not another independent lookup of the same shape -- that was the
        mistake. Conditioned on how comparable the two stations are,
        because that is what the data says decides it.
        """
        from model import Belief
        # a hears b; we want P(b hears a), so b is the RECEIVER.
        p = reciprocity_for(self.out_degree(a), self.out_degree(b))
        return Belief(p, [f"{b}->{a}: reverse of a known link, "
                          f"listener degrees {self.out_degree(a)}->"
                          f"{self.out_degree(b)} -> {p:.2f}"])

    def true_grid_of(self, call: str) -> str:
        """What they WOULD tell us if asked -- ground truth, used only
        to answer a GRID? that the strategy actually spent time on."""
        st = intel.station(self.db, call.upper())
        return (st["grid"] or "") if st else ""

    def p_copy(self, call: str, window_s: float):
        from model import Belief, SESSION_S, _clamp
        row = self.db.execute(
            "SELECT MAX(ts) last, COUNT(*) n FROM sightings "
            "WHERE call=? AND ts<?", (call.upper(), self.horizon)
        ).fetchone()
        if not row or not row["last"]:
            return Belief(0.02, [f"{call}: not decoded before horizon"])
        age = self.horizon - row["last"]
        if age <= SESSION_S:
            return Belief(0.95, [f"{call}: heard {age // 60} min before T0"])
        # Same always-on-responder branch as the production model, so
        # the simulator scores the policy we would actually run. The
        # traffic ratio is a slowly-varying station trait and is not
        # horizon-filtered (documented leak, top of file).
        from model import (RESPONDER_RATIO, RESPONDER_MIN_FRAMES,
                           RESPONDER_HALFLIFE_D)
        st = intel.station(self.db, call)
        tot = ((st["resp_count"] or 0) + (st["spont_count"] or 0)) if st else 0
        if tot >= RESPONDER_MIN_FRAMES and \
                (st["resp_count"] or 0) / tot >= RESPONDER_RATIO:
            days = age / 86400.0
            p = _clamp(0.90 * (0.5 ** (days / RESPONDER_HALFLIFE_D)),
                       lo=0.05)
            return Belief(p, [f"{call}: always-on responder pre-T0"])
        import datetime as _dt
        hour = _dt.datetime.fromtimestamp(self.horizon,
                                          _dt.timezone.utc).hour
        h = self.db.execute(
            "SELECT COUNT(*) n FROM sightings WHERE call=? AND ts<? "
            "AND CAST(strftime('%H', ts, 'unixepoch') AS INT)=?",
            (call.upper(), self.horizon, hour)).fetchone()["n"]
        span_days = max(1.0, (self.horizon - self.db.execute(
            "SELECT MIN(ts) m FROM sightings").fetchone()["m"]) / 86400.0)
        lam = h / span_days
        p = 1.0 - math.exp(-lam * (window_s / 3600.0))
        if age > 7 * 86400:
            p *= 0.3
        elif age > 86400:
            p *= 0.7
        return Belief(_clamp(p), [f"{call}: {h} decodes in hour {hour:02d} "
                                  f"pre-T0 -> p_copy={_clamp(p):.2f}"])

    def p_link(self, hearer: str, heard: str):
        from model import Belief, _clamp
        row = self.db.execute(
            "SELECT * FROM edges WHERE hearer=? AND heard=? AND last_when<?",
            (hearer.upper(), heard.upper(), self.horizon)).fetchone()
        if not row:
            # FALL BACK TO GEOMETRY, as the production model does. This
            # override was dropping the geographic term the base model
            # adds, so 86% of candidate/target pairs came back at the
            # same 0.10 floor, all tied -- which made the "nearest" aim
            # return stations in pool order rather than by distance, and
            # made GRID? worthless because nothing consumed a grid.
            # Grids come from grid_of, which is horizon-filtered, so a
            # location we had not learned yet still does not count.
            # NOTHING UNOBSERVED MAY OUTRANK SOMETHING OBSERVED. A
            # link reported an hour ago is live about 28% of the time
            # (measured). The geographic prior was returning up to 0.70
            # for co-located stations and the no-evidence floor was
            # 0.10 -- the same figure measured as "still live GIVEN
            # evidence", reused for pairs with no evidence at all. Two
            # different quantities: 80% of pairs among well-heard
            # stations were never reported once in five months.
            #
            # It also broke multi-hop routing outright. With every pair
            # floored at 0.10, every station had a direct link to the
            # target, and since each extra hop multiplies by less than
            # one, a single hop was always cheapest. The backward search
            # returned 2,501 one-hop routes and 17 two-hop ones.
            geo, why = self._geo_link(hearer, heard)
            if geo > 0.0:
                return Belief(_clamp(UNSEEN_LINK * 0.7
                                     + UNSEEN_LINK * 2.5
                                     * (geo - 0.15) / 0.55,
                                     lo=UNSEEN_LINK * 0.5,
                                     hi=0.30), [why])
            return Belief(UNSEEN_LINK,
                          [f"{hearer}->{heard}: never reported "
                           f"in five months of logs"])
        age_h = (self.horizon - row["last_when"]) / 3600.0
        base = (0.85 if row["source"] in
                ("hearing", "replied", "relayfrom", "freetext")
                else 0.55)
        # SIGNAL MARGIN, not merely "a link was reported". A hop that
        # only just decodes fails on the first pass and costs a whole
        # cycle to retry, so a route should prefer margin. The decode
        # floor is -24 dB; 19% of reported links sit below -18. Never
        # zero -- a marginal path is worse, not impossible.
        margin = 1.0
        if row["snr"] is not None:
            margin = _clamp((row["snr"] + 24.0) / 18.0, lo=0.25, hi=1.0)
        # HOW LINK EVIDENCE REALLY DECAYS, fitted 2026-08-23 to 86,934
        # dated observations. The old curve was exp(-age/21 days), which
        # barely moves in a week; measured, a link's chance of showing
        # again halves inside a day. The model was three to six times
        # too optimistic about every link, which is exactly why the
        # decision rule was confident about routes that do not exist.
        #
        # The second term is DIURNAL. Evidence from the same hour
        # yesterday predicts better than evidence from this morning --
        # 20.0% at 24 hours against 11.4% at 12 -- because propagation
        # repeats daily. The model already knew that about whether a
        # station is on the air; it did not know it about paths.
        di = max(0.0, math.cos(2.0 * math.pi * age_h / 24.0))
        live = (0.120
                + 0.100 * math.exp(-age_h / 5.0)
                + 0.080 * math.exp(-age_h / 192.0) * di)
        p = _clamp(live * margin * (base / 0.85)
                   * (1.0 + min(0.30, 0.06 * row["n"])))
        return Belief(p, [f"{hearer}->{heard}: {row['source']} x{row['n']}, "
                          f"{age_h/24:.1f}d pre-T0 -> {p:.2f}"])

    def p_ans(self, call: str):
        # The simulator still needs a latent "would they answer" draw
        # to score anything. That is timing/behaviour RESEARCH, which
        # the operator directive explicitly permits — it is not used
        # to rank actions (the planner's p_ans returns a constant).
        from model import (Belief, PRIOR_ANSWER_RATE, PRIOR_WEIGHT,
                           PRIOR_ANSWER_RATE_ENGAGED, PRIOR_WEIGHT_ENGAGED,
                           _clamp)
        rows = list(self.db.execute(
            "SELECT ts, answered FROM probes WHERE target=? AND ts<?",
            (call.upper(), self.horizon)))
        # Recency-weighted, half-life 30 days, same rule as the live
        # model so replay and reality cannot drift apart.
        n = a = 0.0
        for r in rows:
            w = 0.5 ** ((self.horizon - r["ts"]) / (30 * 86400.0))
            n += w
            a += w * r["answered"]
        st = intel.station(self.db, call)
        engaged = bool(st and st["to_us"])
        prior = PRIOR_ANSWER_RATE_ENGAGED if engaged else PRIOR_ANSWER_RATE
        w = PRIOR_WEIGHT_ENGAGED if engaged else PRIOR_WEIGHT
        p = (prior * w + a) / (w + n)
        return Belief(_clamp(p), [f"{call}: {a}/{n} pre-T0 probes"])


@dataclass
class Outcome:
    reached: bool
    seconds: float
    steps: int
    hops: int
    via: str | None


def on_air(db, call: str, t0: float, t1: float) -> bool:
    """Ground truth: did we actually copy `call` in this window?"""
    return intel.sightings_between(db, call, int(t0), int(t1)) > 0


def run_plan(db, plan: list[A.Action], t0: float, model,
             rng: random.Random, cap_s: float = 1800.0,
             willing: dict[str, bool] | None = None,
             forwarding: dict[str, bool] | None = None) -> Outcome:
    """Execute a plan against ground truth.

    `willing` carries the LATENT per-station answer disposition for
    this trial, drawn ONCE and shared by every strategy in the same
    trial. That correlation is the point: whether a station answers is
    a property of its configuration and whether an operator is present
    (remote AutoreplyConfirmation defaults ON), not an independent coin
    flip per probe. Drawing it per attempt made brute-force re-calling
    look far better than it is and hid the value of escalating — the
    first version of this simulator had exactly that flaw.
    """
    if willing is None:
        willing = {}
    if forwarding is None:
        forwarding = {}
    t = t0
    for i, a in enumerate(plan, 1):
        if t - t0 > cap_s:
            break
        start, end = t, t + a.t
        t = end
        if a.info:
            continue                       # discovery: costs time only
        who = a.via or a.target
        if not who or not on_air(db, who, start, end + 60):
            continue                       # station wasn't there
        if a.via:
            # Relay leg: the RELAY must hear the target. Ground truth
            # is third-party evidence (a HEARING mention in a window
            # around this attempt) OR our own decode of the target.
            # Using only our own decodes would make relays structurally
            # unmeasurable — if we can hear T, direct already wins.
            w0, w1 = int(start - 1800), int(end + 1800)
            link_ok = (intel.pair_heard_between(db, a.via, a.target, w0, w1)
                       or on_air(db, a.target, start, end + 120))
            if not link_ok:
                continue
        if a.target not in willing:
            willing[a.target] = rng.random() <= model.p_ans(a.target).p
        if not willing[a.target]:
            continue                       # this station isn't answering
        if a.via and a.via not in forwarding:
            # The relay must also be willing to forward at all. Kept in
            # its OWN dict: "answers me" and "forwards for me" are
            # different dispositions with different rates, and sharing
            # one dict keyed by callsign made a station that appeared
            # first as a target carry its answer draw into its relay
            # role.
            forwarding[a.via] = rng.random() <= model.p_fwd(a.via).p
        if a.via and not forwarding[a.via]:
            continue
        return Outcome(True, t - t0, i, a.hops, a.via)
    return Outcome(False, min(t - t0, cap_s), len(plan), 0, None)


def baseline_direct(db, target: str, t0: float, model,
                    rng: random.Random, cap_s: float = 1800.0,
                    willing=None) -> Outcome:
    plan = [A.ping(target, 0.0, []) for _ in range(int(cap_s // A.rtt(1, 1)))]
    return run_plan(db, plan, t0, model, rng, cap_s, willing)


def baseline_blind_relay(db, target: str, t0: float, model,
                         rng: random.Random, cap_s: float = 1800.0,
                         willing=None) -> Outcome:
    """No intel: alphabetical relay order after one direct try."""
    pool = sorted({r["hearer"] for r in intel.hearers_of(db, target)})
    plan = [A.ping(target, 0.0, [])]
    plan += [A.relay_ping(r, target, 0.0, []) for r in pool[:8]]
    return run_plan(db, plan, t0, model, rng, cap_s, willing)


def pick_instants(db, target: str, n: int, rng: random.Random,
                  relay_cases: bool = False) -> list[int]:
    """Historical instants to replay.

    Default: shortly before a time the target was actually decodable
    here — contact was genuinely possible, so the run measures how
    fast each strategy finds it.

    `relay_cases`: instants where a THIRD PARTY reported hearing the
    target while WE did not decode it in the same window. That is the
    only situation a relay exists to solve, and the only one where a
    relay strategy can be scored honestly.
    """
    if relay_cases:
        rows = list(db.execute(
            "SELECT DISTINCT ts FROM edge_events WHERE heard=? ORDER BY ts",
            (target.upper(),)))
        cands = []
        for r in rows:
            ts = r["ts"]
            if not on_air(db, target, ts - 900, ts + 900):
                cands.append(ts)
        if not cands:
            return []
        picks = rng.sample(cands, min(n, len(cands)))
        return [s - rng.randint(600, 1800) for s in picks]
    rows = list(db.execute(
        "SELECT DISTINCT ts FROM sightings WHERE call=? ORDER BY ts",
        (target.upper(),)))
    if not rows:
        return []
    stamps = [r["ts"] for r in rows]
    picks = rng.sample(stamps, min(n, len(stamps)))
    # Start 20-40 min BEFORE a known activity, so the plan has to find
    # them rather than being handed a mid-session gift.
    return [s - rng.randint(1200, 2400) for s in picks]


def main() -> int:
    ap = argparse.ArgumentParser(description="js8reach replay simulator")
    ap.add_argument("--call", required=True)
    ap.add_argument("--trials", type=int, default=40)
    ap.add_argument("--db", default=str(intel.DEFAULT_DB))
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--cap-min", type=float, default=30.0)
    ap.add_argument("--relay-cases", action="store_true",
                    help="replay only instants where a third party "
                         "heard the target and we did not")
    args = ap.parse_args()

    db = intel.connect(args.db)
    rng = random.Random(args.seed)
    target = args.call.upper()
    instants = pick_instants(db, target, args.trials, rng,
                             relay_cases=args.relay_cases)
    if not instants:
        print(f"sim: no {'relay-case' if args.relay_cases else ''} "
              f"instants available for {target}")
        return 1
    cap = args.cap_min * 60

    results: dict[str, list[Outcome]] = {"js8reach": [], "rules": [],
                                         "direct-only": [],
                                         "blind-relay": []}
    for t0 in instants:
        m = HorizonModel(db, int(t0))
        pl = Planner(m)
        plan = pl.plan(target, max_steps=14)
        # ONE latent draw per trial, shared by every strategy, so the
        # comparison is like-for-like on the same imagined evening.
        seed = rng.randint(0, 2 ** 31)
        latent: dict[str, bool] = {}
        random.Random(seed)  # deterministic per trial
        r = random.Random(seed)
        results["js8reach"].append(run_plan(db, plan, t0, m, r, cap, latent))
        import rules as R
        r = random.Random(seed)
        results["rules"].append(
            run_plan(db, R.plan(m, target), t0, m, r, cap, dict(latent)))
        r = random.Random(seed)
        results["direct-only"].append(
            baseline_direct(db, target, t0, m, r, cap, dict(latent)))
        r = random.Random(seed)
        results["blind-relay"].append(
            baseline_blind_relay(db, target, t0, m, r, cap, dict(latent)))

    scen = ("relay cases (heard by others, not by us)"
            if args.relay_cases else "general (target was decodable here)")
    print(f"\nreplay: {target}, {len(instants)} instants, "
          f"cap {args.cap_min:.0f} min\n  scenario: {scen}\n")
    # E[T] charges a failure the full cap: a strategy that reaches
    # 40% of the time quickly is NOT better than one that reaches
    # 95% a little slower, and a success-only mean would say it was.
    print(f"  {'strategy':<14} {'reached':>8} {'E[T]':>9} "
          f"{'median ok':>10} {'via relay':>10}")
    print("  " + "-" * 58)
    for name, outs in results.items():
        ok = [o for o in outs if o.reached]
        times = sorted(o.seconds for o in ok)
        med = times[len(times) // 2] / 60 if times else float("nan")
        et = sum(o.seconds if o.reached else cap
                 for o in outs) / len(outs) / 60
        relayed = sum(1 for o in ok if o.via)
        print(f"  {name:<14} {len(ok):>4}/{len(outs):<3} "
              f"{et:>8.1f}m {med:>9.1f}m {relayed:>10}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
