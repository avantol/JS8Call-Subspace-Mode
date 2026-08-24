"""invented.py — a made-up band, so sample size stops being the limit.

Andy, 2026-08-23: "sample size? you don't even need real samples. invent
them, in effect for the duration of a run."

The replay tests are limited by how much history exists. Relay cases
need a third party to have reported hearing the target at that moment,
and after mining every dated mention we have 256 of them -- enough that
an 8-stage strategy can still memorise its way to a good score. The
cold case is worse: reaching one named station at a moment nobody chose
succeeds maybe 1 time in 30, so telling a good strategy from a bad one
needs thousands of tries, and history hands us a few hundred.

So this module makes a band up. Stations, who can hear whom, when they
are on the air, whether they answer, whether they pass traffic along --
generated fresh, as many as wanted, for the length of a run.

WHAT IT IS FITTED TO (measured from the 117,693 real decodes, so the
invented band is at least the same SHAPE as the real one):

    13 stations audible in a half hour     median; 5 at the tenth
                                           percentile, 33 at the ninetieth
    58% still on the air 30 minutes later  sessions, not coin flips
    each station hears 3 others            median -- but 58 at the
                                           ninetieth percentile and 714
                                           at the top. A few stations
                                           hear nearly everything and
                                           most hear almost nothing, and
                                           a strategy that finds the
                                           former is worth a lot.
    13% of links are mutual                HEARING IS ONE-WAY. If A hears
                                           B, B almost certainly does not
                                           hear A. An invented band that
                                           got this wrong would make
                                           relaying look far easier than
                                           it is, because every relay
                                           found on the way out would
                                           work on the way back.

WHAT THIS BUYS AND WHAT IT COSTS. It buys unlimited trials, and a
strategy cannot memorise particular stations because there are no
particular stations -- every run draws a new band. What it costs is
that a strategy tuned here is tuned against my idea of a band, not
against the band. So the invented world is where the SEARCH runs, and
the real history stays as the exam: anything discovered here has to
prove itself on replayed instants before it goes on the air.

Everything is derived from a seed by hashing, never by a random stream,
so the same trial hands every strategy the identical band -- and the
same dither knob works, moving a station off its standing habit.
"""
from __future__ import annotations

import hashlib

SLOT_S = 900.0            # 15 minutes; on-air is decided per slot
BLOCK = 3                 # a session runs about 3 slots -> 58% persist

ME = "ME"


def _u(*parts) -> float:
    """A repeatable number in [0,1) for these exact parts."""
    key = "\x1f".join(str(p) for p in parts).encode()
    return int.from_bytes(hashlib.blake2b(key, digest_size=8).digest(),
                          "big") / float(1 << 64)


