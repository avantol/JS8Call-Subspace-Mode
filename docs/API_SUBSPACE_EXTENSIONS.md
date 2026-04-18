# JS8Call Subspace API Extensions

Additions and clarifications to the JS8Call TCP/UDP JSON API, introduced
in Subspace Edition. All existing commands continue to work unchanged.

## Connection Model

The API server listens on TCP port **2442** by default. Multiple clients
may connect simultaneously; every outbound event (`RX.DIRECTED`,
`RX.ACTIVITY`, `RX.SPOT`, `TX.FRAME`, `TX.COMPLETE`, `STATION.*`, etc.)
is broadcast to every connected client.

**Two usage patterns are supported:**

1. **Poll-based** — connect, send a command, read the response, disconnect.
   Simple and stateless; miss push events between polls. This is what
   short-lived helpers like `nc` scripts use.

2. **Persistent (recommended for automation)** — connect once, read
   forever. You receive every push event as it happens. Send commands on
   the same socket when you need to act. Much gentler on the server's
   TCP stack, especially under activity bursts.

### Message Format

```json
{"type":"CMD","value":"optional-string","params":{"KEY":"VALUE"}}
```

Responses and pushed events share the same shape. The `_ID` field (when
present) correlates a response to its request. Push events that were not
solicited use `_ID: -1`.

---

## New Commands

### `EVENTS.KEEPALIVE`

Lightweight round-trip ping. Use on a persistent connection to confirm
the socket and event stream are alive without touching any state.

**Request:** `{"type":"EVENTS.KEEPALIVE"}`

**Response:** `{"type":"EVENTS.PONG","params":{"_ID":<id>,"UTC":<ms>}}`

### `MODE.GET_SUBMODE_NAME`

Returns the current submode with both its numeric speed and human-readable
name, so clients need not maintain the mapping locally.

**Request:** `{"type":"MODE.GET_SUBMODE_NAME"}`

**Response:**
```json
{"type":"MODE.SUBMODE_NAME",
 "params":{"_ID":<id>,"SPEED":<int>,"NAME":"Normal|Fast|Turbo|Slow|Ultra|Subspace"}}
```

### `RX.GET_BAND_ACTIVITY` (extended)

In addition to the existing per-offset fields (`FREQ`, `DIAL`, `OFFSET`,
`TEXT`, `SNR`, `UTC` — unchanged, describing the *latest* decode at each
offset), each offset now also carries a `HISTORY` array containing up
to ten recent decodes at that offset. Fixes the divergence where the
UI's band panel shows multiple distinct stations that landed on similar
offsets while the API only exposed the most recent one.

```json
"1700": {
  "OFFSET": 1700, "TEXT": "WD4KAV: @HB HEARTBEAT CN87", "SNR": -6,
  "UTC": 1776486000000, "DIAL": 7078000, "FREQ": 7079700,
  "HISTORY": [
    {"TEXT":"N6GRG: @HB HEARTBEAT DM04","SNR":-9,"UTC":1776485994000},
    {"TEXT":"WD4KAV: @HB HEARTBEAT CN87","SNR":-6,"UTC":1776486000000}
  ]
}
```

Existing clients continue to work unchanged; they'll see exactly the
same latest-only fields as before. Clients that want the full picture
iterate `HISTORY`.

### `TX.SEND_DIRECTED`

Server-side composer for properly-formatted directed messages. Avoids
the FROM/TO-reversal mistake that's easy to make with raw `TX.SEND_MESSAGE`.

**Request:**
```json
{"type":"TX.SEND_DIRECTED",
 "params":{"TO":"KD8SKZ","CMD":"SNR","EXTRA":"-13","PRIORITY":"HIGH"}}
```

Server composes `<MYCALL>: KD8SKZ SNR -13` and enqueues at the given
priority. `TO` may be a callsign or an `@GROUP` (e.g. `@ALLCALL`).
Validates `TO` against a callsign regex.

