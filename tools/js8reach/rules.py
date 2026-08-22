"""rules.py — the deterministic ranking, stated as rules.

Operator's framing (2026-08-21): "it's deterministic: create a list of
hop possibilities, stop when you find a good one, try it, if fail try
another list."

That is the whole algorithm, and it needs no probabilities — only an
ORDER and a rule for when to switch tactics. This module writes both
out explicitly so they can be read and argued with, instead of being
implied by a scoring function.

THE ORDER
  1. Target heard in the last 15 min      -> call it, up to 3 times
  2. Otherwise, one broadcast sweep       -> ask the whole band who
     (@ALLCALL QUERY CALL T?)                hears it, in one TX
  3. Relays, best-first. When a sweep has answered, its replies are
     the ONLY evidence used, ordered by:
        a. how recently IT heard the target   (the reply says: "6H")
        b. how well it hears the target       (the reply says: "-18")
        c. how close it is to the target      (geography still counts:
           a relay far from the target is a worse relay even if it
           answered)
     A station that answered the sweep has PROVEN its first hop by
     replying -- we decoded it seconds ago -- so first-hop SNR is a
     threshold, not a score.

     With no sweep replies, fall back to: heard this hour, then
     greatest PROGRESS toward the target (dist(us,T) - dist(relay,T)).

  NOT USED, deliberately (operator, 2026-08-21): whether a station has
  relayed before, how often it has answered us, how many times we have
  decoded it. "If it doesn't reply NOW, that's what matters." Past
  behaviour is not evidence about this minute, and the mission is
  usually a station and relays we have never used.
  4. A few more direct calls              (cheap; conditions change)
  5. Store-and-forward via the best relay -> delivery, not contact

WHY THIS ORDER
  * A direct call is the cheapest thing we can do (68 s vs 175 s for a
    relay), so it goes first whenever there is any reason to think the
    target is there.
  * The sweep is one transmission that evaluates EVERY relay candidate
    at once, so it precedes trying relays one at a time (2+ candidates
    always makes it worth it).
  * Relays are ordered by whether we can actually raise them first,
    and only then by how likely they are to hear the target: a perfect
    second hop is worthless without a first hop.
"""

from __future__ import annotations

import actions as A
import grid as G
import intel

RECENT_S = 900          # "heard just now"
WARM_S = 3600           # "heard this hour"
MAX_DIRECT_FIRST = 3    # calls before we change tactics
MAX_DIRECT_LATER = 3    # calls after the relays have been tried
MAX_RELAYS = 6


def _age(model, call: str) -> float:
    st = intel.station(model.db, call)
    if not st or not st["last_heard"]:
        return float("inf")
    return max(0.0, model.now - st["last_heard"])


def _km_to_target(model, call: str, target: str) -> float:
    ga, gb = model.grid_of(call), model.grid_of(target)
    if not (ga and gb and G.valid(ga) and G.valid(gb)):
        return float("inf")
    try:
        return G.distance_km(ga, gb)
    except ValueError:
        return float("inf")


def _progress(model, call: str, target: str) -> float:
    """How much of the path this station covers, in the right
    direction: dist(us,T) - dist(relay,T). Negative = wrong way."""
    tg = model.grid_of(target)
    if not (model.mygrid and tg and G.valid(model.mygrid) and G.valid(tg)):
        return 0.0
    try:
        base = G.distance_km(model.mygrid, tg)
    except ValueError:
        return 0.0
    d = _km_to_target(model, call, target)
    if d == float("inf"):
        return 0.0
    return base - d


def parse_age(text: str) -> float:
    """'NOW' / '15S' / '6H' / '3D' -> seconds, as JS8 formats them."""
    t = (text or "").strip().upper()
    if not t or t == "NOW":
        return 0.0
    try:
        n = float(t[:-1])
    except ValueError:
        return float("inf")
    return n * {"S": 1, "M": 60, "H": 3600, "D": 86400}.get(t[-1], 0)


