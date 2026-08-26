"""attempt.py — RUN one reaching attempt, unattended. The executor.

    python3 attempt.py KC1NNR --band 15m
    python3 attempt.py KC1NNR --band 15m --via K9IMM   (force move 1's route)

Everything learned this week, in one loop, so none of it needs a human
in real time (Andy, 2026-08-26: "self-sufficient code, not relying on
your real-time oversight at all"):

  * decisions come from decide.Decider -- the same factors dryrun shows;
  * ONE persistent socket: transmit AND events on it (two clients
    self-evict on the app's single slot -- measured the hard way);
  * TX-end anchored on the TX.COMPLETE event, which fires at SIGNAL
    end -- never on ALL.TXT, whose stamps are floored to the boundary
    and written pre-key (mainwindow.cpp:11228);
  * waits from decide.waits_for: per stage, period-parameterized,
    callsign-adjusted. A complete reply ends any wait instantly;
  * THE FORWARD GATE: a relay that has not been heard forwarding by
    escalate_s is dead either way -- it did not forward, or we cannot
    hear it, and then the return could never reach us through it
    either. Move on; passive listening still catches a direct answer
    (exactly how KQ4DNM was reached);
  * every transmission and event is logged with true offsets, so the
    round writes its own ledger.
"""
from __future__ import annotations

import argparse
import json
import re
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import decide                                   # noqa: E402
from callsign import base                       # noqa: E402
from live import LiveMap                        # noqa: E402
from livemodel import LiveBoard, LiveModel      # noqa: E402


def period_grace() -> float:
    return 15.0


def now_s() -> str:
    t = time.time()
    return time.strftime("%H:%M:%S", time.gmtime(t)) + f".{int(t % 1 * 1000):03d}"


class Radio:
    """The one socket: transmit and event stream together."""

    def __init__(self, host="127.0.0.1", port=2442):
        self.sock = socket.create_connection((host, port), 5)
        self.sock.settimeout(0.5)
        self.buf = b""

    def send(self, text: str) -> None:
        self.sock.sendall(json.dumps(
            {"type": "TX.SEND_MESSAGE", "value": text,
             "params": {"_ID": -1}}).encode() + b"\n")

    def events(self):
        """Yield (type, value) without blocking longer than 0.5 s."""
        try:
            chunk = self.sock.recv(65536)
            if chunk:
                self.buf += chunk
        except socket.timeout:
            pass
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            try:
                m = json.loads(line.decode())
            except ValueError:
                continue
            yield m.get("type", ""), str(m.get("value", ""))