**Response:**
```json
{"type":"TX.SEND_DIRECTED",
 "value":"<composed message>",
 "params":{"_ID":<id>,"OK":true,"ERROR":"","COMPOSED":"WM8Q: KD8SKZ SNR -13"}}
```

On failure, `OK:false` and `ERROR` names the reason; nothing is queued.

### `QSO.GET_CONTEXT`

One-shot bundled state snapshot. Replaces 4-5 separate polls with a
single response — cheap "what's going on right now?" for automation.

**Request:** `{"type":"QSO.GET_CONTEXT"}`

**Response:**
```json
{"type":"QSO.CONTEXT",
 "params":{
   "_ID":<id>,
   "CALLSIGN":"WM8Q",
   "GRID":"DN61",
   "SUBMODE":0,
   "SUBMODE_NAME":"NORMAL",
   "DIAL":7078000,
   "OFFSET":1700,
   "PTT":false,
   "TX_QUEUE_DEPTH":0,
   "CALL_SELECTED":"",
   "ACTIVE_OFFSETS_2MIN":5,
   "UTC":<ms>}}
```

### `RX.CLEAR_OFFSET`

Remove a specific offset's band-activity history. Useful to drop stale
or unwanted rows without clearing the whole band pane.

**Request:**
```json
{"type":"RX.CLEAR_OFFSET","params":{"OFFSET":1500}}
```

**Response:**
```json
{"type":"RX.CLEAR_OFFSET",
 "params":{"_ID":<id>,"OFFSET":1500,"CLEARED":true}}
```

`CLEARED` is `false` if no activity existed at that offset.

---

## Extended Commands

### `MODE.SET_SPEED`

Now accepts the Subspace (FT2) submode in addition to Normal / Fast /
Turbo / Slow / Ultra. `SPEED` value for Subspace is `Varicode::JS8CallFT2`
(integer constant exported via `MODE.GET_SPEED` / `MODE.GET_SUBMODE_NAME`).

### `TX.SEND_MESSAGE`

Accepts an optional `PRIORITY` param:

- `"HIGH"` — default (matches prior behavior)
- `"NORMAL"` — fits between user-typed text and background traffic
- `"LOW"` — lowest priority; queue is drained in order

```json
{"type":"TX.SEND_MESSAGE","value":"WM8Q: CQ CQ CQ DN61","params":{"PRIORITY":"NORMAL"}}
```

Higher-priority items that arrive later will transmit before queued
lower-priority items.

---

## New Push Events

### `TX.COMPLETE`

Pushed when a queued transmission block finishes transmitting (all frames
sent, PTT released). Lets automation replace `TX.GET_QUEUE_DEPTH` polling
with an event-driven flow.

```json
{"type":"TX.COMPLETE",
 "value":"<sent message text>",
 "params":{"_ID":-1,"SUBMODE":<int>,"UTC":<ms>}}
```

Note: fires once per transmission block, not once per frame. `TX.FRAME`
events continue to fire per audio frame during TX.

---

## Example: Persistent Python Client

```python
import socket, json

s = socket.create_connection(("127.0.0.1", 2442))
buf = b""

def send(cmd, value="", **params):
    msg = json.dumps({"type": cmd, "value": value, "params": params}) + "\n"
    s.sendall(msg.encode())

send("STATION.GET_CALLSIGN")

while True:
    chunk = s.recv(65536)
    if not chunk: break
    buf += chunk
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        if not line.strip(): continue
        msg = json.loads(line)
        t = msg.get("type", "")
        if t == "RX.DIRECTED":
            params = msg["params"]
            if params.get("TO") == "WM8Q":
                send("TX.SEND_MESSAGE",
                     f"WM8Q: {params['FROM']} SNR {params['SNR']:+03d}",
                     PRIORITY="HIGH")
        elif t == "TX.COMPLETE":
            print("tx done:", msg["value"])
```