def _normal_q(u: float) -> float:
    """Inverse normal, Acklam's approximation. Good to 1e-9, which is
    far better than the data behind it deserves, but it lets a
    distribution be specified by its median and its ninetieth
    percentile instead of by fiddling with exponents until the printed
    numbers look about right -- which is how the first cut of this file
    was written, and it missed all three targets."""
    u = min(max(u, 1e-12), 1 - 1e-12)
    a = (-3.969683028665376e+01, 2.209460984245205e+02,
         -2.759285104469687e+02, 1.383577518672690e+02,
         -3.066479806614716e+01, 2.506628277459239e+00)
    b = (-5.447609879822406e+01, 1.615858368580409e+02,
         -1.556989798598866e+02, 6.680131188771972e+01,
         -1.328068155288572e+01)
    c = (-7.784894002430293e-03, -3.223964580411365e-01,
         -2.400758277161838e+00, -2.549732539343734e+00,
         4.374664141464968e+00, 2.938163982698783e+00)
    d = (7.784695709041462e-03, 3.224671290700398e-01,
         2.445134137142996e+00, 3.754408661907416e+00)
    pl, ph = 0.02425, 1 - 0.02425
    if u < pl:
        q = (2 * -__import__("math").log(u)) ** 0.5
        return (((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / \
               ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    if u > ph:
        q = (2 * -__import__("math").log(1 - u)) ** 0.5
        return -(((((c[0]*q+c[1])*q+c[2])*q+c[3])*q+c[4])*q+c[5]) / \
                ((((d[0]*q+d[1])*q+d[2])*q+d[3])*q+1)
    q = u - 0.5
    r = q * q
    return (((((a[0]*r+a[1])*r+a[2])*r+a[3])*r+a[4])*r+a[5]) * q / \
           (((((b[0]*r+b[1])*r+b[2])*r+b[3])*r+b[4])*r+1)


def _lognormal(u: float, median: float, p90: float) -> float:
    """A quantity given by its median and its ninetieth percentile --
    which is how the real measurements come out."""
    import math
    sigma = (math.log(p90) - math.log(median)) / 1.2815515655446004
    return math.exp(math.log(median) + sigma * _normal_q(u))


class Band:
    """One invented band. Same seed, same band, every time."""

    def __init__(self, seed: int, n_stations: int = 220):
        self.seed = seed
        self.calls = [f"S{i:03d}" for i in range(n_stations)]
        self.n = n_stations
        # A band is asked the same questions thousands of times as
        # strategies are compared on it, and every answer is a fixed
        # property of the band, so each one is worked out once.
        self._deg: dict = {}
        self._link: dict = {}
        self._air: dict = {}
        self._nbr: dict = {}

    # ---- standing traits of a station --------------------------------

    def up_rate(self, call: str) -> float:
        """Share of sessions this station is on the air at all, set so
        a 220-station band puts about 13 of them on in a half hour --
        the real median -- with the real spread of 5 to 33."""
        u = _u(self.seed, "up", call)
        return min(0.95, _lognormal(u, 0.020, 0.115))

    def answer_rate(self, call: str) -> float:
        """Matches the real spread: 0.43 to 0.92, median about 0.82."""
        u = _u(self.seed, "ansrate", call)
        return 0.40 + 0.55 * (u ** 0.45)

    def forward_rate(self, call: str) -> float:
        u = _u(self.seed, "fwdrate", call)
        return 0.50 + 0.45 * (u ** 1.6)

    def out_degree(self, call: str) -> int:
        """How many stations this one can hear BEFORE reciprocity adds
        the links that come back; the two together land on the measured
        median of 3 with a long tail. A few stations hear nearly
        everything, and finding those is worth a lot."""
        got = self._deg.get(call)
        if got is None:
            u = _u(self.seed, "deg", call)
            got = int(min(self.n - 1, round(_lognormal(u, 1.9, 40.0))))
            self._deg[call] = got
        return got

    # ---- who hears whom ----------------------------------------------

    def hears(self, a: str, b: str) -> bool:
        """Can `a` hear `b`?

        RECIPROCITY, AND WHY THE MEASURED 13% IS NOT IT. Counting all
        observed links, only 13% come back the other way, and the first
        version of this file took that as a fact about radio. It is
        mostly a fact about WHO REPORTS. A skimmer with a big antenna
        at a quiet site hears hundreds of stations and is heard by
        almost none, and those stations dominate the edge count. An
        unobserved link is also not a proven absent one -- we see an
        edge only when somebody publishes it -- so 13% is a lower
        bound on a biased sample.

        The path itself is reciprocal; the asymmetry lives in the
        antennas and the noise floors at the two ends. So reciprocity
        here follows the DEGREE RATIO: two ordinary stations that hear
        a similar number of others usually hear each other, while a
        station that hears twenty times what the other does mostly
        does not get heard back. That reproduces the lopsided
        population without claiming an ordinary pair is one-way.

        This matters more than it looks: the reply to a relayed message
        retraces the same stations, so every reverse link has to work.
        Modelling ordinary pairs as one-way made relaying nearly
        impossible in the invented band and no strategy could beat
        simply calling.
        """
        if a == b:
            return False
        key = (a, b)
        got = self._link.get(key)
        if got is None:
            got = self._hears_uncached(a, b)
            self._link[key] = got
        return got

    def _hears_uncached(self, a: str, b: str) -> bool:
        share_a = self.out_degree(a) / float(self.n)
        if _u(self.seed, "link", a, b) < share_a:
            return True
        # b hears a: does it come back? Similar stations, usually yes.
        share_b = self.out_degree(b) / float(self.n)
        if _u(self.seed, "link", b, a) < share_b:
            hi, lo = max(share_a, share_b), min(share_a, share_b)
            ratio = lo / hi if hi > 0 else 1.0
            return _u(self.seed, "recip", *sorted((a, b))) < 0.85 * ratio
        return False

    def strength(self, a: str, b: str) -> float:
        """How solid a link is, given it exists at all. Solid links
        hold up in poor conditions; marginal ones come and go."""
        return _u(self.seed, "strength", a, b)

    def hears_now(self, a: str, b: str, t: float) -> bool:
        """Can `a` hear `b` AT THIS MOMENT?

        Links were static in the first version -- a station we could
        not hear, we could never hear -- which is wrong about HF and
        quietly deleted the only reason `wait` could ever be a sensible
        move. A marginal path is open when the band is up and shut when
        it is down; a solid one holds either way. Since a relayed reply
        has to retrace every link, this decides far more than it looks.
        """
        if not self.hears(a, b):
            return False
        return self.strength(a, b) * self.openness(t) > 0.25

    def heard_by_me(self, call: str) -> bool:
        return self.hears(ME, call)

    def hears_me(self, call: str) -> bool:
        return self.hears(call, ME)

    # ---- when they are on ---------------------------------------------

    def openness(self, t: float) -> float:
        """How good the band is right now, for EVERYBODY at once.

        Without this the invented band was 220 independent coin flips
        and the count of audible stations barely moved -- 10 to 23,
        where the real one runs 5 to 33. Propagation does not treat
        stations independently: it lifts or drops the whole band, and
        that swing is precisely what makes "wait" a real move and makes
        a quiet moment worth abandoning. Leaving it out would have
        quietly deleted the reason timing matters.
        """
        block = int(t // (SLOT_S * BLOCK))
        return 0.35 + 1.5 * (_u(self.seed, "open", block) ** 1.4)

    def on_air(self, call: str, t: float) -> bool:
        """On the air in the slot containing `t`.

        Decided per BLOCK of slots rather than per slot, which is what
        makes a station STAY on for a while instead of flickering --
        58% still there half an hour later, as measured. Sessions are
        the reason waiting can ever be a sensible move, so getting this
        wrong would quietly delete one of the strategy's options.
        """
        block = int(t // (SLOT_S * BLOCK))
        key = (call, block)
        got = self._air.get(key)
        if got is None:
            got = (_u(self.seed, "onair", call, block)
                   < self.up_rate(call) * self.openness(t))
            self._air[key] = got
        return got

    def on_air_between(self, call: str, start: float, end: float) -> bool:
        t = start
        while t <= end:
            if self.on_air(call, t):
                return True
            t += SLOT_S * BLOCK
        return self.on_air(call, end)

    # ---- what a station would say if asked ----------------------------

    def neighbours(self, call: str, t: float, limit: int = 4) -> list:
        """What it would list in answer to HEARING? -- the stations it
        hears that are actually on the air right now."""
        key = (call, int(t // (SLOT_S * BLOCK)))
        got = self._nbr.get(key)
        if got is None:
            got = []
            for c in self.calls:
                if c != call and self.hears(call, c) and self.on_air(c, t):
                    got.append(c)
                    if len(got) >= limit:
                        break
            self._nbr[key] = got
        return got

    def audible_now(self, t: float) -> list:
        return [c for c in self.calls if self.on_air(c, t)]


class InventedTruth:
    """Ground truth for one trial, with the same questions the replayed
    history answers -- so the strategy runner cannot tell them apart."""

    def __init__(self, band: Band, target: str):
        self.band = band
        self.target = target

    def on_air(self, call, start, end) -> bool:
        """Transmitting -- whether or not WE can copy it."""
        return self.band.on_air_between(call, start, end)

    def audible(self, call, start, end) -> bool:
        """Transmitting AND the path to us is open right now. The
        distinction does not exist in the mined log, where a record
        only exists because we copied it, but it is the whole point of
        a relay case: the target is on the air and we cannot hear a
        thing."""
        return (self.band.hears_now(ME, call, start)
                and self.band.on_air_between(call, start, end))

    def target_up(self, start, end) -> bool:
        return self.band.on_air_between(self.target, start, end)

    def hears(self, a, b, start, end) -> bool:
        return (self.band.hears_now(a, b, start)
                and self.band.on_air_between(a, start, end))

    def neighbours(self, call, when) -> list:
        return self.band.neighbours(call, when)


class InventedModel:
    """What we BELIEVE about the band -- which is not what is true.

    The first version returned exact truth, justified as isolating the
    strategy from errors in the belief model. It did the reverse: it
    handed the strategy a perfect oracle, so the search leaned on
    "aim at the strongest station" -- infallible in an invented band,
    a guess on a real one. That strategy then reached 5.8% of invented
    targets and 2.7% of real ones, worse than simply calling.

    So beliefs here are a DEGRADED view, the way real ones are. Every
    belief is the truth seen through two failures that the mined log
    has for real:

      NOT EVERYTHING IS OBSERVED. We only know a station hears another
      because somebody published it. A real link nobody reported looks
      exactly like no link.
      WHAT IS OBSERVED GOES STALE. It was true when it was reported;
      the band has moved since.

    Getting who-to-aim-at right has to be part of the problem, because
    on a real band it is most of the problem.
    """

    # Share of true links nobody ever reported, so we cannot know them.
    UNSEEN = 0.45
    # Share of believed links that have since stopped being true.
    STALE = 0.25

    def __init__(self, band: Band):
        self.band = band
        self.mycall = ME

    def p_ans(self, call):
        # Answer rates come from probes we really sent, so this one is
        # roughly known -- but never exactly.
        true = self.band.answer_rate(call)
        wob = (_u(self.band.seed, "ansbelief", call) - 0.5) * 0.3
        return _Belief(min(0.95, max(0.05, true + wob)))

    def p_fwd(self, call):
        true = self.band.forward_rate(call)
        wob = (_u(self.band.seed, "fwdbelief", call) - 0.5) * 0.35
        return _Belief(min(0.95, max(0.05, true + wob)))

    def _believed(self, a, b) -> float:
        """Do we think a hears b?"""
        if self.band.hears(a, b):
            if _u(self.band.seed, "unseen", a, b) < self.UNSEEN:
                return 0.05          # true, but nobody ever reported it
            return 0.9
        if _u(self.band.seed, "stale", a, b) < self.STALE * 0.12:
            return 0.7               # we believe a link that has gone
        return 0.05

    def p_hears_us(self, call):
        return _Belief(self._believed(call, ME))

    def p_copy(self, call, _window):
        true = self.band.up_rate(call)
        wob = _u(self.band.seed, "copybelief", call) * 0.6 + 0.7
        return _Belief(min(0.95, true * wob))

    def p_link(self, hearer, heard):
        return _Belief(self._believed(hearer, heard))


class _Belief:
    __slots__ = ("p", "why")

    def __init__(self, p):
        self.p = p
        self.why = []


class InventedBoard:
    """Pool, aim orderings and delivery chains for an invented band."""

    def __init__(self, band: Band, target: str, max_hops: int = 4,
                 now: float | None = None):
        self.band = band
        self.target = target
        self.now = now
        self.chain = self._chains(target, max_hops)
        # Stations we could actually raise. BOTH ways, not just "they
        # hear us": we only ever LEARN that a station hears us by
        # decoding the message in which it says so, which means every
        # partner we know about is already two-way. Listing one-way
        # stations as candidates and then failing them on the return
        # leg threw away 94% of the pool for stations that would never
        # have been candidates in the first place -- and left the
        # invented band with no working relay at all.
        # ... and ON THE AIR. Candidates were being assembled from the
        # standing who-hears-whom graph with no regard for whether they
        # were awake, so 2,443 of 2,850 relay attempts died on "not on
        # the air" and three worked. No operator does this: you relay
        # through somebody you can hear right now. The historical model
        # already filtered on `last_heard`; this one did not, and the
        # gap made relaying look useless in the invented band.
        def awake(c):
            return now is None or band.on_air_between(c, now, now + 1800)

        def workable(c):
            if now is None:
                return band.hears(c, ME) and band.hears(ME, c)
            return (band.hears_now(c, ME, now)
                    and band.hears_now(ME, c, now))

        pool = [c for c in band.calls
                if c != target and c != ME and workable(c) and awake(c)]
        extra = [c for c in self.chain
                 if c not in pool and c != ME and c != target and awake(c)]
        extra.sort(key=lambda c: (len(self.chain[c]), -band.up_rate(c)))
        self.pool = pool + extra[:40]
        self.hears_us = {c: (0.9 if band.hears(c, ME) else 0.05)
                         for c in self.pool}
        self.order = {
            "strongest": sorted(self.pool, key=lambda c: -self.hears_us[c]),
            "freshest":  sorted(self.pool, key=lambda c: -band.up_rate(c)),
            "nearest":   sorted(self.pool,
                                key=lambda c: not band.hears(target, c)),
            "forwarder": sorted(self.pool, key=lambda c: -band.forward_rate(c)),
            "unknown":   sorted(self.pool,
                                key=lambda c: band.hears(target, c)),
            "learned":   list(self.pool),
        }

    def _chains(self, target: str, max_hops: int) -> dict:
        """Backwards from the target, same as the historical version:
        the target has to HEAR the last station for delivery to work."""
        b = self.band
        seen = {target}
        chains: dict = {}
        frontier = {target: []}
        for _hop in range(max_hops):
            nxt: dict = {}
            for station, tail in frontier.items():
                for c in b.calls:
                    if c in seen or c == ME or not b.hears(station, c):
                        continue
                    seen.add(c)
                    nxt[c] = [c] + tail
                    chains.setdefault(c, [c] + tail)
                if len(nxt) > 400:
                    break
            if not nxt:
                break
            frontier = nxt
        return chains


def trial(seed: int, n_stations: int = 220, relay_only: bool = False,
          cold: bool = False):
    """One invented situation: a band, a target worth trying, and the
    ground truth. The target is drawn from stations that are reachable
    in PRINCIPLE -- somebody can deliver to them -- because a target
    nobody on earth could reach measures nothing about strategy.
    """
    band = Band(seed, n_stations)
    base_t = 1_700_000_000 + (seed % 5000) * 1800
    for i in range(n_stations):
        cand = band.calls[(seed + i * 7) % n_stations]
        if cand == ME:
            continue
        if relay_only and band.hears(ME, cand) \
                and band.strength(ME, cand) > 0.6:
            continue        # a solid path to them: a relay is not the
                            # question. A MARGINAL one still counts --
                            # that is exactly the case where a relay
                            # beats waiting for the band.
        board = InventedBoard(band, cand)
        if not board.chain:
            continue
        if cold:
            # MATCH THE REAL COLD SAMPLER. It draws targets with more
            # than 20 decodes, at moments the band was busy -- a
            # station somebody might plausibly ask for, when there is
            # traffic about. Drawing any station at any moment made
            # the invented cold case far harsher than the real one
            # (0.27% reach against 2.2%), and the search duly decided
            # the best move was to quit before transmitting. The exam
            # threw that out, but the test should not have taught it.
            if band.up_rate(cand) < 0.02:
                continue                    # too rare to be asked for
            if len(band.audible_now(base_t)) < 4:
                continue                    # band is dead; not the case
            # THE COLD CASE: band open, and nobody knows whether this
            # particular station is up. Andy 2026-08-23: "you might not
            # have that kind of information... you still have to be
            # able to make a choice and try <something>." So the moment
            # is NOT chosen to suit the target -- that is the whole
            # point, and it is the one case where giving up early can
            # be the right move.
            here = InventedBoard(band, cand, now=base_t)
            if not here.pool:
                continue
            return band, cand, base_t, InventedModel(band), here, \
                InventedTruth(band, cand)
        # PICK A MOMENT THE TARGET IS ACTUALLY ON THE AIR. The real
        # relay sampler only offers instants where somebody reported
        # hearing the target, so the target is up by construction; an
        # invented trial that picks a moment blind finds the target
        # absent 92% of the time and measures nothing about strategy.
        # The two tests have to ask the same question.
        for k in range(48):
            t0 = base_t + k * 1800
            if not band.on_air_between(cand, t0, t0 + 1800):
                continue
            # Rebuild the pool for THIS moment: who is awake now.
            here = InventedBoard(band, cand, now=t0)
            if not here.pool:
                continue
            return band, cand, t0, InventedModel(band), here, \
                InventedTruth(band, cand)
    return None
