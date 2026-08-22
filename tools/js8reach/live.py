"""live.py — read the Spots Map's live state instead of guessing at it.

This is the client side of TribbleNet, the live who-hears-whom mesh.

The map is the point. It already knows who is on the air this minute,
where they are, and who is hearing whom — from radio AND the internet
spot feed. Until now none of that was reachable by tooling, so the
planner reconstructed a worse, staler version of it from five months of
log files. `RX.GET_SPOT_MAP` (Build 371.2, debug/test surface) hands it
over directly.

Everything here is CURRENT evidence, which is the only kind that ranks
a route: a station spotted 4 minutes ago by a station 200 km from the
target beats any amount of archaeology.

    from live import LiveMap
    lm = LiveMap.fetch()                  # one API round trip
    lm.active(within_s=1800)              # who is on the air now
    lm.hearers_of("K2AY")                 # who is hearing the target NOW
"""

from __future__ import annotations

import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from js8client import Js8Client  # noqa: E402


@dataclass
class Sighting:
    call: str
    grid: str
    age_s: float
    snr: int
    # NOTE: there is deliberately no `heard_by` here. A spot names ONE
    # station; who heard it is a relationship, and relationships live in
    # the hearing store, which holds all of them instead of one. The app
    # stopped filling HEARD_BY when the map became observation-based
    # (Build 372) and nothing was lost, because every spot is derived
    # from that same store. Ask hearers_of() / reports_by() instead.
    pskr: bool = False          # internet-sourced
    rx_only: bool = False
    # [2026-08-21] TRUE = this spot is a report OF MY SIGNAL, filed by
    # `call`. Then `call` is the REPORTER and `snr` is how well IT
    # hears ME -- it says nothing about `call` transmitting. Missing
    # this field made is_active() announce "its transmission was
    # decoded here at -5" for N9EAT, a station we have never decoded
    # in 5.5 months; the -5 was N9EAT's report of US.
    reports_me: bool = False


# Identity lives in ONE place now (callsign.py). Kept importable as
# live.base so existing callers do not care where it moved.
from callsign import base, same  # noqa: E402,F401


