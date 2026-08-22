#!/usr/bin/env python3
"""js8client — shared event-driven TCP-API client for Subspace Edition.

[TODO #155] The one client library every script consumes (prospector,
msg rotator, CQ loop, spotter-style tools). Replaces per-script socket
code, fixed sleeps, and polling with ONE persistent socket that both
sends and listens (the EOT lesson), where every wait is a wait on a
matching pushed event with a timeout — zero dead time, zero busy-work.

Wire protocol: newline-delimited JSON objects on TCP (default
127.0.0.1:2442), each {"type": ..., "value": ..., "params": {...}}.

Events the app PUSHES (inventory 2026-08-20, Build 371 source):
  RX.DIRECTED / RX.TEXT / RX.SPOT / RX.CALL_SELECTED
  TX.FRAME / TX.COMPLETE / TX.QUEUE_DEPTH / TX.TEXT
  TX.CHUNKED_PROGRESS / TX.CHUNKED_COMPLETE / TX.CHUNKED_FAILED
  STATION.STATUS / STATION.BUSY / STATION.CLOSING / STATION.SPOT
  MODE.SPEED / MODE.ARQ / RIG.FREQ / RIG.PTT
Request/response pairs (client sends left, app answers right):
  STATION.GET_CALLSIGN -> STATION.CALLSIGN
  STATION.GET_GRID     -> STATION.GRID
  STATION.GET_INFO     -> STATION.INFO
  STATION.GET_OS       -> STATION.VERSION-ish (see app)
  RIG.GET_FREQ         -> RIG.FREQ
  MODE.GET_SPEED       -> MODE.SPEED
  TX.GET_TEXT          -> TX.TEXT
  INBOX.GET_MESSAGES   -> INBOX.MESSAGES
Fire-and-forget commands:
  TX.SEND_MESSAGE / TX.SEND_DIRECTED / TX.SEND_CHUNKED
  MODE.SET_SPEED / RIG.SET_FREQ / RX.GET_CALL_ACTIVITY ...

Typical usage (no sleeps anywhere):

    from js8client import Js8Client

    with Js8Client() as js8:
        mycall = js8.request("STATION.GET_CALLSIGN")["value"]
        js8.send_message(f"{peer} SNR?")
        done = js8.wait_for("TX.COMPLETE", timeout=300)
        reply = js8.wait_for(
            "RX.DIRECTED",
            predicate=lambda m: m["params"].get("FROM") == peer,
            timeout=180)

Latency accounting: every wait_for() records how long it actually
blocked; Js8Client.stats() reports count/total/max per event type —
the before/after measurement #155(d) asks for.
"""

from __future__ import annotations

import json
import logging
import socket
import threading
import time
from collections import defaultdict
from typing import Any, Callable, Dict, List, Optional

log = logging.getLogger("js8client")

Message = Dict[str, Any]


class Js8Error(Exception):
    pass


class Js8Timeout(Js8Error):
    """A wait elapsed without its event. Carries what we waited for."""

    def __init__(self, what: str, timeout: float):
        super().__init__(f"timeout ({timeout:.0f}s) waiting for {what}")
        self.what = what
        self.timeout = timeout


class _Waiter:
    __slots__ = ("predicate", "event", "match")

    def __init__(self, predicate: Callable[[Message], bool]):
        self.predicate = predicate
        self.event = threading.Event()
        self.match: Optional[Message] = None