def rank_relays(model, target: str, live_hearers: set[str] | None = None,
                limit: int = MAX_RELAYS,
                live_reports: dict[str, tuple[int, str]] | None = None,
                failed: set[str] | None = None,
                forwarded: set[str] | None = None) -> list[str]:
    """Rule 3: order relay candidates. No arithmetic, just the sort.

    `live_reports` maps call -> (snr_of_target, age_string) taken
    straight from the sweep replies ("KF0DRT: WM8Q YES -18 (6H)").
    When present it is the ONLY thing that orders the responders --
    they have already proven their first hop by answering.
    """
    live = set(live_hearers or set()) | set(live_reports or {})
    reports = live_reports or {}
    # LIVE evidence about the relay itself, worth more than anything
    # about its link to the target:
    #   forwarded = we watched it relay our traffic (first hop PROVEN)
    #   failed    = we asked and it never keyed (first hop broken NOW:
    #               deaf to us, relay off, or not there)
    # A relay that cannot take our traffic is useless however well it
    # hears the target -- the mistake that nearly sent VA3NB's
    # store-and-forward to KF0DRT minutes after it ignored us.
    fwd = set(forwarded or set())
    bad = set(failed or set())
    pool = model.relay_candidates(target, limit=40)
    # Directional candidates: stations that cover real distance toward
    # the target. These are often invisible to a "near the target" or
    # "observed hearing the target" pool -- for a Maine target they
    # found a station in the target's OWN grid square that no observed
    # edge knew about.
    import route as _R
    for h in _R.forward_stations(model, target, limit=10):
        if h.call not in pool:
            pool.append(h.call)
    for h in live:
        if h not in pool:
            pool.append(h)

    def key(c: str):
        tier = 0 if c in fwd else (2 if c in bad else 1)
        if c in reports:
            snr, age = reports[c]
            # Live evidence only: when did it hear the target, how
            # well, and how far is it from the target. No history.
            # Age is BUCKETED (hour / 6 h / day / older) because JS8
            # quantizes it coarsely and a 14H-vs-15H difference is
            # noise -- ordering strictly on it let a station 370 km
            # further away with 4 dB worse copy outrank a better one
            # (VA3NB, 2026-08-21).
            a = parse_age(age)
            bucket = (0 if a <= 3600 else 1 if a <= 6 * 3600
                      else 2 if a <= 86400 else 3)
            return (tier, 0, bucket, -snr,
                    _km_to_target(model, c, target))
        return (
            tier,
            1 if c in live else 2,                    # answered, no detail
            0 if _age(model, c) <= WARM_S else 1,     # can we raise it
            -_progress(model, c, target),             # most progress
            _km_to_target(model, c, target),
        )

    return sorted(pool, key=key)[:limit]


# If the FRESHEST station reporting the target is older than this, the
# target is probably not transmitting at all -- and no relay fixes
# that. Learned the hard way (VA3NB, 2026-08-21): seven stations
# answered the sweep, the freshest had heard him 6 h earlier, and two
# relay attempts (~6 min of airtime) discovered exactly what that
# number already said. KJ7VWV even forwarded correctly; the target
# simply was not there.
STALE_TARGET_S = 2 * 3600


def triage(live_reports):
    # Read the sweep replies as a verdict on the TARGET, not just as a
    # relay menu. Returns (target_probably_absent, explanation).
    if not live_reports:
        return False, ""
    ages = {c: parse_age(a) for c, (_s, a) in live_reports.items()}
    freshest_call = min(ages, key=ages.get)
    freshest = ages[freshest_call]
    if freshest > STALE_TARGET_S:
        hrs = freshest / 3600.0
        return True, (
            f"{len(live_reports)} station(s) answered, but the FRESHEST "
            f"sighting is {hrs:.0f}h old ({freshest_call}) -- nobody has "
            f"heard the target recently, so it is probably not on the "
            f"air. Relaying cannot fix that; hand the traffic to a relay "
            f"for later delivery instead.")
    return False, (f"freshest sighting {freshest / 60:.0f} min old "
                   f"({freshest_call}) -- the target is active")


def plan(model, target, message=None, live_hearers=None,
         allow_broadcast=True, live_reports=None, failed=None,
         forwarded=None):
    # The ordered list of things to try, by the rules above.
    out = []

    absent, verdict = triage(live_reports)
    if absent:
        relays = rank_relays(model, target, live_hearers, MAX_RELAYS,
                             live_reports, failed, forwarded)
        if relays:
            return [A.store_and_forward(
                relays[0], target, message or "<your traffic>", 0.0,
                [verdict])]
    age = _age(model, target)

    if age <= RECENT_S:
        why = [f"{target} heard {age / 60:.0f} min ago -> rule 1: call it"]
        out += [A.ping(target, 0.0, why) for _ in range(MAX_DIRECT_FIRST)]
    else:
        # A direct call is the cheapest action we have (68 s vs 175 s),
        # so make a few before changing tactics even when the target
        # has not been heard recently — conditions change minute to
        # minute and the cost of being wrong is small.
        out += [A.ping(target, 0.0,
                       [f"{target} last heard {age / 3600:.1f}h ago -> "
                        f"cheap calls before changing tactics"])
                for _ in range(MAX_DIRECT_FIRST)]

    relays = rank_relays(model, target, live_hearers, MAX_RELAYS,
                         live_reports, failed, forwarded)
    if allow_broadcast and len(relays) >= 2:
        out.append(A.broadcast_query_call(
            target, 0.0,
            [f"rule 2: one TX evaluates {len(relays)} relay candidates"]))

    for r in relays:
        km = _km_to_target(model, r, target)
        prog = _progress(model, r, target)
        a = _age(model, r)
        out.append(A.relay_ping(
            r, target, 0.0,
            [f"rule 3: {r} heard {a / 3600:.1f}h ago, covers "
             f"{prog:.0f} km toward {target}, {km:.0f} km still to run"]))

    out += [A.ping(target, 0.0, ["rule 4: conditions change; call again"])
            for _ in range(MAX_DIRECT_LATER)]

    if message and relays:
        out.append(A.store_and_forward(
            relays[0], target, message, 0.0,
            [f"rule 5: hand it to {relays[0]} for delivery"]))
    return out
