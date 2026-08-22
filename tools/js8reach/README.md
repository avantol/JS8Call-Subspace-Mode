# js8reach — reaching a station or a grid, offline, fastest-first

Plans the quickest way to raise a target station (or a station in /
nearest a target grid) over JS8 at Normal speed with no internet, using
only legacy JS8Call commands.

    python3 mine.py                     # build intel from the logs (~5 s)
    python3 cli.py --call KD7WPQ        # plan, with expected time
    python3 cli.py --grid DM79 --explain
    python3 sim.py --call KD7WPQ --trials 30
    python3 ../../tests_js8reach/test_js8reach.py

## Directional routing (`--route`)

Operator's rule: work out which way the target lies, find the furthest
station we can raise in that direction, ask what IT hears, hop, repeat.
Greedy geographic routing, with `HEARING?` as the primitive that sees
past our own horizon. Progress is `dist(us,T) - dist(hop,T)`, so a
station in the wrong direction is negative and drops out — no bearing
cone needed.

For a Maine target from Montana this finds a station in the target's
OWN grid square (14 km away) that no observed edge in five months of
logs knew about. The same ordering now drives relay choice in the
default rules.

## How it decides (default: deterministic rules)

    1. call the target directly, up to 3 times   (68 s each -- cheapest)
    2. one @ALLCALL QUERY CALL sweep             (one TX asks everyone)
    3. relays, ordered: sweep-confirmed > we heard it this hour >
       nearest the target > known to forward
    4. a few more direct calls
    5. store-and-forward via the best relay

Written out in `rules.py`. No probabilities: the procedure only needs
an ORDER and a rule for when to switch tactics, and both are stated so
they can be argued with.

`--scored` selects the alternative p/t index policy below. It is kept
for research: its `p` is a SORT KEY built from recency and distance,
not a measured probability, and the two agree on the first move
whenever the target was heard recently.

## The scoring alternative (--scored)

Probes are sequential (half-duplex), so for candidate actions with
success probability `p` and duration `t`, expected time to first
success is minimized by ordering on **p/t descending** (adjacent-
exchange proof; verified against brute force in the tests). Information
actions (`@ALLCALL QUERY CALL T?`, `HEARING?`) have no success mass and
cannot be ranked on that scale, so they are placed by comparing whole
plans' E[T] instead. After every observation the plan is recomputed.

Costs mirror the app's own timing math (`ChunkedArq.h:276`,
`mainwindow.cpp:3086`): `t = P/2 + txFrames*P + (2+replyFrames)*P +
hops*90`, P = 15 s.

## Where the knowledge comes from

`mine.py` reads three sources the app writes but never reads back:

| source | yields |
|---|---|
| `DIRECTED.TXT` (120k frames, 5.5 months) | who was on air when, at what SNR, who works whom, HEARING lists, relay proof (`*DE*`) |
| `ALL.TXT` (93k of our own TX lines) | what we asked and when. **Not used for planning** (operator directive 2026-08-21) — kept for timing research and the simulator's latent draw |
| `JS8Call-grids.db` (#164) | positions |

Everything is scored, never filtered: decode garbage shows up as
singletons and the model discounts it.

## What drives a decision

**Recency and location, and nothing else.** `p_copy` is the larger of
(a) a fast-decaying recency term — a decode minutes ago is direct
evidence they are on the air and the path works, 6 h e-folding — and
(b) geometric plausibility from distance. There is deliberately NO
penalty for a station we have never decoded: that is the absence of
evidence, not evidence of a bad path, and the mission is usually to
reach someone we have never worked. Long-run decode counts and
hour-of-day rates are used only by `window.py`, to answer WHEN the
path is open. Per-station answer
history is deliberately excluded: the mission is reaching a station we
have likely never worked through relays we have likely never used, so
that term is the prior — and a constant multiplies every candidate
identically, so it cannot change a p/t ordering. Where history does
exist it is a few samples, whose only effect would be to perturb the
ranking on thin evidence.

Set `model.USE_ANSWER_HISTORY = True` to restore it.

## Known limits

* **Relay branch is under-evidenced.** Only ~590 third-party mentions
  exist in 5.5 months, so the simulator can validate the direct /
  activity model strongly but the relay ordering only weakly.
* **Answer behaviour is modelled, not replayed** — we never ran these
  probes historically. Rates come from the 2,114 probes we did run.
* **Relay capability is undiscoverable by any JS8 command**; `p_fwd`
  rests on observed forwards plus a prior.
* Slowly-varying station traits are not horizon-filtered in replay.
