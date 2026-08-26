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

    def attempt_done(self) -> None:
        """Clear the map's red attempt line NOW -- the verdict is in."""
        self.sock.sendall(json.dumps(
            {"type": "TX.ATTEMPT_DONE", "value": "",
             "params": {"_ID": -1}}).encode() + b"\n")

    def events(self, wake_in: float = 0.5):
        """Yield (type, value); block until an event arrives OR
        `wake_in` seconds pass -- the caller sets wake_in to the time
        remaining until its deadline, so the decision fires ON the
        deadline instead of up to half a second late (operator, 2026-
        08-26: "set a timer for period boundary minus frame setup
        time; when that timer fires and we have no from-frame, we do
        something else"). Evidence is push; only absence is timed."""
        self.sock.settimeout(max(0.005, min(0.5, wake_in)))
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
    # LITERAL CALL ONLY -- no base() fallback. The fallback matched
    # OUR OWN row for WM8Q/P and printed its warning as the target's
    # (operator: "there's that rule again. a base call is nearly never
    # correct"). WM8Q/P's record is WM8Q/P's; absence of a row is the
    # true answer, not a licence to borrow the base call's.
    row = model.db.execute(
        "SELECT rx_only, radio_when, snr_to_me FROM stations WHERE "
        "band=? AND call=?",
        (model.band, target.upper().strip())).fetchone()
    if row is not None:
        if row["rx_only"]:
            extra = (f" (it hears us at {row['snr_to_me']:+d} -- delivery "
                     f"provable)" if row["snr_to_me"] and
                     row["snr_to_me"] > -99 else "")
            print(f"{now_s()}  WARNING: {target.upper().strip()} has never "
                  f"been heard transmitting -- receive-only monitor; "
                  f"expect no reply{extra}", flush=True)
        elif not row["radio_when"]:
            print(f"{now_s()}  WARNING: {target.upper().strip()} never "
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
    # THE LITERAL CALLSIGN, affixes preserved. base() here would have
    # turned WM8Q/P into WM8Q -- our own call -- transmitting
    # "WM8Q: WM8Q SNR?" and matching our own autoreplies as the
    # answer. Affixes are part of the on-air identity (standing rule);
    # base() is for record lookups only.
    T = target.upper().strip()
    past_vias: list = []          # relays already asked this attempt
    hold_until = 0.0              # leave air clear for a late reply
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
            if mv is None:
                print(f"{now_s()}  nothing left to try -- every option "
                      f"is spent; verdict: busy or disabled, retry "
                      f"from the top later", flush=True)
                break
        while time.time() < hold_until:
            time.sleep(0.5)
            for typ, val in radio.events():
                if typ == "RX.DIRECTED" and \
                        re.match(rf"{re.escape(T)}\s*:", val.strip(),
                                 re.I):
                    print(f"{now_s()}  ANSWER (late route): "
                          f"{val[:70]}", flush=True)
                    print(f"{now_s()}  REACHED {T} via a late forward",
                          flush=True)
                    return 0
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

        # ---- ONE DEADLINE, extended only by observed progress -------
        # The operator's construction (2026-08-26), replacing my
        # per-stage clocks after both of their hand-off bugs: a single
        # deadline computed from the WHOLE caused chain at TX-end;
        # tripwires only ever SHORTEN it (a relay that never keys ends
        # it in one slot); each caused message actually OBSERVED to
        # start EXTENDS it to that event plus the remainder of the
        # chain. One number to maintain -- pieces of the chain cannot
        # be lost between clocks, and a late forward (K4GMX +104 s)
        # extends naturally instead of eating the budget.
        import math
        def slot_end(t, n):
            """Wall time of the n-th slot boundary after t, minus the
            0.7 s enqueue margin."""
            return ((math.floor((t - 1.0) / 15.0) + 1) * 15.0
                    + 15.0 * n - 0.7)
        group = mv.kind == "shout"
        responder = (mv.via or T).upper()
        tx_end = None
        deadline = None
        fwd_started = fwd_done = ans_started = None
        rx_fwd = re.compile(rf"\*DE\*\s+{model.mycall}", re.I)
        rx_ans = re.compile(rf"^{re.escape(T)}\s*:", re.I)
        rx_yes = re.compile(rf":\s*{model.mycall}\s+YES\b", re.I)
        comp = decide.completion_secs(mv.kind, T)
        hard_cap = time.time() + 90 + 16 * 15
        while time.time() < hard_cap:
            wake = (deadline - time.time()) if deadline else 0.5
            for typ, val in radio.events(wake):
                v = val.strip()
                nowt = time.time()
                if typ == "TX.COMPLETE" and tx_end is None:
                    tx_end = nowt
                    # initial deadline = the FIRST caused message's
                    # slot (the tripwire); hb gets its documented two
                    deadline = slot_end(tx_end, 2 if mv.kind == "hb"
                                        else 1)
                    print(f"{now_s()}      TX-END; deadline "
                          f"{time.strftime('%H:%M:%S', time.gmtime(deadline))}",
                          flush=True)
                elif typ == "RX.ACTIVITY" and tx_end:
                    up = v.upper()
                    if mv.kind == "relay" and mv.via \
                            and fwd_started is None \
                            and up.startswith(mv.via.upper() + ":"):
                        fwd_started = nowt
                        # forward observed: extend for its remaining
                        # ~2 frames plus the target's 3 answer slots
                        deadline = max(deadline, nowt + 30.0 + 45.0)
                        print(f"{now_s()}      forward STARTED "
                              f"+{nowt-tx_end:.0f}s -- deadline extended",
                              flush=True)
                    elif ans_started is None and \
                            (re.match(rf"[A-Z0-9/]+:\s*{model.mycall}\b",
                                      up) if group else
                             up.startswith(responder + ":")
                             if mv.kind != "relay" else
                             up.startswith(T + ":")):
                        ans_started = nowt
                        deadline = max(deadline, nowt + comp + 15.0)
                        print(f"{now_s()}      answer STARTED "
                              f"+{nowt-tx_end:.0f}s -- deadline extended",
                              flush=True)
                elif typ == "RX.DIRECTED":
                    off = f"+{nowt-tx_end:.0f}s" if tx_end else ""
                    for pv in past_vias:
                        if v.upper().startswith(pv + ":") \
                                and rx_fwd.search(v):
                            hold_until = nowt + 30.0
                            print(f"{now_s()}      LATE FORWARD from "
                                  f"{pv}: holding TX two slots for its "
                                  f"reply", flush=True)
                    if rx_ans.match(v):
                        print(f"{now_s()}  ANSWER {off}: {v[:70]}",
                              flush=True)
                        print(f"{now_s()}  REACHED {T} on move "
                              f"{move_no}, {nowt-w.t0:.0f}s total, "
                              f"{move_no} transmissions", flush=True)
                        return 0
                    if mv.kind == "relay" and mv.via and \
                            v.upper().startswith(mv.via.upper() + ":") \
                            and rx_fwd.search(v):
                        fwd_done = nowt
                        deadline = max(deadline, slot_end(nowt, 3))
                        print(f"{now_s()}      forward complete {off}; "
                              f"target has three slots", flush=True)
                    if rx_yes.search(v):
                        who = base(v.split(":")[0]).upper()
                        w.learned[who] = 0.9
                        print(f"{now_s()}      learned {off}: {v[:60]}",
                              flush=True)
            if tx_end is None or time.time() < deadline:
                continue
            state = ("relay silent -- never keyed"
                     if mv.kind == "relay" and fwd_started is None else
                     "forwarded, but the target never answered"
                     if fwd_done is not None else
                     "answer started but never assembled -- frames lost"
                     if ans_started is not None else
                     ("the whole group is" if group else responder + " is")
                     + " busy or disabled")
            print(f"{now_s()}      VERDICT "
                  f"+{time.time()-tx_end:.0f}s: {state} -- retry from "
                  f"the top later", flush=True)
            radio.attempt_done()
            if mv.kind == "relay" and mv.via:
                past_vias.append(mv.via.upper())
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
