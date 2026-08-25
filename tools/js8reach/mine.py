"""mine.py — build the js8reach intel DB from logs the app throws away.

Sources (all read-only):
  ~/.local/share/JS8Call/DIRECTED.TXT   every directed frame we decoded:
        "<date time>\t<dial MHz>\t<offset>\t<snr>\t<FROM>: <rest> [diamond]"
  ~/.local/share/JS8Call/ALL.TXT        adds OUR OWN transmissions
        ("<date time>  Transmitting <dial> MHz  JS8:  <text>") — the only
        record of what we asked and when, hence the only honest basis
        for measured answer rates and latencies.
  ~/.config/JS8Call-grids.db            the app's grid bank (#164), for
        positions; never written.

Derived evidence:
  sighting     we decoded X at SNR s at time t          -> stations, activity, sightings
  reverse SNR  "<X>: <MYCALL> SNR -06"                  -> how X hears US
  edge         "<X>: <Y> HEARING A B C D"               -> X hears A,B,C,D
               "<X>: <Y> ..." (any directed frame)      -> X works Y (weak edge)
               relayed "... *DE* Z"                     -> Z is the true hearer
  relay proof  X transmitted a frame containing "*DE*"
               or a "CALL>" relay head                  -> X forwards
  probe        our TX "<MYCALL>: <T> <CMD>?" at t, and whether T sent us
               anything within the reply window         -> p_ans, latency

Robustness: JS8 decodes include garbage (bit errors that still pass the
frame CRC produce plausible-looking junk callsigns). We never reject on
looks alone — we COUNT corroboration (`edges.n`, `stations.heard_count`)
and let the model discount singletons.
"""

from __future__ import annotations

import argparse
import re
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import intel  # noqa: E402

LOG_DIR = Path.home() / ".local" / "share" / "JS8Call"
DIRECTED = LOG_DIR / "DIRECTED.TXT"
ALLTXT = LOG_DIR / "ALL.TXT"
GRIDS_DB = Path.home() / ".config" / "JS8Call-grids.db"

# A callsign as JS8 packs it: letters/digits with at least one digit,
# optional /P /M /QRP style affix. Deliberately permissive — scoring,
# not filtering, separates real stations from decode garbage.
# ONE character may precede the digit, not two. Written {2,}, this
# rejected every 1x2 and 1x3 callsign -- K2AY, W4CAT, K8IMT, K1BRG --
# and every single-letter-prefix European one -- G0BMH, G3L, F4LNO.
# It is used by eleven patterns here including RE_FROM, which decides
# whether a decoded line is attributed to a sender at all, so a
# non-match silently discarded the line as a continuation frame.
#
# MEASURED 2026-08-25 over the whole log: 39,534 of 166,040 directed
# lines were being dropped -- 23.8% of five months of traffic -- along
# with 710 stations that appear in it and were never once recorded.
# Every figure derived from this corpus was computed without them.
CALLSIGN = r"[A-Z0-9]{1,3}[0-9][A-Z0-9]{1,5}(?:/[A-Z0-9]+)?"
RE_DIRECTED_LINE = re.compile(
    r"^(?P<date>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\t"
    r"(?P<dial>[\d.]+)\t(?P<offset>-?\d+)\t(?P<snr>[+-]?\d+)\t"
    r"(?P<text>.*)$")
RE_FROM = re.compile(rf"^(?P<from>{CALLSIGN}):\s+(?P<rest>.*)$")
RE_CALL = re.compile(CALLSIGN)
RE_DE = re.compile(rf"\*DE\*\s+(?P<de>{CALLSIGN})")
# "<S>: <A>> <rest>" -- S is asking A to pass something along. Note the
# space after the marker: "WD4KAV: WB7TSQ> AC7WY>KE0ZDH HEARING?".
RE_RELAY_ASK = re.compile(
    rf"^(?P<s>{CALLSIGN}):\s+(?P<a>{CALLSIGN})>\s*(?P<rest>.+)$")
RE_TX = re.compile(
    r"^(?P<date>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\s+"
    r"Transmitting\s+(?P<dial>[\d.]+)\s+MHz\s+\w+:\s+(?P<text>.*)$")