class Js8Client:
    """One persistent socket; reader thread dispatches pushed events to
    registered waiters and callbacks. All public methods thread-safe.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 2442,
                 connect_timeout: float = 10.0):
        self._host = host
        self._port = port
        self._sock = socket.create_connection((host, port),
                                              timeout=connect_timeout)
        # Blocking reads with no timeout on the reader thread — the
        # thread parks in recv, costing nothing until data arrives.
        self._sock.settimeout(None)
        self._wlock = threading.Lock()
        self._waiters_lock = threading.Lock()
        self._waiters: List[_Waiter] = []
        self._listeners: Dict[str,
                              List[Callable[[Message], None]]] = \
            defaultdict(list)
        self._stats_lock = threading.Lock()
        self._stats: Dict[str, List[float]] = defaultdict(list)
        self._closed = threading.Event()
        self._reader = threading.Thread(target=self._read_loop,
                                        name="js8client-reader",
                                        daemon=True)
        self._reader.start()

    # ---- lifecycle ---------------------------------------------------

    def close(self) -> None:
        self._closed.set()
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()

    def __enter__(self) -> "Js8Client":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # ---- send side ---------------------------------------------------

    def send(self, type_: str, value: str = "",
             params: Optional[Dict[str, Any]] = None) -> None:
        """Fire one API message. Never blocks on a reply."""
        msg = {"type": type_, "value": value,
               "params": params or {"_ID": int(time.time() * 1000)}}
        data = (json.dumps(msg) + "\n").encode()
        with self._wlock:
            self._sock.sendall(data)
        log.debug("sent %s", msg)

    def send_message(self, text: str) -> None:
        """Queue outgoing text exactly as if typed (TX.SEND_MESSAGE)."""
        self.send("TX.SEND_MESSAGE", text)

    # ---- event-driven waits (the point of #155) ----------------------

    def wait_for(self, type_: str,
                 predicate: Optional[Callable[[Message], bool]] = None,
                 timeout: float = 60.0) -> Message:
        """Block until a pushed message of `type_` (and matching
        `predicate`, if given) arrives. No polling: the reader thread
        wakes us the moment the event lands. Raises Js8Timeout.
        """
        def match(m: Message) -> bool:
            return m.get("type") == type_ and \
                (predicate is None or predicate(m))
        return self._wait(match, what=type_, timeout=timeout)

    def wait_any(self, types: List[str],
                 timeout: float = 60.0) -> Message:
        """First message whose type is in `types` (e.g. the chunked
        transfer terminals COMPLETE/FAILED)."""
        tset = set(types)
        return self._wait(lambda m: m.get("type") in tset,
                          what="|".join(types), timeout=timeout)

    def request(self, type_: str, value: str = "",
                reply_type: Optional[str] = None,
                timeout: float = 15.0) -> Message:
        """Send `type_` and await its reply. Reply type defaults to
        the GET_ -> noun convention (STATION.GET_GRID -> STATION.GRID).
        """
        if reply_type is None:
            reply_type = type_.replace(".GET_", ".")
        # Register the waiter BEFORE sending — a fast reply must not
        # race past us (this gap was a real bug class in the ad-hoc
        # clients).
        waiter = self._add_waiter(
            lambda m: m.get("type") == reply_type)
        try:
            self.send(type_, value)
            return self._await(waiter, what=reply_type,
                               timeout=timeout)
        finally:
            self._drop_waiter(waiter)

    # ---- high-level idioms -------------------------------------------

    def send_and_wait_complete(self, text: str,
                               timeout: float = 600.0) -> Message:
        """Queue text and block until the app reports the transmission
        block finished (TX.COMPLETE fires at the queue-drained stopTx,
        i.e. last frame + drained — not the per-frame stopTx)."""
        waiter = self._add_waiter(
            lambda m: m.get("type") == "TX.COMPLETE")
        try:
            self.send_message(text)
            return self._await(waiter, what="TX.COMPLETE",
                               timeout=timeout)
        finally:
            self._drop_waiter(waiter)

    def wait_directed(self, from_call: Optional[str] = None,
                      contains: Optional[str] = None,
                      timeout: float = 300.0) -> Message:
        """Next RX.DIRECTED, optionally filtered by sender and/or a
        substring of the text (e.g. the EOT diamond or 'SNR')."""
        def pred(m: Message) -> bool:
            p = m.get("params", {})
            if from_call and \
                    p.get("FROM", "").upper() != from_call.upper():
                return False
            if contains and contains not in m.get("value", ""):
                return False
            return True
        return self.wait_for("RX.DIRECTED", predicate=pred,
                             timeout=timeout)

    def on(self, type_: str,
           callback: Callable[[Message], None]) -> None:
        """Register a persistent listener (runs on the reader thread —
        keep it quick, hand off real work to your own queue)."""
        with self._waiters_lock:
            self._listeners[type_].append(callback)

    # ---- measurement (#155 d) ----------------------------------------

    def stats(self) -> Dict[str, Dict[str, float]]:
        """Per-event-type blocking totals from every wait made through
        this client: {type: {count, total_s, max_s}}."""
        with self._stats_lock:
            return {
                k: {"count": len(v), "total_s": sum(v),
                    "max_s": max(v)}
                for k, v in self._stats.items() if v
            }

    # ---- internals ---------------------------------------------------

    def _add_waiter(self, predicate) -> _Waiter:
        w = _Waiter(predicate)
        with self._waiters_lock:
            self._waiters.append(w)
        return w

    def _drop_waiter(self, w: _Waiter) -> None:
        with self._waiters_lock:
            if w in self._waiters:
                self._waiters.remove(w)

    def _await(self, w: _Waiter, what: str,
               timeout: float) -> Message:
        t0 = time.monotonic()
        ok = w.event.wait(timeout)
        waited = time.monotonic() - t0
        with self._stats_lock:
            self._stats[what].append(waited)
        if not ok:
            raise Js8Timeout(what, timeout)
        if w.match is None:
            # Woken by disconnect, not by a match (the read loop sets
            # every waiter's event on socket close).
            raise Js8Error(f"disconnected while waiting for {what}")
        return w.match

    def _wait(self, predicate, what: str, timeout: float) -> Message:
        w = self._add_waiter(predicate)
        try:
            return self._await(w, what, timeout)
        finally:
            self._drop_waiter(w)

    def _read_loop(self) -> None:
        buf = b""
        while not self._closed.is_set():
            try:
                chunk = self._sock.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    log.warning("unparseable line: %r", line[:120])
                    continue
                self._dispatch(msg)
        self._closed.set()
        # Wake every waiter so blocked callers see the disconnect
        # instead of hanging out their full timeout.
        with self._waiters_lock:
            for w in self._waiters:
                w.event.set()

    def _dispatch(self, msg: Message) -> None:
        log.debug("recv %s", msg.get("type"))
        with self._waiters_lock:
            waiters = list(self._waiters)
            listeners = list(self._listeners.get(msg.get("type"), ()))
        for w in waiters:
            if w.match is None:
                try:
                    if w.predicate(msg):
                        w.match = msg
                        w.event.set()
                except Exception:
                    log.exception("waiter predicate failed")
        for cb in listeners:
            try:
                cb(msg)
            except Exception:
                log.exception("listener failed")


# ---- demo / measurement -----------------------------------------------

def _main() -> int:
    import argparse
    ap = argparse.ArgumentParser(
        description="js8client demo: --rtt measures request round "
                    "trips; --monitor streams pushed events")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2442)
    ap.add_argument("--rtt", action="store_true",
                    help="measure request->reply latency x10")
    ap.add_argument("--monitor", action="store_true",
                    help="print every pushed event until Ctrl-C")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(message)s")
    with Js8Client(args.host, args.port) as js8:
        if args.rtt:
            for _ in range(10):
                t0 = time.monotonic()
                r = js8.request("STATION.GET_CALLSIGN")
                print(f"STATION.GET_CALLSIGN -> {r.get('value')!r} "
                      f"in {(time.monotonic() - t0) * 1000:.1f} ms")
            print("stats:", json.dumps(js8.stats(), indent=2))
            return 0
        if args.monitor:
            js8.on_any = None  # readability; monitor uses wait loop
            print("monitoring — Ctrl-C to stop")
            try:
                while True:
                    m = js8._wait(lambda _m: True, what="*",
                                  timeout=3600)
                    print(f"{m.get('type'):24s} "
                          f"{str(m.get('value'))[:70]}")
            except KeyboardInterrupt:
                return 0
        ap.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
