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
CALLSIGN = r"[A-Z0-9]{2,}[0-9][A-Z0-9]*(?:/[A-Z0-9]+)?"
RE_DIRECTED_LINE = re.compile(
    r"^(?P<date>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\t"
    r"(?P<dial>[\d.]+)\t(?P<offset>-?\d+)\t(?P<snr>[+-]?\d+)\t"
    r"(?P<text>.*)$")
RE_FROM = re.compile(rf"^(?P<from>{CALLSIGN}):\s+(?P<rest>.*)$")
RE_CALL = re.compile(CALLSIGN)
RE_DE = re.compile(rf"\*DE\*\s+(?P<de>{CALLSIGN})")
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
# Blind calls: asking someone a question proves nothing about whether
# you can hear them. Everything else addressed to a specific station
# is either a reply or conversation, and both imply reception.
RE_QUERY_CMD = re.compile(
    r"^(?:SNR\?|\?|HEARING\?|GRID\?|STATUS\?|INFO\?|AGN\?|QUERY|"
    r"HEARTBEAT|HB|CQ|MSG\b|DIT)(?:\s|$)")


def epoch(datestr: str) -> int:
    return int(datetime.strptime(datestr, "%Y-%m-%d %H:%M:%S")
               .replace(tzinfo=timezone.utc).timestamp())


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

    def edge(self, hearer: str, heard: str, ts: int, snr: int | None,
             source: str) -> None:
        if hearer == heard or self.is_me(heard):
            return
        e = self.edges.setdefault((hearer, heard), {
            "last_when": 0, "n": 0, "snr": None, "source": source})
        e["n"] += 1
        if source == "hearing":
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
                if self.is_me(sender):
                    continue  # our own frames come from ALL.TXT
                self.sighting(sender, ts, int(m["snr"]),
                              float(m["dial"]), int(m["offset"]))

                # Relay proof: this station forwarded someone's traffic.
                de = RE_DE.search(rest)
                head = re.match(rf"^(?:{CALLSIGN}|@[A-Z0-9]+)>", rest)
                if de or head:
                    self._st(sender)["relay_seen"] += 1
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
                        self.edge(sender, to, ts, None, "replied")
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
            "rev_snr_last, rev_snr_best, rev_last, relay_seen, to_us, "
            "grid, resp_count, spont_count)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            [(c, s["first_heard"], s["last_heard"], s["heard_count"],
              s["snr_n"], s["snr_sum"], s["snr_min"], s["snr_max"],
              s["rev_snr_n"], s["rev_snr_last"], s["rev_snr_best"],
              s["rev_last"], s["relay_seen"], s["to_us"], s["grid"],
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