# Our own probe forms. The app logs frame TEXT, which carries the
# "<MYCALL>: " prefix only sometimes ("WM8Q: KD9KUA HEARING?" vs the
# bare "KB7ITU SNR?" — field-verified in ALL.TXT), so the prefix is
# optional here. Relay heads ("A>B QUERY CALL X?") are captured too.
RE_PROBE = re.compile(
    rf"^(?:(?P<me>{CALLSIGN}):\s+)?(?P<heads>(?:{CALLSIGN}>\s*)*)"
    rf"(?P<to>@?[A-Z0-9/]+)\s+(?P<cmd>SNR\?|GRID\?|HEARING\?|STATUS\?|"
    # NOT \b: every "?"-terminated alternative ends on a non-word
    # char, and \b between "?" and " " never matches (silently caught
    # 55 of 3,100 real probes before this was tested against the log).
    r"INFO\?|QUERY CALL|QUERY ARQ\?|QUERY MSGS)(?=\s|$)")

# Reply-window ceiling for crediting an answer to a probe: the app's own
# QUERY CALL window (mainwindow.h kQCallReplyWindowMs) = 300 s.
PROBE_WINDOW_S = 300

# Frames that only exist because somebody asked: the fingerprint of an
# always-on responder. "HEARTBEAT SNR" is an ACK to a heartbeat, i.e.
# a reply, unlike a bare "HEARTBEAT".
RE_REPLY_CMD = re.compile(
    r"^(?:HEARTBEAT\s+SNR|SNR|ACK|YES|NO|GRID|STATUS|INFO|QUERY\s+\w+)"
    r"(?:\s|$)")
# "<X>: <Y> SNR -06" and its heartbeat form: X hears Y, and this well.
RE_THIRD_PARTY_SNR = re.compile(
    r"^(?:HEARTBEAT\s+)?SNR\s+(?P<snr>[+-]?\d+)")

# Blind calls: asking someone a question proves nothing about whether
# you can hear them. Everything else addressed to a specific station
# is either a reply or conversation, and both imply reception.
RE_QUERY_CMD = re.compile(
    r"^(?:SNR\?|\?|HEARING\?|GRID\?|STATUS\?|INFO\?|AGN\?|QUERY|"
    r"HEARTBEAT|HB|CQ|MSG\b|DIT)(?:\s|$)")


def epoch(datestr: str) -> int:
    return int(datetime.strptime(datestr, "%Y-%m-%d %H:%M:%S")
               .replace(tzinfo=timezone.utc).timestamp())


# WHICH KINDS OF THIRD-PARTY EVIDENCE GET A TIMESTAMPED EVENT.
#
# An "edge" is the standing fact that X can hear Y; an "event" is the
# dated proof of it, and the replay simulator can only use the dated
# kind -- it has to know who could hear the target AT THAT MOMENT.
# Until 2026-08-23 only HEARING lists were kept as events, 591 of them,
# which left the relay case with 94 usable instants: far too few to
# judge a strategy on, and the search duly overfitted them.
#
#   replied    X answered Y, so X heard Y. Hard proof, dated, 25,984 of
#              them -- 44x the HEARING evidence, and it was all being
#              dropped here.
#   relayfrom  X put Z's traffic on the air behind a *DE*, so X heard
#              Z. Hard proof, dated.
#   hearing    X published a list of what it hears. Hard proof, dated.
#
# freetext is deliberately NOT here. X sending directed text to Y only
# proves X CALLED Y -- a blind call proves nothing about reception, and
# treating it as an edge is the same mistake as the blind-call edges
# killed off the map in #167.
EVENT_SOURCES = ("hearing", "replied", "relayfrom", "querycall")

# "<X>: <MYCALL> YES <snr> (<age>)" -- the answer to a QUERY CALL. It
# says X hears the station WE ASKED ABOUT, this well, this long ago.
# The closing paren is OPTIONAL. JS8 truncates a frame that runs out of
# room and marks it "~~~~~", so a perfectly readable answer arrives as
# "YES +14 (58M~~~~~". Requiring the paren threw away the strongest
# report of 2026-08-25 (KF5YWQ at +14) along with NT5DF and WB5BNV --
# the signal and the age are both fully legible, and the only thing
# missing is punctuation.
RE_QCALL_YES = re.compile(
    rf"^(?P<who>{CALLSIGN}):\s+(?P<me>{CALLSIGN})\s+YES\s+"
    rf"(?P<snr>[+-]\d{{1,3}})\s*\((?P<age>NOW|\d+[SMHD])")