@dataclass
class LiveMap:
    band: str = ""
    my_call: str = ""
    my_grid: str = ""
    grids: dict = field(default_factory=dict)
    spots: list = field(default_factory=list)      # Sighting
    hearing: list = field(default_factory=list)    # raw HEARING entries

    @classmethod
    def fetch(cls, band: str = "", timeout: float = 15.0) -> "LiveMap":
        with Js8Client() as js8:
            r = js8.request("RX.GET_SPOT_MAP", reply_type="RX.SPOT_MAP",
                            timeout=timeout)
        p = r.get("params", {})
        if p.get("ERROR"):
            raise RuntimeError(p["ERROR"])
        lm = cls(band=p.get("BAND", ""), my_call=p.get("MY_CALL", ""),
                 my_grid=p.get("MY_GRID", ""),
                 grids={k.upper(): v for k, v in
                        (p.get("GRIDS") or {}).items()},
                 hearing=p.get("HEARING") or [])
        seen = set()
        for key in ("SPOTS_ALL", "SPOTS_MINE"):
            for s in p.get(key) or []:
                ident = (s.get("CALL"), s.get("WHEN"))
                if ident in seen:
                    continue
                seen.add(ident)
                lm.spots.append(Sighting(
                    call=(s.get("CALL") or "").upper(),
                    grid=s.get("GRID") or "",
                    age_s=float(s.get("AGE_S", -1)),
                    snr=int(s.get("SNR", -99)),
                    pskr=bool(s.get("PSKR")),
                    rx_only=bool(s.get("RX_ONLY")),
                    reports_me=bool(s.get("REPORTS_ME"))))
        return lm

    # ---- the questions worth asking --------------------------------

    def active(self, within_s: float = 1800) -> list[Sighting]:
        """Stations seen on the air within the window, freshest first."""
        out = [s for s in self.spots if 0 <= s.age_s <= within_s]
        out.sort(key=lambda s: s.age_s)
        return out

    def is_active(self, call: str, within_s: float = 1800
                  ) -> tuple[str, float, str] | None:
        """(evidence, age_s, detail) if `call` is demonstrably on the
        air, else None.

        The distinction that matters is what the station has DONE,
        not how we came to know it (operator, 2026-08-21):

          "transmitting"  somebody decoded a TRANSMISSION from it --
                          our own radio or anyone else's report, it
                          makes no difference. It keys up, so it can
                          be worked.
          "listening"     it appears only as the REPORTER of other
                          stations. Its receiver is provably live,
                          which is the precondition for hearing our
                          call -- but nobody has heard it transmit
                          SO FAR, so it may be receive-only,
                          unattended, or simply quiet.

        Both are worth calling; only the first is evidence anyone will
        answer. Missing the second entirely cost a planning round:
        K2AY was plainly on the map as the reporter of two east-coast
        stations while this function reported "no spot within 90 min",
        because it only ever looked at the plotted call.
        """
        c = base(call)
        best = None
        for s in self.spots:
            if not (0 <= s.age_s <= within_s):
                continue
            if base(s.call) == c and s.reports_me:
                # It heard US. Proves its receiver is live AND gives a
                # delivery edge, but not that it transmits.
                cand = ("listening", s.age_s,
                        f"it reported hearing ME"
                        + (f" at {s.snr:+d}" if s.snr > -99 else "")
                        + " -- our traffic REACHES it; nobody has"
                          " decoded it transmitting")
            elif base(s.call) == c:
                cand = ("transmitting", s.age_s,
                        "its transmission was decoded"
                        + (f" at {s.snr:+d}" if s.snr > -99 else ""))
            else:
                continue
            # "transmitting" always outranks "listening", however much
            # fresher the listening evidence is: one says it can be
            # worked, the other only that it might hear us.
            rank = 0 if cand[0] == "transmitting" else 1
            if best is None or (rank, cand[1]) < (best[0], best[1]):
                best = (rank, cand[1], cand)
        # The hearing store is a third witness, and it was being ignored
        # entirely: if somebody DECODED this station, it transmitted,
        # whether or not it was ever plotted as a spot. Only edges with
        # a real SNR count -- a -99 edge may be nothing more than
        # someone addressing it (see hearers_of).
        for h in self.hearing:
            hearer = (h.get("CALL") or "").upper()
            # (a) the target as the HEARD party: somebody decoded it, so
            #     it transmitted.
            for e in h.get("HEARS") or []:
                if base(e.get("CALL") or "") != c:
                    continue
                age = float(e.get("AGE_S", -1))
                snr = int(e.get("SNR", -99))
                if not (0 <= age <= within_s) or snr <= -99:
                    continue
                cand = ("transmitting", age,
                        f"{hearer} decoded it at {snr:+d}")
                if best is None or (0, age) < (best[0], best[1]):
                    best = (0, age, cand)
            # (b) the target as the HEARER: it reported hearing somebody,
            #     so its receiver is live even though nothing has decoded
            #     IT. This used to come from a spot's HEARD_BY; when that
            #     field went away the verdict went with it, and only the
            #     "it hears ME" case above survived. The same evidence is
            #     right here in the hearing store, from the other side --
            #     it just was not being read.
            if base(hearer) != c:
                continue
            for e in h.get("HEARS") or []:
                age = float(e.get("AGE_S", -1))
                if not (0 <= age <= within_s):
                    continue
                snr = int(e.get("SNR", -99))
                cand = ("listening", age,
                        f"it reported hearing {e.get('CALL') or '?'}"
                        + (f" at {snr:+d}" if snr > -99 else "")
                        + " -- receiver live, not heard transmitting"
                          " so far")
                if best is None or (1, age) < (best[0], best[1]):
                    best = (1, age, cand)
        return best[2] if best else None

    def reports_by(self, call: str, within_s: float = 3600
                   ) -> list[tuple[str, float, int]]:
        """What `call` has been heard reporting: (station, age, snr).
        These are stations IT can hear -- the far side of a relay."""
        c = base(call)
        out: dict = {}
        # The hearing store is the ONLY source now. It always held every
        # one of these edges; the spot loop that used to run first could
        # only ever restate a subset (2026-08-21: omitting the store made
        # "what are WE hearing?" report 1 station when the map held 30,
        # which I then misread as a weak receiver).
        for h in self.hearing:
            if base(h.get("CALL") or "") != c:
                continue
            for e in h.get("HEARS") or []:
                age = float(e.get("AGE_S", -1))
                if not (0 <= age <= within_s):
                    continue
                k = base(e.get("CALL") or "")
                if k and (k not in out or age < out[k][0]):
                    out[k] = (age, int(e.get("SNR", -99)))
        return sorted(((k, v[0], v[1]) for k, v in out.items()),
                      key=lambda t: t[1])

    def transmitters(self) -> set:
        """BASE calls the map has seen KEY UP. Base, so AL0A/P counts
        as AL0A -- see base()."""
        # reports_me spots are reports of OUR signal: the call is the
        # reporter, not a transmitter. Counting them here would let a
        # station that has never keyed pass as "it keys up".
        return {base(s.call) for s in self.spots
                if s.call and not s.reports_me}

    def literal_call(self, call: str) -> str:
        """The exact callsign this operator is USING right now
        (affix included), for addressing. Falls back to what we were
        given. Freshest sighting wins."""
        b = base(call)
        best = None
        for s in self.spots:
            if s.call and base(s.call) == b and s.age_s >= 0:
                if best is None or s.age_s < best[0]:
                    best = (s.age_s, s.call)
        return best[1] if best else call.upper()

    def live_copiers(self, call: str, within_s: float = 3600) -> list:
        """(hearer, age_s, snr) for everyone copying `call` RIGHT NOW.

        This is the honest, live-only replacement for judging a relay's
        transmit strength. I had reached for 5.5 months of reception
        reports to call AC7WY 'low power'; Andy: "you're breaking the
        rules by looking past an hour!" -- and he is right twice over,
        because the operational question is not the station's power but
        whether its FORWARD can be copied right now, which is power AND
        propagation together. The map window answers exactly that."""
        return self.hearers_of(call, within_s)

    def hearers_of(self, call: str, within_s: float = 3600
                   ) -> list[tuple[str, float, int]]:
        """(hearer, age_s, snr) for stations currently hearing `call`.

        Source: the hearing store's per-edge records. (There was a
        second source -- a spot's `HEARD_BY` -- until Build 372. It only
        ever restated a subset of these, because the app derives its
        spots from this same store.)

        PHANTOM EDGES ARE FILTERED HERE. The app builds hearing edges
        from A *addressing* B as well as from A *decoding* B (TODO
        #167), so the store will happily claim "A hears B" on the
        strength of a call B never answered. Those edges are
        recognisable: no SNR was ever measured (-99), because nothing
        was ever decoded.

        This is not theoretical. 2026-08-21, routing to K2AY: we sent
        `AE0YH>K2AY SNR?`, AE0YH forwarded `K2AY> SNR? *DE* WM8Q`, and
        the map immediately grew an "AE0YH hears K2AY" edge -- SNR -99,
        timestamped to the second of the forward. OUR OWN relayed probe
        manufactured the evidence that our return leg was working. A
        planner trusting it would report the route complete having
        heard nothing from the target at all.

        An edge survives only if the SNR is real (something was
        decoded), or the heard station has been seen transmitting
        somewhere on the map (so it demonstrably keys up).
        """
        c = base(call)
        out: dict[str, tuple[float, int]] = {}
        keys_up = c in self.transmitters()
        for h in self.hearing:
            hearer = (h.get("CALL") or "").upper()
            for e in h.get("HEARS") or []:
                if base(e.get("CALL") or "") != c:
                    continue
                age = float(e.get("AGE_S", -1))
                if not (0 <= age <= within_s):
                    continue
                snr = int(e.get("SNR", -99))
                if snr <= -99 and not keys_up:
                    continue          # phantom: addressed, not decoded
                prev = out.get(hearer)
                if prev is None or age < prev[0]:
                    out[hearer] = (age, snr)
        return sorted(((k, v[0], v[1]) for k, v in out.items()),
                      key=lambda t: t[1])

    def grid_of(self, call: str) -> str:
        return self.grids.get(call.upper(), "")


