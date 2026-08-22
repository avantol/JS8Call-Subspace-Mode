"""planner.py — the time-optimal probe ordering.

Optimality: probes are strictly sequential (half-duplex radio). For
independent candidate actions with success probability p_i and duration
t_i, run until first success, expected total time is minimized by
ordering on p_i/t_i DESCENDING. Adjacent-exchange proof: running (i,j)
costs t_i + (1-p_i)*t_j and (j,i) costs t_j + (1-p_j)*t_i, so (i,j)
wins iff p_j*t_i < p_i*t_j, i.e. iff p_i/t_i > p_j/t_j.

Real observations are dependent — a broadcast sweep rewrites the
probability of every relay action — so the index is recomputed after
every observation (Bayesian update, then replan). That is the standard
tractable approximation to the underlying POMDP, and it is exactly
optimal in the independent case.

Hops need no separate objective: a relay hop costs ~90 s round trip, so
ordering by time already orders by hops; hops only break ties.

Discovery actions (broadcast / HEARING?) cannot make contact at all,
so they carry no success mass and CANNOT be ranked on the p/t scale.
They are placed by comparing whole plans instead: E[T] of a plan that
includes the sweep against E[T] of one that does not. That keeps the
index theorem applying only where it is valid.
"""

from __future__ import annotations

from dataclasses import dataclass

import actions as A
from model import Model

# A silent probe shifts the posterior but never closes it.
MAX_DIRECT_RETRIES = 12
SILENCE_DECAY = 0.82
# Failure of any attempt has two possible causes: the target was not
# reachable (propagation / not on air) or the target does not answer at
# all (autoreply off, nobody home). A relay fixes ONLY the first. So
# every failed attempt decays the shared "will answer" belief for EVERY
# later action against that target — pings and relays alike — while the
# steeper reach decay applies to repeated direct calls only. Without
# this, relays inherit a fresh answer probability the evidence has
# already argued against, and the planner keeps paying 3 min a time for
# paths that cannot work (caught by the correlated replay simulator,
# 2026-08-21).
ANSWER_DECAY = 0.88


@dataclass
class Step:
    action: A.Action
    e_time_after: float          # E[time to contact] if we start here