def run(target: str, band: str, force_via: str, max_moves: int) -> int:
    lm = LiveMap.fetch(band=band)
    model = LiveModel(lm, band=band)
    # [#180] A GRID is a first-class target: resolve it to the best
    # reachable station in the square (screened -- a receive-only
    # monitor warns and sinks, never silently chosen), then run the
    # normal loop. A square with only monitors that report us is a
    # PARTIAL SUCCESS (delivery in proven), reported as such.
    import gridtarget
    if gridtarget.is_grid(target):
        chosen, monitors = gridtarget.resolve(model, target)
        if chosen is None:
            return 2
        target = chosen
    # [#173] The screen applies to NAMED targets too -- warn, never
    # refuse. KD2M was run as a named target and the ledger said
    # nothing about it being a receive-only monitor; the grid path
    # would have warned. A named monitor proceeds (the operator chose
    # it) but the ledger must say what it is.
    row = model.db.execute(
        "SELECT rx_only, radio_when, snr_to_me FROM stations WHERE "
        "band=? AND call=?", (model.band, base(target).upper())).fetchone()
    if row is not None:
        if row["rx_only"]:
            extra = (f" (it hears us at {row['snr_to_me']:+d} -- delivery "
                     f"provable)" if row["snr_to_me"] and
                     row["snr_to_me"] > -99 else "")
            print(f"{now_s()}  WARNING: {base(target).upper()} has never "
                  f"been heard transmitting -- receive-only monitor; "
                  f"expect no reply{extra}", flush=True)
        elif not row["radio_when"]:
            print(f"{now_s()}  WARNING: {base(target).upper()} never "
                  f"heard on radio this session -- internet-only "
                  f"evidence", flush=True)
    board = LiveBoard(model, target)
    d = decide.Decider(model, board, target, model.mycall)
    d._routes = decide.best_routes(d, board.pool)
    w = type("W", (), {})()          # the attempt's memory
    w.t0 = w.t = time.time()
    w.tried = {}; w.asked = set(); w.asked_grid = set()
    w.learned = {}; w.relocated = set(); w.gave_up = False

    radio = Radio()
    T = base(target).upper()
    print(f"{now_s()}  target {T} on {model.band}; "
          f"{len(board.pool)} candidates", flush=True)

    for move_no in range(1, max_moves + 1):
        if force_via and move_no == 1:
            chain = board.chain.get(force_via.upper()) or [force_via.upper()]
            mv = decide.Move("relay", decide.T_RELAY[len(chain)],
                             via=force_via.upper(), chain=chain,
                             why="route forced by operator",
                             reply_from=T)
            print(f"{now_s()}  [{move_no}] FORCED route via {force_via}",
                  flush=True)
        else:
            w.t = time.time()
            mv = d.choose(w)
        wire = mv.wire(model.mycall, T)
        check, escalate, abandon = mv.waits
        print(f"{now_s()}  [{move_no}] SEND {wire}", flush=True)
        # the WHY, factor by factor, before it airs -- so correctness
        # can be judged from the ledger alone
        print(decide.explain(mv, T, model.mycall), flush=True)
        radio.send(wire)
        w.tried[(mv.kind, mv.via or (T if mv.kind in ("snr", "grid")
                                     else ""))] = time.time()
        if mv.via:
            w.asked.add(mv.via)

        # ---- wait, event-driven, gated on ANSWER-STARTED ------------
        # RX.ACTIVITY hands us every frame at decode time, so the first
        # frame of any reply is due one period after TX-end. Silence
        # verdict is uniform (~21 s); the long window runs only once a
        # reply is provably inbound.
        # WHO must start answering inside the slot. For a directed
        # message: that station. For a GROUP query: anyone -- and if
        # nobody starts, the whole group is BUSY OR DISABLED and the
        # verdict covers them all at once (Andy, 2026-08-26). Either
        # way the remedy is identical: try again from the top later,
        # never wait longer now.
        group = mv.kind == "shout"
        responder = (mv.via or T).upper()
        tx_end = None
        started_at = None            # first frame from the responder
        fwd_done_at = None           # relay only: forward assembled
        rx_fwd = re.compile(rf"\*DE\*\s+{model.mycall}", re.I)
        rx_ans = re.compile(rf"^{re.escape(T)}\s*:", re.I)
        rx_yes = re.compile(rf":\s*{model.mycall}\s+YES\b", re.I)
        check, verdict, abandon = mv.waits
        comp = decide.completion_secs(mv.kind, T)
        hard_cap = time.time() + 90 + abandon
        # SLOT-ANCHORED VERDICT (Andy, 2026-08-26: "we should have had
        # a verdict in the range of 12.6+1 to 12.6+2 s... we could get
        # the next msg out without delaying another frame"). The reply
        # occupies exactly one slot: [B, B+15], B = the boundary after
        # our TX-end. Its decode posts at B+11..14 (early passes run
        # before the signal even ends). So the silence verdict belongs
        # INSIDE the slot -- at B + period - 0.7 -- leaving 0.7 s to
        # enqueue the next move, which then keys at B+15: no period is
        # ever lost to a dead call. The HB 25% late slot gets one more.
        slot_B = None                   # set when TX-END arrives
        while time.time() < hard_cap:
            for typ, val in radio.events():
                v = val.strip()
                if typ == "TX.COMPLETE" and tx_end is None:
                    tx_end = time.time()
                    import math
                    # true signal end is ~0.5 s before the event lands;
                    # the boundary is the next 15 s multiple after it
                    slot_B = (math.floor((tx_end - 1.0) / 15.0) + 1) * 15.0
                    print(f"{now_s()}      TX-END (slot B "
                          f"{time.strftime('%H:%M:%S', time.gmtime(slot_B))})",
                          flush=True)
                elif typ == "RX.ACTIVITY" and tx_end and started_at is None \
                        and (re.match(rf"[A-Z0-9/]+:\s*{model.mycall}\b",
                                      v.upper())
                             if group else
                             v.upper().startswith(responder + ":")):
                    started_at = time.time()
                    print(f"{now_s()}      answer STARTED "
                          f"+{started_at-tx_end:.0f}s: {v[:50]}", flush=True)
                elif typ == "RX.DIRECTED":
                    off = f"+{time.time()-tx_end:.0f}s" if tx_end else ""
                    if rx_ans.match(v):
                        print(f"{now_s()}  ANSWER {off}: {v[:70]}", flush=True)
                        print(f"{now_s()}  REACHED {T} on move "
                              f"{move_no}, {time.time()-w.t0:.0f}s "
                              f"total, {move_no} transmissions",
                              flush=True)
                        return 0
                    if mv.kind == "relay" and mv.via and \
                            v.upper().startswith(mv.via.upper() + ":") \
                            and rx_fwd.search(v):
                        fwd_done_at = time.time()
                        print(f"{now_s()}      forward complete {off}: "
                              f"{v[:60]}", flush=True)
                        # phase 2: the TARGET's answer must now start;
                        # rearm the started-gate for it.
                        responder = T
                        started_at = None
                        tx_end = fwd_done_at        # new anchor
                    if rx_yes.search(v):
                        who = base(v.split(":")[0]).upper()
                        w.learned[who] = 0.9
                        print(f"{now_s()}      learned {off}: {v[:60]}",
                              flush=True)
            if tx_end is None:
                continue
            dt = time.time() - tx_end
            slots = 2 if mv.kind == "hb" else 1     # documented late slot
            verdict_wall = slot_B + 15.0 * slots - 0.7
            if started_at is None and time.time() > verdict_wall:
                who = ("the whole group" if group else responder)
                print(f"{now_s()}      VERDICT +{dt:.1f}s: {who} busy or "
                      f"disabled -- retry from the top later; next move "
                      f"can key the coming boundary", flush=True)
                break
            if started_at is not None and \
                    time.time() - started_at > comp + period_grace():
                print(f"{now_s()}      answer started but never "
                      f"assembled (+{time.time()-started_at:.0f}s) -- "
                      f"frames lost; moving on", flush=True)
                break
    print(f"{now_s()}  NOT REACHED after {move_no} moves "
          f"({time.time()-w.t0:.0f}s total) -- verdict: busy or "
          f"disabled; retry from the top later (rerun this command)",
          flush=True)
    return 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("target")
    ap.add_argument("--band", default="")
    ap.add_argument("--via", default="",
                    help="force move 1 to relay via this station")
    ap.add_argument("--max-moves", type=int, default=6,
                    help="courtesy cap on transmissions per attempt")
    a = ap.parse_args()
    return run(a.target, a.band, a.via, a.max_moves)


if __name__ == "__main__":
    raise SystemExit(main())