def _main() -> int:
    import argparse
    ap = argparse.ArgumentParser(
        description="Dump the Spots Map's live state")
    ap.add_argument("--call", help="who is hearing this station now?")
    ap.add_argument("--within-min", type=float, default=60.0)
    args = ap.parse_args()
    lm = LiveMap.fetch()
    print(f"band {lm.band}  me {lm.my_call}/{lm.my_grid}  "
          f"{len(lm.spots)} spots, {len(lm.hearing)} hearing entries, "
          f"{len(lm.grids)} grids")
    if args.call:
        w = args.within_min * 60
        me = lm.is_active(args.call, w)
        if me:
            kind, age, detail = me
            verdict = ("HEARD TRANSMITTING" if kind == "transmitting"
                       else "ONLY KNOWN TO BE LISTENING (so far)")
            print(f"\n{args.call.upper()}: {verdict}, "
                  f"{age / 60:.0f} min ago -- {detail}")
            rb = lm.reports_by(args.call, w)
            if rb:
                print(f"  it is currently hearing:")
                for st, a, snr in rb[:6]:
                    print(f"     {st:10s} {a / 60:4.0f} min ago  "
                          f"snr {snr if snr > -99 else '?'}")
        else:
            print(f"\n{args.call.upper()}: no evidence on the map "
                  f"within {args.within_min:.0f} min")
        hs = lm.hearers_of(args.call, w)
        if hs:
            print(f"currently hearing {args.call.upper()}:")
            for h, age, snr in hs:
                print(f"   {h:10s} {age / 60:5.0f} min ago  snr "
                      f"{snr if snr > -99 else '?'}  "
                      f"grid {lm.grid_of(h) or '?'}")
        else:
            print("nobody on the map is currently hearing it")
    else:
        print("\nactive now (freshest first):")
        for s in lm.active(args.within_min * 60)[:15]:
            src = "pskr" if s.pskr else "radio"
            print(f"   {s.call:10s} {s.grid:9s} {s.age_s / 60:5.0f} min "
                  f"{src}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
