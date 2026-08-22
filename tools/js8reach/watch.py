#!/usr/bin/env python3
"""watch.py — wait for a station to appear. Costs no airtime.

When the sweep says nobody has heard the target for hours, the target
is not on the air and there is nothing useful to transmit at. Calling
into that costs ~70 s a try and teaches nothing; store-and-forward is
not delivery either (the target only learns a message is waiting if it
beacons AND the storing station has heartbeat-ACK enabled -- which is
off by default -- or if it spontaneously sends QUERY MSGS).

So the productive move is to listen. This tails DIRECTED.TXT and says
the moment the target shows any sign of life:

    DIRECT      we decoded the target ourselves -> call it NOW, the
                path is open this minute
    THIRD PARTY someone named it in a HEARING list -> it is on the air
                but we cannot copy it: relay through that station
    MENTION     its call appeared in someone's traffic -> weaker, but
                worth a look

    (QUERY CALL replies cannot be used here: "WM8Q YES -18 (6H)" never
    names the station it is about.)

Reads the log file, not the API, so it never touches the single TCP
connection JS8Call allows.

    python3 watch.py VA3NB
    python3 watch.py VA3NB --once      # exit on first sighting
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

DIRECTED = Path.home() / ".local" / "share" / "JS8Call" / "DIRECTED.TXT"
LINE = re.compile(
    r"^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\t(?P<dial>[\d.]+)\t"
    r"(?P<off>-?\d+)\t(?P<snr>[+-]?\d+)\t(?P<text>.*)$")
FROM = re.compile(r"^(?P<from>[A-Z0-9/]+):\s+(?P<rest>.*)$")


def classify(target: str, sender: str, rest: str
             ) -> tuple[str, str] | None:
    """(kind, detail) if this frame is evidence the target is up."""
    t = target.upper()
    up = rest.upper()
    if sender.upper().split("/")[0] == t.split("/")[0]:
        return "DIRECT", "we decoded it ourselves"
    if t not in up:
        return None
    # Someone reporting that THEY hear the target.
    if re.search(rf"\bHEARING\b[^\n]*\b{re.escape(t)}\b", up):
        return "THIRD PARTY", f"{sender} lists it in HEARING"
    # NOTE: a QUERY CALL reply ("WM8Q YES -18 (6H)") never names the
    # station it is about -- the binding lives only in the asker's
    # pending state, which is why the app needs #161 to interpret its
    # own sweeps. A passive watcher therefore cannot attribute those
    # replies and does not try.
    return "MENTION", f"{sender}: {rest[:48]}"


def main() -> int:
    ap = argparse.ArgumentParser(description="Wait for a station to appear")
    ap.add_argument("target")
    ap.add_argument("--once", action="store_true",
                    help="exit after the first sighting")
    ap.add_argument("--log", default=str(DIRECTED))
    args = ap.parse_args()
    target = args.target.upper()
    path = Path(args.log)
    if not path.exists():
        print(f"watch: {path} not found", file=sys.stderr)
        return 1

    print(f"watching for {target} (no transmission; reading {path.name})",
          flush=True)
    with path.open(errors="replace") as fh:
        fh.seek(0, 2)                       # start at the end: live only
        while True:
            line = fh.readline()
            if not line:
                time.sleep(2.0)
                continue
            m = LINE.match(line.rstrip("\n"))
            if not m:
                continue
            fm = FROM.match(m["text"].strip())
            if not fm:
                continue
            hit = classify(target, fm["from"], fm["rest"])
            if not hit:
                continue
            kind, detail = hit
            print(f"[{m['ts']}Z] {kind}: {detail}"
                  + (f"  (snr {m['snr']}, {m['off']} Hz)"
                     if kind == "DIRECT" else ""), flush=True)
            if kind == "DIRECT":
                print(f"  -> {target} is audible HERE right now. "
                      f"Call it: \"{target} SNR?\"", flush=True)
            elif kind == "THIRD PARTY":
                print(f"  -> {target} is on the air but we cannot copy "
                      f"it. Relay: \"{fm['from']}>{target} SNR?\"",
                      flush=True)
            if args.once and kind in ("DIRECT", "THIRD PARTY"):
                return 0


if __name__ == "__main__":
    raise SystemExit(main())