# our own outgoing query, which names the target the YES refers to
RE_QCALL_TX = re.compile(rf"QUERY CALL\s+(?P<target>{CALLSIGN})")
RE_QCALL_TX_TAIL = re.compile(rf"^(?P<target>{CALLSIGN})\?")


class Miner:
    def __init__(self, db: sqlite3.Connection, mycall: str):
        self.db = db
        self.me = mycall.upper()
        self.me_base = self.me.split("/")[0]
        self.stations: dict[str, dict] = {}
        self.activity: dict[tuple[str, int], int] = {}
        self.edges: dict[tuple[str, str], dict] = {}
        self.sightings: list[tuple] = []
        self.probes: list[dict] = []
        self.rx_by_call: dict[str, list[int]] = {}
        self.edge_events: list[tuple] = []
        self.relay_asks: list[tuple] = []
        self.relay_fwds: dict = {}
        self.qcall_sends: list[tuple] = []    # (ts, target) we asked about
        self.qcall_replies: list[tuple] = []  # (ts, who, snr, ageSecs)
        self.skewed = 0
        self.now = int(datetime.now(timezone.utc).timestamp())

    # ---- accumulation -------------------------------------------

    def _st(self, call: str) -> dict:
        return self.stations.setdefault(call, {
            "first_heard": None, "last_heard": None, "heard_count": 0,
            "snr_n": 0, "snr_sum": 0, "snr_min": None, "snr_max": None,
            "rev_snr_n": 0, "rev_snr_last": None, "rev_snr_best": None,
            "rev_last": None, "relay_seen": 0, "to_us": 0, "grid": None,
            "resp_count": 0, "spont_count": 0,
            "relay_asked": 0, "relay_done": 0,
        })

    def is_me(self, call: str) -> bool:
        return call.split("/")[0] == self.me_base

    def sighting(self, call: str, ts: int, snr: int, dial: float,
                 offset: int) -> None:
        # Clock-skew guard: the logs carry the PC clock of the moment,
        # and a brief mis-set clock writes future-dated decodes (field
        # 2026-08-21: 3 lines stamped 9 days ahead made a station look
        # permanently "mid-session"). One hour of tolerance absorbs
        # ordinary drift; anything beyond is not evidence.
        if ts > self.now + 3600:
            self.skewed += 1
            return
        s = self._st(call)
        s["heard_count"] += 1
        s["first_heard"] = min(s["first_heard"] or ts, ts)
        s["last_heard"] = max(s["last_heard"] or ts, ts)
        s["snr_n"] += 1
        s["snr_sum"] += snr
        s["snr_min"] = snr if s["snr_min"] is None else min(s["snr_min"], snr)
        s["snr_max"] = snr if s["snr_max"] is None else max(s["snr_max"], snr)
        hour = datetime.fromtimestamp(ts, timezone.utc).hour
        key = (call, hour)
        self.activity[key] = self.activity.get(key, 0) + 1
        self.sightings.append((ts, call, snr, dial, offset))
        self.rx_by_call.setdefault(call, []).append(ts)

    def relay_request(self, asked: str, by: str, ts: int) -> None:
        """Somebody asked `asked` to pass traffic along. Whether it did
        is settled later, when we see a *DE* from it crediting `by`."""
        self._st(asked)["relay_asked"] += \
            0.5 ** ((self.now - ts) / (30 * 86400.0))
        self.relay_asks.append((ts, asked, by))

    def relay_forward(self, station: str, origin: str, ts: int) -> None:
        self.relay_fwds.setdefault(station, []).append((ts, origin))

    def harvest_call_queries(self) -> int:
        """Recover the QUERY CALL answers the app threw away.

        A reply "KF5YWQ: WM8Q YES +14 (58M)" states that KF5YWQ hears
        the station we asked about, at +14, 58 minutes before it spoke.
        That is first-hand, dated, third-party evidence -- the single
        richest routing input the mode produces, and the one the router
        most needs.

        None of it was ever recorded. #178: the app's capture matched
        an anchored pattern against one FRAME that also carried our own
        callsign prefix, so the pending query was never armed and the
        (correct) binder was never reached. 323 broadcasts with a
        recoverable target and 675 answers sat unused.

        Both halves survive in the logs -- our transmissions in ALL.TXT
        and the replies in DIRECTED.TXT -- so the pairing can be redone
        here. The target usually lands in the SECOND frame ("... QUERY
        CALL" / "AI5TS?"), which is exactly why the live capture failed,
        so the frames are stitched before matching.
        """
        got = 0
        for start, target in self.qcall_sends:
            for rt, who, snr, ageSecs in self.qcall_replies:
                if not (0 < rt - start <= PROBE_WINDOW_S):
                    continue
                self.edge(who, target, rt - ageSecs, snr, "querycall")
                got += 1
        return got

    def settle_relays(self) -> None:
        """Match each request to a forward within 15 minutes."""
        # Weighted like the probes: a request halves in weight every 30
        # days, so the counts stored are effective-recent counts and the
        # p_fwd formulas need no change. SQLite stores the floats fine.
        for ts, asked, by in self.relay_asks:
            w = 0.5 ** ((self.now - ts) / (30 * 86400.0))
            for ft, forigin in self.relay_fwds.get(asked, ()):
                if 0 <= ft - ts < 900 and forigin == by:
                    self._st(asked)["relay_done"] += w
                    break

    def edge(self, hearer: str, heard: str, ts: int, snr: int | None,
             source: str) -> None:
        if hearer == heard or self.is_me(heard):
            return
        e = self.edges.setdefault((hearer, heard), {
            "last_when": 0, "n": 0, "snr": None, "source": source})
        e["n"] += 1
        if source in EVENT_SOURCES:
            # Keep the individual sighting, not just the aggregate:
            # the simulator needs "who heard T at time t".
            self.edge_events.append((ts, hearer, heard, source))
        if ts > e["last_when"]:
            e["last_when"] = ts
            e["source"] = source
            if snr is not None:
                e["snr"] = snr

    def reverse_snr(self, call: str, snr: int, ts: int) -> None:
        s = self._st(call)
        s["rev_snr_n"] += 1
        s["rev_snr_last"] = snr
        s["rev_snr_best"] = (snr if s["rev_snr_best"] is None
                             else max(s["rev_snr_best"], snr))
        s["rev_last"] = ts

    # ---- DIRECTED.TXT -------------------------------------------

    def mine_directed(self, path: Path) -> int:
        n = 0
        with path.open(errors="replace") as fh:
            for line in fh:
                m = RE_DIRECTED_LINE.match(line.rstrip("\n"))
                if not m:
                    continue
                text = m["text"].replace("♦", " ").strip()
                fm = RE_FROM.match(text)
                if not fm:
                    continue  # continuation frame, no sender attribution
                sender = fm["from"].upper()
                rest = fm["rest"].strip()
                ts = epoch(m["date"])
                n += 1
                # [#178] "<X>: <MYCALL> YES <snr> (<age>)" -- the answer
                # to a QUERY CALL. Kept with its reported age so
                # harvest_call_queries() can pair it with the outgoing
                # query and backdate the edge properly. The age is the
                # whole point: a YES (15S) and a YES (19H) mean utterly
                # different things and the router had no way to tell.
                ym = RE_QCALL_YES.match(text.upper())
                if ym and self.is_me(ym["me"]):
                    a = ym["age"]
                    if a == "NOW":
                        secs = 0
                    else:
                        secs = int(a[:-1]) * {"S": 1, "M": 60,
                                              "H": 3600, "D": 86400}[a[-1]]
                    self.qcall_replies.append(
                        (ts, ym["who"].upper(), int(ym["snr"]), secs))
                if self.is_me(sender):
                    continue  # our own frames come from ALL.TXT
                self.sighting(sender, ts, int(m["snr"]),
                              float(m["dial"]), int(m["offset"]))

                # Relay proof: this station forwarded someone's traffic.
                de = RE_DE.search(rest)
                head = re.match(rf"^(?:{CALLSIGN}|@[A-Z0-9]+)>", rest)
                if de or head:
                    self._st(sender)["relay_seen"] += 1
                # Who was ASKED to relay, and by whom. A frame carrying
                # *DE* is the forward itself, not a request -- crediting
                # it as both would score every forward as its own answer.
                if de:
                    self.relay_forward(sender, de["de"].upper(), ts)
                else:
                    ask = RE_RELAY_ASK.match(text)
                    if ask and not self.is_me(ask["a"]):
                        self.relay_request(ask["a"].upper(),
                                           ask["s"].upper(), ts)
                if de:
                    # An overheard relay in flight: this station is
                    # forwarding traffic it RECEIVED from the *DE*
                    # call, so it hears that call. Third-party
                    # evidence we get for free by listening
                    # (operator, 2026-08-21).
                    src = de["de"].upper()
                    if src != sender and not self.is_me(src):
                        self.edge(sender, src, ts, None, "relayfrom")

                # True hearer for relayed payloads is the *DE* tail.
                hearer = de["de"].upper() if de else sender

                # Addressee = first token (strip relay heads).
                body = re.sub(rf"^(?:(?:{CALLSIGN}|@[A-Z0-9]+)>\s*)+", "",
                              rest)
                parts = body.split()
                if not parts:
                    continue
                to = parts[0].upper().rstrip(">")
                tail = " ".join(parts[1:])

                # Traffic profile. A station whose transmissions are
                # overwhelmingly REPLIES is a full-time listening
                # station: it only keys when asked, so its low
                # spontaneous rate says nothing about availability
                # (operator 2026-08-21: "ac7wy is a full-time tx/rx
                # station... no recent tx except maybe HB, but still
                # ready to relay"). Estimating such a station's
                # reachability from decode volume is exactly wrong.
                first = parts[1].upper() if len(parts) > 1 else ""
                st = self._st(sender)
                if RE_REPLY_CMD.match(tail.upper()):
                    st["resp_count"] += 1
                elif first.startswith("HEARTBEAT") or first in (
                        "HB", "CQ") or to.startswith("@"):
                    st["spont_count"] += 1
                else:
                    st["spont_count"] += 1

                if not to.startswith("@") and not self.is_me(to):
                    # An edge means "A CAN HEAR B". A directed frame
                    # only proves that when it is a REPLY: "A: B SNR
                    # +05", an ACK, a YES. A station CALLING another
                    # ("A: B SNR?") proves nothing about whether it
                    # hears B -- it is asking (operator, 2026-08-21:
                    # "was it only KJ7VWV *calling* VA3NB? if so, that
                    # doesn't count!"). Queries no longer create edges.
                    tu = tail.upper()
                    if RE_REPLY_CMD.match(tu):
                        # CAPTURE THE SIGNAL REPORT. "A: B SNR -06"
                        # does not merely prove A hears B, it says HOW
                        # WELL -- and 58,609 of these were being
                        # recorded with the number thrown away. It
                        # matters because a link that only just decodes
                        # fails on the first pass and costs a whole
                        # cycle to retry, so a route should prefer a
                        # hop with margin over one at the floor. 14% of
                        # the reports are below -18 dB.
                        m_snr = RE_THIRD_PARTY_SNR.match(tu)
                        self.edge(sender, to, ts,
                                  int(m_snr.group("snr")) if m_snr else None,
                                  "replied")
                    elif tail.strip() and not RE_QUERY_CMD.match(tu):
                        # Directed FREE TEXT: a conversation in
                        # progress, which is a live link -- nobody
                        # ragchews into the void (operator,
                        # 2026-08-21). Distinct from a blind query,
                        # which proves nothing.
                        self.edge(sender, to, ts, None, "freetext")

                if self.is_me(to):
                    self._st(sender)["to_us"] += 1
                    rm = re.search(r"\b(?:HEARTBEAT\s+SNR|SNR|ACK)\s+"
                                   r"([+-]\d{1,2})\b", tail)
                    if rm:
                        self.reverse_snr(sender, int(rm.group(1)), ts)

                # HEARING list -> hard edges (the richest source).
                hm = re.search(r"\bHEARING\b(?P<list>.*)$", tail)
                if hm:
                    listing = RE_DE.sub("", hm["list"])
                    for c in RE_CALL.findall(listing.upper()):
                        if c != hearer and not self.is_me(c):
                            self.edge(hearer, c, ts, None, "hearing")

                # GRID reply -> position (corroborates the grid bank).
                gm = re.search(r"\bGRID\s+([A-R]{2}\d{2}(?:[A-X]{2})?)\b",
                               tail.upper())
                if gm:
                    self._st(hearer)["grid"] = gm.group(1)
        return n

    # ---- ALL.TXT: our own probes + measured answers --------------

    def mine_probes(self, path: Path) -> int:
        sent: list[tuple[int, str, str]] = []
        with path.open(errors="replace") as fh:
            for line in fh:
                m = RE_TX.match(line.rstrip("\n"))
                if not m:
                    continue
                text = m["text"].replace("♦", " ").strip()
                # [#178] Our own QUERY CALL, for the harvest recovery.
                # The target usually lands in the SECOND frame ("...
                # QUERY CALL" / "AI5TS?") -- which is precisely why the
                # app's live capture failed -- so the frames are
                # stitched here across consecutive Transmitting lines.
                ts_tx = epoch(m["date"])
                up = text.upper()
                if "QUERY CALL" in up:
                    qm = RE_QCALL_TX.search(up)
                    if qm:
                        self.qcall_sends.append((ts_tx, qm["target"].upper()))
                        self._qcall_await = None
                    else:
                        self._qcall_await = ts_tx
                elif getattr(self, "_qcall_await", None) is not None:
                    tm = RE_QCALL_TX_TAIL.match(up)
                    if tm:
                        self.qcall_sends.append(
                            (self._qcall_await, tm["target"].upper()))
                    self._qcall_await = None
                pm = RE_PROBE.match(text)
                if not pm:
                    continue
                # Prefix present => must be us; absent => the app was
                # logging our own frame anyway (ALL.TXT "Transmitting"
                # lines are by definition ours).
                if pm["me"] and not self.is_me(pm["me"]):
                    continue
                to = pm["to"].upper()
                heads = [h.rstrip("> ").upper()
                         for h in pm["heads"].split(">") if h.strip()]
                # The station we expect an answer FROM is the first hop
                # for relayed probes, else the addressee.
                target = heads[0] if heads else to
                if target.startswith("@"):
                    continue  # broadcast: no single expected answerer
                sent.append((epoch(m["date"]), target, pm["cmd"]))

        # Credit an answer when the target transmitted to us inside the
        # window. Uses the sighting index built from DIRECTED.TXT.
        for ts, target, cmd in sent:
            stamps = self.rx_by_call.get(target, [])
            latency = None
            for t in stamps:
                if ts < t <= ts + PROBE_WINDOW_S:
                    latency = t - ts
                    break
            # Presence: did we decode the target within +/-10 min of
            # this probe? Without this flag the answer rate measures
            # absence, not willingness (see intel.py schema note).
            present = any(abs(t - ts) <= 600 for t in stamps)
            self.probes.append({"ts": ts, "target": target, "cmd": cmd,
                                "answered": 1 if latency else 0,
                                "latency_s": latency,
                                "present": 1 if present else 0})
        return len(sent)

    # ---- grid bank ----------------------------------------------

    def mine_grids(self, path: Path) -> int:
        if not path.exists():
            return 0
        src = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        n = 0
        for call, grid in src.execute("SELECT call, grid FROM grids"):
            call = call.upper()
            s = self._st(call)
            # On-air GRID text (mined above) outranks a bank entry only
            # when longer; otherwise the bank wins (it is the app's own
            # precision authority).
            if not s["grid"] or len(grid) >= len(s["grid"]):
                s["grid"] = grid.upper()
            n += 1
        src.close()
        return n

    # ---- write ---------------------------------------------------

    def flush(self) -> None:
        n = self.harvest_call_queries()
        if n:
            print(f"  recovered {n:,} QUERY CALL answers the app "
                  f"discarded (#178)")
        self.settle_relays()
        db = self.db
        db.execute("DELETE FROM stations")
        db.execute("DELETE FROM activity")
        db.execute("DELETE FROM edges")
        db.execute("DELETE FROM probes")
        db.execute("DELETE FROM sightings")
        db.execute("DELETE FROM edge_events")
        db.executemany(
            "INSERT INTO stations (call, first_heard, last_heard, "
            "heard_count, snr_n, snr_sum, snr_min, snr_max, rev_snr_n, "
            "rev_snr_last, rev_snr_best, rev_last, relay_seen, "
            "relay_asked, relay_done, to_us, "
            "grid, resp_count, spont_count)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [(c, s["first_heard"], s["last_heard"], s["heard_count"],
              s["snr_n"], s["snr_sum"], s["snr_min"], s["snr_max"],
              s["rev_snr_n"], s["rev_snr_last"], s["rev_snr_best"],
              s["rev_last"], s["relay_seen"],
              s.get("relay_asked", 0), s.get("relay_done", 0),
              s["to_us"], s["grid"],
              s["resp_count"], s["spont_count"])
             for c, s in self.stations.items()])
        db.executemany(
            "INSERT INTO activity (call, hour, n) VALUES (?,?,?)",
            [(c, h, n) for (c, h), n in self.activity.items()])
        db.executemany(
            "INSERT INTO edges (hearer, heard, last_when, n, snr, source)"
            " VALUES (?,?,?,?,?,?)",
            [(a, b, e["last_when"], e["n"], e["snr"], e["source"])
             for (a, b), e in self.edges.items()])
        db.executemany(
            "INSERT INTO probes (ts, target, cmd, answered, latency_s,"
            " present) VALUES (?,?,?,?,?,?)",
            [(p["ts"], p["target"], p["cmd"], p["answered"],
              p["latency_s"], p["present"]) for p in self.probes])
        db.executemany(
            "INSERT INTO sightings (ts, call, snr, dial, offset)"
            " VALUES (?,?,?,?,?)", self.sightings)
        db.executemany(
            "INSERT INTO edge_events (ts, hearer, heard, source)"
            " VALUES (?,?,?,?)", self.edge_events)
        db.commit()