class Planner:
    def __init__(self, model: Model, *, allow_broadcast: bool = True,
                 message: str | None = None):
        self.m = model
        self.allow_broadcast = allow_broadcast
        self.message = message
        self.attempted: set[str] = set()

    # ---- expected time of a whole ordered plan ---------------------

    @staticmethod
    def expected_time(plan: list[A.Action]) -> float:
        """E[time to first success] for a sequential plan (the quantity
        the ordering minimizes). Actions that cannot make contact
        (discovery) contribute their cost with no success mass."""
        total, reach = 0.0, 1.0
        for a in plan:
            total += reach * a.t
            if not a.info:
                reach *= (1 - a.p)
        # Residual: nothing worked. Charge a nominal store-and-forward
        # tail so plans that leave the target unreached are penalized.
        return total + reach * 600.0

    # ---- candidate generation --------------------------------------

    def candidates(self, target: str, *, window_s: float = 3600.0,
                   known_hearers: dict[str, float] | None = None
                   ) -> list[A.Action]:
        m = self.m
        out: list[A.Action] = []

        # Direct pings, including RETRIES. A probe that drew silence
        # is weak evidence of absence, never proof (the project's
        # standing invariant) — JS8 stations are bursty, so the next
        # window carries almost the same chance. The replay simulator
        # proved this the hard way: without retries the planner
        # escalated to relays while plain re-calling would have won
        # (KD7WPQ, 2026-08-21: direct-only 29/30 vs planner 23/30).
        if f"ping:{target}" not in self.attempted:
            b = m.p_direct(target, window_s)
            for k in range(MAX_DIRECT_RETRIES):
                decay = SILENCE_DECAY ** k
                why = list(b.why) if k == 0 else [
                    f"retry {k + 1}: silence is not absence, "
                    f"p x{decay:.2f}"]
                out.append(A.ping(target, b.p * decay, why))

        # Relay candidates: mined edges, plus anything a live broadcast
        # sweep has just told us (known_hearers overrides the prior).
        pool = list(m.relay_candidates(target))
        for extra in (known_hearers or {}):
            if extra not in pool:
                pool.append(extra)
        for r in pool:
            key = f"relay:{r}:{target}"
            if key in self.attempted:
                continue
            b = m.p_via(r, target, window_s)
            p = b.p
            why = list(b.why)
            if known_hearers and r in known_hearers:
                # A fresh "YES +snr (age)" is direct evidence of the
                # R->T link; it replaces the aged mined estimate.
                fresh = known_hearers[r]
                p = min(0.95, p / max(0.10, m.p_link(r, target).p) * fresh)
                why.append(f"{r}: answered our sweep, live link {fresh:.2f}")
            out.append(A.relay_ping(r, target, p, why))

        if self.allow_broadcast and f"bcast:{target}" not in self.attempted:
            out.append(self._broadcast_action(target, out, window_s))

        if self.message:
            best = max((a for a in out if a.kind == "relay"),
                       key=lambda a: a.p, default=None)
            if best and best.via:
                b = m.p_fwd(best.via) * m.p_link(best.via, target)
                out.append(A.store_and_forward(
                    best.via, target, self.message, b.p * 0.9, b.why))
        return out

    def _broadcast_action(self, target: str, others: list[A.Action],
                          window_s: float) -> A.Action:
        """The sweep cannot make contact, so it carries no contact
        probability. Its worth is decided at PLAN level (see `choose`):
        we compare E[T] of plans that include it against plans that do
        not. `p` here is only a display figure — the chance the sweep
        returns at least one usable hearer."""
        relays = [a for a in others if a.kind == "relay"]
        # P(at least one candidate is on air and answers the sweep).
        miss = 1.0
        for a in relays:
            pc = self.m.p_copy(a.via, window_s).p * self.m.p_ans(a.via).p
            miss *= (1 - pc)
        p_any = min(0.95, 1 - miss)
        why = [f"one TX evaluates {len(relays)} relay candidates in "
               f"parallel (replies land on their own offsets); "
               f"P(at least one answers) = {p_any:.2f}"]
        return A.broadcast_query_call(target, p_any, why)

    def _informed(self, relays: list[A.Action],
                  target: str) -> list[A.Action]:
        """Relay actions as they look AFTER a sweep: the aged link
        estimate is replaced by fresh "YES +snr (age)" evidence, so
        only the relay's own reachability and relay-willingness remain
        uncertain."""
        out = []
        for a in relays:
            if not a.via:
                continue
            link = self.m.p_link(a.via, target).p
            p = min(0.95, a.p / max(0.05, link) * 0.85)
            out.append(A.relay_ping(a.via, target, p,
                                    a.why + ["post-sweep: link confirmed"]))
        out.sort(key=lambda x: (-x.index, x.hops))
        return out

    def choose(self, target: str, **kw) -> tuple[str, list[A.Action]]:
        """Pick the plan with the lowest E[time to contact] and return
        (label, ordered actions). Its FIRST action is what to transmit;
        after the observation we re-run this.

        Contact actions inside a plan are ordered by p/t — that part is
        provably optimal. Information actions cannot be ranked on that
        scale (they have no success mass), so they are placed by
        comparing whole plans instead.
        """
        cands = self.candidates(target, **kw)
        contacts = [a for a in cands if not a.info and a.kind != "sandf"]
        sandf = [a for a in cands if a.kind == "sandf"]
        infos = [a for a in cands if a.info]
        contacts.sort(key=lambda a: (-a.index, a.hops))
        relays = [a for a in contacts if a.kind == "relay"]
        head = [a for a in contacts if a.kind != "relay"]
        # Interleaved plan: keep taking whichever of (next retry, next
        # relay) has the better index — this is the pure p/t ordering
        # over the combined contact set, which `contacts` already is.
        interleaved = contacts

        plans: list[tuple[str, list[A.Action]]] = [
            ("index-order", interleaved + sandf),
            ("direct-only", head + sandf)]
        for info in infos:
            informed = self._informed(relays, target)
            plans.append((f"{info.kind}-first",
                          [info] + informed + head + sandf))
            if head:
                plans.append((f"direct-then-{info.kind}",
                              head + [info] + informed + sandf))
        plans = [(lbl, _shared_decay(pln)) for lbl, pln in plans]
        label, plan = min(plans,
                          key=lambda pl: self.expected_time(pl[1]))
        return label, plan

    # ---- the policy -------------------------------------------------

    def next_action(self, target: str, **kw) -> A.Action | None:
        _label, plan = self.choose(target, **kw)
        return plan[0] if plan else None

    def plan(self, target: str, *, max_steps: int = 6, **kw
             ) -> list[A.Action]:
        """The plan as it looks right now. The live runner re-plans
        after every observation instead of following this blindly."""
        _label, plan = self.choose(target, **kw)
        return plan[:max_steps]

    def plan_labeled(self, target: str, *, max_steps: int = 6, **kw
                     ) -> tuple[str, list[A.Action]]:
        label, plan = self.choose(target, **kw)
        return label, plan[:max_steps]

    def mark_attempted(self, a: A.Action) -> None:
        if a.kind == "ping":
            self.attempted.add(f"ping:{a.target}")
        elif a.kind == "relay":
            self.attempted.add(f"relay:{a.via}:{a.target}")
        elif a.kind == "broadcast":
            self.attempted.add(f"bcast:{a.target}")


def _shared_decay(plan: list[A.Action]) -> list[A.Action]:
    """Apply the shared "does this target answer at all" decay down a
    plan. The k-th contact attempt against a target inherits
    ANSWER_DECAY**k, because the k failures before it are evidence
    against the target answering by ANY route."""
    seen: dict[str, int] = {}
    out = []
    for a in plan:
        if a.info or not a.target:
            out.append(a)
            continue
        k = seen.get(a.target, 0)
        seen[a.target] = k + 1
        if k == 0:
            out.append(a)
            continue
        import copy as _copy
        b = _copy.copy(a)
        b.p = a.p * (ANSWER_DECAY ** k)
        b.why = list(a.why) + [
            f"{k} earlier attempt(s) drew silence: shared answer belief "
            f"x{ANSWER_DECAY ** k:.2f} (a relay cannot fix unwillingness)"]
        out.append(b)
    return out


def _best_p(relays: list[A.Action]) -> float:
    return max((a.p for a in relays), default=0.35)