def main() -> int:
    ap = argparse.ArgumentParser(description="Build the js8reach intel DB")
    ap.add_argument("--mycall", default=None)
    ap.add_argument("--db", default=str(intel.DEFAULT_DB))
    ap.add_argument("--directed", default=str(DIRECTED))
    ap.add_argument("--alltxt", default=str(ALLTXT))
    ap.add_argument("--grids", default=str(GRIDS_DB))
    args = ap.parse_args()

    mycall = args.mycall
    if not mycall:
        ini = Path.home() / ".config" / "JS8Call.ini"
        for line in ini.read_text(errors="replace").splitlines():
            if line.startswith("MyCall="):
                mycall = line.split("=", 1)[1].strip()
                break
    if not mycall:
        print("mine: --mycall required (not found in JS8Call.ini)",
              file=sys.stderr)
        return 1

    db = intel.connect(args.db)
    m = Miner(db, mycall)
    nd = m.mine_directed(Path(args.directed))
    np_ = m.mine_probes(Path(args.alltxt))
    ng = m.mine_grids(Path(args.grids))
    m.flush()
    intel.set_meta(db, "mycall", mycall.upper())
    mygrid = ""
    for line in (Path.home() / ".config" / "JS8Call.ini").read_text(
            errors="replace").splitlines():
        if line.startswith("MyGrid="):
            mygrid = line.split("=", 1)[1].strip().upper()
            break
    intel.set_meta(db, "mygrid", mygrid)
    intel.set_meta(db, "mined_at", str(int(datetime.now(timezone.utc)
                                           .timestamp())))
    db.commit()

    answered = sum(1 for p in m.probes if p["answered"])
    print(f"mined  {nd:,} directed frames, {np_:,} of our probes "
          f"({answered:,} answered), {ng:,} grid-bank rows")
    if m.skewed:
        print(f"  dropped {m.skewed} future-dated decode(s) (clock skew)")
    print(f"  stations {len(m.stations):,}   edges {len(m.edges):,}   "
          f"sightings {len(m.sightings):,}   "
          f"third-party mentions {len(m.edge_events):,}")
    print(f"  -> {args.db}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
