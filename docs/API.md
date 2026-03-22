# JS8Call-Improved API Documentation v2.6.0

This document provides complete documentation of JS8Call-Improved's external control API,
derived from reading the source code.

---

## Table of Contents

1. [Transport & Configuration](#transport--configuration)
2. [Message Format](#message-format)
3. [Routine Messages (App to Client)](#routine-messages-app-to-client)
4. [Command Messages (Client to App)](#command-messages-client-to-app)
5. [WSJT-X UDP Binary Protocol](#wsjt-x-udp-binary-protocol)
6. [Experimental / TODO](#experimental--todo)

---

## Transport & Configuration

JS8Call-Improved supports **three** network interfaces, configured under
`Settings -> Reporting Tab -> API`:

### 1. Native JSON over UDP

| Setting            | Default           |
|--------------------|-------------------|
| Server address     | `127.0.0.1`       |
| Server port        | `2242`            |
| Enable checkbox    | Off               |
| Accept requests    | Separate checkbox |

- **Direction**: JS8Call *sends* UDP datagrams to the configured address:port.
  It also *receives* datagrams on the same socket (bound to an ephemeral port).
- **Framing**: Each UDP datagram is one complete JSON message (no delimiter needed;
  one datagram = one message).
- **Ping**: The UDP client sends a `PING` message every 15 seconds automatically.
- **Close**: On shutdown, the UDP client sends a `CLOSE` message.
- **Duplicate suppression**: Consecutive identical datagrams are suppressed.
- Config keys: `UDPServer`, `UDPServerPort`, `UDPEnabled`, `AcceptUDPRequests`

### 2. Native JSON over TCP

| Setting            | Default           |
|--------------------|-------------------|
| Server address     | `127.0.0.1`       |
| Server port        | `2442`            |
| Enable checkbox    | Off               |
| Accept requests    | Separate checkbox |
| Max connections    | Configurable      |

- **Direction**: JS8Call runs a TCP *server*. Clients connect to it.
- **Framing**: Newline-delimited JSON. Each message is a complete JSON object
  followed by `\n`. Clients must send newline-terminated JSON lines.
- **Reading**: The server reads via `readLine()` and parses each line as JSON.
- **Error**: If JSON parsing fails, the server responds with `API.ERROR`.
- **Connection full**: If max connections exceeded, returns `API.ERROR` with
  value `"Connections Full"` and disconnects.
- Config keys: `TCPServer`, `TCPServerPort`, `TCPEnabled`, `AcceptTCPRequests`

### 3. WSJT-X Binary UDP Protocol

| Setting            | Default           |
|--------------------|-------------------|
| Server address     | `127.0.0.1`       |
| Server port        | `2237`            |
| Enable checkbox    | Off               |

- **Direction**: JS8Call sends WSJT-X binary protocol datagrams to the configured
  address:port and receives commands back.
- **Framing**: WSJT-X binary protocol with magic number `0xadbccbda`, schema
  negotiation, QDataStream serialization. See [WSJT-X UDP Binary Protocol](#wsjt-x-udp-binary-protocol).
- Config keys: `WSJTXServer`, `WSJTXServerPort`, `WSJTXProtocolEnabled`

### Dual Protocol Note

When both UDP JSON and WSJT-X are enabled on the **same** address and port,
native JSON messages that would duplicate WSJT-X information (e.g., `RIG.FREQ`,
`STATION.STATUS`) are suppressed to avoid conflicts.

---

## Message Format

All native API messages (UDP and TCP) use JSON with this structure:

```json
{
  "type": "MESSAGE.TYPE",
  "value": "string value or empty",
  "params": {
    "_ID": 269554125481,
    "FIELD1": "value1",
    "FIELD2": 42
  }
}
```

- **`type`** (string): The message type identifier.
- **`value`** (string): Primary value, often empty string.
- **`params`** (object): Key-value map of parameters. **Required** in both directions.
- **`_ID`** (integer): Message ID. For app-originated messages, `-1`.
  For client requests, an auto-generated epoch-based value is used.
  Responses include the request's `_ID` so clients can correlate them.

### _ID Number

The ID number is the epoch time of 1499299200000 (July 6, 2017) plus current
epoch time in milliseconds.

### Error Responses

```json
{"params":{"_ID":"269558031750"},"type":"API.ERROR","value":"unterminated object: json parsing error"}
```

The `value` field contains the error description. Also used for `"Connections Full"`.

---

## Routine Messages (App to Client)

These messages are emitted automatically by JS8Call without being requested.

### RX.ACTIVITY

Emitted for every decoded frame of received activity.

**Direction**: App -> Client

```json
{
  "type": "RX.ACTIVITY",
  "value": "HC5PH: ",
  "params": {
    "_ID": -1,
    "FREQ": 7080420,
    "DIAL": 7078000,
    "OFFSET": 2420,
    "SNR": -22,
    "SPEED": 1,
    "TDRIFT": 0.265,
    "UTC": 1769740328005,
    "BITS": 0
  }
}
```

| Field    | Type    | Description                                        |
|----------|---------|----------------------------------------------------|
| FREQ     | integer | Absolute frequency (DIAL + OFFSET) in Hz           |
| DIAL     | integer | Dial frequency in Hz                               |
| OFFSET   | integer | Audio offset frequency in Hz                       |
| SNR      | integer | Signal-to-noise ratio in dB                        |
| SPEED    | integer | Submode speed (0=Normal, 1=Fast, 2=Turbo, 4=Slow, 8=Ultra) |
| TDRIFT   | float   | Time drift in seconds                              |
| UTC      | integer | UTC timestamp in milliseconds since epoch          |
| BITS     | integer | Frame bit flags (First/Last/Data indicators)       |

**Source**: `JS8_Mainwindow/processRxActivity.cpp`

---

### RX.DIRECTED

Emitted when a directed (command) message is decoded. Includes heartbeats,
SNR requests, messages, CQs, and all other JS8 directed commands.

**Direction**: App -> Client

```json
{
  "type": "RX.DIRECTED",
  "value": "KE2DMC: @HB HEARTBEAT ",
  "params": {
    "_ID": -1,
    "FROM": "KE2DMC",
    "TO": "@HB",
    "CMD": " HEARTBEAT",
    "GRID": "FN32",
    "EXTRA": "",
    "TEXT": "@HB HEARTBEAT ",
    "FREQ": 7078816,
    "DIAL": 7078000,
    "OFFSET": 816,
    "SNR": 1,
    "SPEED": 0,
    "TDRIFT": 0.54,
    "UTC": 1769740226361
  }
}
```

| Field  | Type    | Description                                          |
|--------|---------|------------------------------------------------------|
| FROM   | string  | Sending station callsign                             |
| TO     | string  | Destination callsign or group (@HB, @ALLCALL, etc.) |
| CMD    | string  | Command type (e.g., " HEARTBEAT", " MSG", " SNR", " CQ", " GRID", " HEARING") |
| GRID   | string  | Grid square of sender (if available)                 |
| EXTRA  | string  | Extra command data                                   |
| TEXT   | string  | Full message text (without FROM prefix)              |
| FREQ   | integer | Absolute frequency (DIAL + OFFSET) in Hz             |
| DIAL   | integer | Dial frequency in Hz                                 |
| OFFSET | integer | Audio offset frequency in Hz                         |
| SNR    | integer | Signal-to-noise ratio in dB                          |
| SPEED  | integer | Submode speed                                        |
| TDRIFT | float   | Time drift in seconds                                |
| UTC    | integer | UTC timestamp in milliseconds since epoch            |

**Source**: `JS8_Mainwindow/processCommandActivity.cpp`

---

### RX.SPOT

Emitted when a station is spotted (sent to reporting networks).

**Direction**: App -> Client

```json
{
  "type": "RX.SPOT",
  "value": "",
  "params": {
    "_ID": -1,
    "FREQ": 7078870,
    "DIAL": 7078000,
    "OFFSET": 870,
    "CALL": "KF7MIX",
    "SNR": -5,
    "GRID": "EM48"
  }
}
```

| Field  | Type    | Description                              |
|--------|---------|------------------------------------------|
| FREQ   | integer | Absolute frequency in Hz                 |
| DIAL   | integer | Dial frequency in Hz                     |
| OFFSET | integer | Audio offset in Hz                       |
| CALL   | string  | Spotted callsign                         |
| SNR    | integer | Signal-to-noise ratio in dB              |
| GRID   | string  | Grid square (if known)                   |

**Source**: `JS8_UI/mainwindow.cpp` line ~6879

---

### RIG.PTT

Emitted when PTT state changes (transmit on/off).

**Direction**: App -> Client

```json
{
  "type": "RIG.PTT",
  "value": "on",
  "params": {
    "_ID": -1,
    "PTT": true,
    "UTC": 1768760160665
  }
}
```

| Field | Type    | Description                                |
|-------|---------|--------------------------------------------|
| PTT   | boolean | true = transmitting, false = not           |
| UTC   | integer | UTC timestamp in milliseconds since epoch  |
| value | string  | "on" or "off"                              |

**Source**: `JS8_UI/mainwindow.cpp` `emitPTT()`

---

### TX.FRAME

Emitted when a frame of tones is being transmitted.

**Direction**: App -> Client

```json
{
  "type": "TX.FRAME",
  "value": "",
  "params": {
    "_ID": -1,
    "TONES": [4,2,5,6,1,3,0,7,1,5,7,6,0,2,2,3,...]
  }
}
```

| Field | Type         | Description                            |
|-------|--------------|----------------------------------------|
| TONES | integer[]   | Array of 79 tone values (0-7 for 8-FSK) |

**Source**: `JS8_UI/mainwindow.cpp` `emitTones()`

---

### RIG.FREQ

Emitted on band/frequency changes (automatic, not in response to a request).

**Direction**: App -> Client

```json
{
  "type": "RIG.FREQ",
  "value": "",
  "params": {
    "_ID": -1,
    "BAND": "40m",
    "FREQ": 7079950,
    "DIAL": 7078000,
    "OFFSET": 1950
  }
}
```

| Field  | Type    | Description                          |
|--------|---------|--------------------------------------|
| BAND   | string  | Band name (e.g., "40m", "20m")       |
| FREQ   | integer | Absolute frequency in Hz             |
| DIAL   | integer | Dial frequency in Hz                 |
| OFFSET | integer | Audio offset in Hz                   |

**Note**: The automatic version (band change) includes `BAND`; the response to
`RIG.GET_FREQ` does not.

**Source**: `JS8_UI/mainwindow.cpp` line ~1327

---

### STATION.STATUS (automatic)

Emitted periodically and on state changes (frequency, mode, selection changes).

**Direction**: App -> Client

```json
{
  "type": "STATION.STATUS",
  "value": "",
  "params": {
    "FREQ": 7079950,
    "DIAL": 7078000,
    "OFFSET": 1950,
    "SPEED": 0,
    "SELECTED": "KM4PVB"
  }
}
```

| Field    | Type    | Description                              |
|----------|---------|------------------------------------------|
| FREQ     | integer | Absolute frequency in Hz                 |
| DIAL     | integer | Dial frequency in Hz                     |
| OFFSET   | integer | Audio offset in Hz                       |
| SPEED    | integer | Current submode speed                    |
| SELECTED | string  | Currently selected callsign (may be "")  |

**Note**: No `_ID` field in automatic status messages (unlike request responses).

**Source**: `JS8_UI/mainwindow.cpp` line ~7167

---

### STATION.CLOSING

Emitted when JS8Call is shutting down.

**Direction**: App -> Client

```json
{
  "type": "STATION.CLOSING",
  "value": "",
  "params": {
    "_ID": -1,
    "REASON": "User closed application"
  }
}
```

| Field  | Type   | Description            |
|--------|--------|------------------------|
| REASON | string | Reason for closing     |

**Source**: `JS8_UI/mainwindow.cpp` `closeEvent()`

---

### LOG.QSO

Emitted when a QSO is logged.

**Direction**: App -> Client

```json
{
  "type": "LOG.QSO",
  "value": "<ADIF record string>",
  "params": {
    "_ID": -1,
    "UTC.ON": 1768860000000,
    "UTC.OFF": 1768860600000,
    "CALL": "W1AW",
    "GRID": "FN31",
    "FREQ": 7078000,
    "MODE": "JS8",
    "SUBMODE": "Normal",
    "RPT.SENT": "-15",
    "RPT.RECV": "-12",
    "NAME": "ARRL",
    "COMMENTS": "",
    "STATION.OP": "",
    "STATION.CALL": "WM8Q",
    "STATION.GRID": "EM85",
    "EXTRA": {}
  }
}
```

| Field        | Type    | Description                              |
|--------------|---------|------------------------------------------|
| UTC.ON       | integer | QSO start time (ms since epoch)          |
| UTC.OFF      | integer | QSO end time (ms since epoch)            |
| CALL         | string  | DX station callsign                      |
| GRID         | string  | DX station grid                          |
| FREQ         | integer | Dial frequency in Hz                     |
| MODE         | string  | Operating mode                           |
| SUBMODE      | string  | Submode name                             |
| RPT.SENT     | string  | Report sent                              |
| RPT.RECV     | string  | Report received                          |
| NAME         | string  | DX station name                          |
| COMMENTS     | string  | QSO comments                             |
| STATION.OP   | string  | Operator callsign                        |
| STATION.CALL | string  | My callsign                              |
| STATION.GRID | string  | My grid                                  |
| EXTRA        | object  | Additional fields from log dialog        |
| value        | string  | Full ADIF record text                    |

**Source**: `JS8_UI/mainwindow.cpp` `acceptQSO()` line ~4204

---

### PING (UDP auto)

The UDP client sends a PING every 15 seconds automatically.

**Direction**: App -> External UDP Server

```json
{
  "type": "PING",
  "value": "",
  "params": {
    "NAME": "JS8Call",
    "VERSION": "2.6.0-NOT_FOR_RELEASE",
    "UTC": 1769740000000
  }
}
```

**Source**: `JS8_Main/MessageClient.cpp`

---

### CLOSE (UDP auto)

Sent by the UDP client when JS8Call shuts down.

**Direction**: App -> External UDP Server

```json
{
  "type": "CLOSE",
  "value": ""
}
```

**Source**: `JS8_Main/MessageClient.cpp` destructor

---

## Command Messages (Client to App)

These messages are sent by an external client to control JS8Call.
Requires "Accept UDP/TCP requests" to be enabled.

### PING

Wakes up the API connection. No response is generated.

```json
{"params":{},"type":"PING","value":""}
```

---

### RIG.GET_FREQ

Gets the current dial and offset frequencies.

**Request**:
```json
{"params":{},"type":"RIG.GET_FREQ","value":""}
```

**Response** (`RIG.FREQ`):
```json
{
  "type": "RIG.FREQ",
  "value": "",
  "params": {
    "_ID": 269554125481,
    "FREQ": 7079950,
    "DIAL": 7078000,
    "OFFSET": 1950
  }
}
```

If WSJT-X protocol is also enabled, a WSJT-X Status message is sent too.

---

### RIG.SET_FREQ

Sets the dial frequency and/or audio offset.

**Request**:
```json
{"params":{"DIAL":7078000,"OFFSET":1950},"type":"RIG.SET_FREQ","value":""}
```

| Field  | Type    | Description                  |
|--------|---------|------------------------------|
| DIAL   | integer | Dial frequency in Hz         |
| OFFSET | integer | Audio offset in Hz           |

Either or both fields may be provided.

**Response**: A `STATION.STATUS` message is emitted as a side effect of the
frequency change.

---

### RIG.GET_PTT

Gets the current PTT (transmit) status. *API 2.6+*

**Request**:
```json
{"params":{},"type":"RIG.GET_PTT","value":""}
```

**Response** (`RIG.PTT_STATUS`):
```json
{
  "type": "RIG.PTT_STATUS",
  "value": "",
  "params": {
    "_ID": 269908447335,
    "PTT": false,
    "MESSAGE": ""
  }
}
```

| Field   | Type    | Description                                    |
|---------|---------|------------------------------------------------|
| PTT     | boolean | true if transmitting                           |
| MESSAGE | string  | Current message being transmitted, or ""       |

---

### RIG.SET_TUNE

Turns the TUNE function on or off. *API 2.6+*

**Request**:
```json
{"params":{},"type":"RIG.SET_TUNE","value":"true"}
```

| Field | Type   | Description        |
|-------|--------|--------------------|
| value | string | "true" or "false"  |

**Response** (`RIG.SET_TUNE` + `RIG.PTT`):
```json
{"params":{"_ID":270422213693,"value":true},"type":"RIG.SET_TUNE","value":""}
{"params":{"PTT":true,"UTC":1769721024401,"_ID":-1},"type":"RIG.PTT","value":"on"}
```

Both the SET_TUNE confirmation and a PTT state change message are triggered.
There is a built-in max time on tuning; no GET call exists.

---

### RIG.TX_HALT

Immediately halts the transmitter (E-stop). *API 2.6+*

**Request**:
```json
{"params":{},"type":"RIG.TX_HALT","value":""}
```

**Response** (`RIG.TX_HALT`):
```json
{"params":{"_ID":270426906894,"value":true},"type":"RIG.TX_HALT","value":""}
```

---

### STATION.GET_CALLSIGN

Gets the configured station callsign.

**Request**:
```json
{"params":{},"type":"STATION.GET_CALLSIGN","value":""}
```

**Response** (`STATION.CALLSIGN`):
```json
{"params":{"_ID":269553944755},"type":"STATION.CALLSIGN","value":"WM8Q"}
```

---

### STATION.GET_GRID

Gets the station grid square.

**Request**:
```json
{"params":{},"type":"STATION.GET_GRID","value":""}
```

**Response** (`STATION.GRID`):
```json
{"params":{"_ID":269558175403},"type":"STATION.GRID","value":"EM85"}
```

---

### STATION.SET_GRID

Sets the dynamic grid square (does not change saved config).

**Request**:
```json
{"params":{},"type":"STATION.SET_GRID","value":"EM85"}
```

**Response** (`STATION.GRID`):
```json
{"params":{"_ID":269558371794},"type":"STATION.GRID","value":"EM85"}
```

---

### STATION.GET_INFO

Gets the station info text.

**Request**:
```json
{"params":{},"type":"STATION.GET_INFO","value":""}
```

**Response** (`STATION.INFO`):
```json
{"params":{"_ID":269559398161},"type":"STATION.INFO","value":"JS8-IMPROVED VER 2.6.0"}
```

---

### STATION.SET_INFO

Sets the dynamic station info text.

**Request**:
```json
{"params":{},"type":"STATION.SET_INFO","value":"My station info"}
```

**Response** (`STATION.INFO`):
```json
{"params":{"_ID":269559620289},"type":"STATION.INFO","value":"My station info"}
```

---

### STATION.GET_STATUS

Gets the station status message.

**Request**:
```json
{"params":{},"type":"STATION.GET_STATUS","value":""}
```

**Response** (`STATION.STATUS`):
```json
{"params":{"_ID":269559773383},"type":"STATION.STATUS","value":"IDLE ..."}
```

---

### STATION.SET_STATUS

Sets the dynamic station status message.

**Request**:
```json
{"params":{},"type":"STATION.SET_STATUS","value":"My status text"}
```

**Response** (`STATION.STATUS`):
```json
{"params":{"_ID":269559773383},"type":"STATION.STATUS","value":"My status text"}
```

---

### STATION.VERSION

Gets the JS8Call version string. Use to check API compatibility. *API 2.6+*

**Request**:
```json
{"params":{},"type":"STATION.VERSION","value":""}
```

**Response** (`STATION.VERSION`):
```json
{"params":{"VERSION":"2.6.0-NOT_FOR_RELEASE","_ID":269908381596},"type":"STATION.VERSION","value":""}
```

---

### STATION.GET_OS

Gets OS information. *API 2.6+*

**Request**:
```json
{"params":{},"type":"STATION.GET_OS","value":""}
```

**Response** (`STATION.GET_OS`):
```json
{
  "type": "STATION.GET_OS",
  "value": "",
  "params": {
    "_ID": 269977840701,
    "OS_NAME": "Ubuntu 24.04.3 LTS",
    "OS_KERNEL": "linux",
    "OS_KERNEL_VERSION": "6.14.0-37-generic"
  }
}
```

---

### STATION.GET_SPOT

Gets the current spot button status. *API 2.6+*

**Request**:
```json
{"params":{},"type":"STATION.GET_SPOT","value":""}
```

**Response** (`STATION.SPOT`):
```json
{"params":{"_ID":270409823907,"value":true},"type":"STATION.SPOT","value":""}
```

---

### STATION.SET_SPOT

Sets the spot button on or off. *API 2.6+*

**Request**:
```json
{"params":{},"type":"STATION.SET_SPOT","value":"true"}
```

| Field | Type   | Description       |
|-------|--------|-------------------|
| value | string | "true" or "false" |

**Response** (`STATION.SPOT`):
```json
{"params":{"_ID":270409737648,"value":true},"type":"STATION.SPOT","value":""}
```

---

### RX.GET_CALL_ACTIVITY

Returns recently heard callsigns, filtered by the configured callsign aging.

**Request**:
```json
{"params":{},"type":"RX.GET_CALL_ACTIVITY","value":""}
```

**Response** (`RX.CALL_ACTIVITY`):
```json
{
  "type": "RX.CALL_ACTIVITY",
  "value": "",
  "params": {
    "_ID": 269560000000,
    "AB4WV": {"GRID":"","SNR":-18,"UTC":1768858992167},
    "K4EXA": {"GRID":"EM63","SNR":-18,"UTC":1768858242147}
  }
}
```

Each callsign is a key in `params` with a nested object containing `GRID`, `SNR`, and `UTC`.

---

### RX.GET_CALL_SELECTED

Returns the currently selected callsign in the UI.

**Request**:
```json
{"params":{},"type":"RX.GET_CALL_SELECTED","value":""}
```

**Response** (`RX.CALL_SELECTED`):
```json
{"params":{"_ID":269560649847},"type":"RX.CALL_SELECTED","value":"KM4PVB"}
```

Value is empty string if no callsign is selected.

---

### RX.GET_BAND_ACTIVITY

Returns current band activity keyed by frequency offset.

**Request**:
```json
{"params":{},"type":"RX.GET_BAND_ACTIVITY","value":""}
```

**Response** (`RX.BAND_ACTIVITY`):
```json
{
  "type": "RX.BAND_ACTIVITY",
  "value": "",
  "params": {
    "_ID": 269560000000,
    "1067": {
      "DIAL": 7078000,
      "FREQ": 7079067,
      "OFFSET": 1067,
      "TEXT": "W6OEM: VE3SOY HEARTBEAT SNR -20 ",
      "SNR": -7,
      "UTC": 1768860611907
    }
  }
}
```

Each offset value is a key in `params` with a nested detail object.

---

### RX.GET_TEXT

Gets the contents of the directed message (RX) window (last 1024 characters).

**Request**:
```json
{"params":{},"type":"RX.GET_TEXT","value":""}
```

**Response** (`RX.TEXT`):
```json
{"params":{"_ID":269562514193},"type":"RX.TEXT","value":"...decoded text..."}
```

---

### TX.GET_TEXT

Gets the current text in the transmit message box (last 1024 characters).

**Request**:
```json
{"params":{},"type":"TX.GET_TEXT","value":""}
```

**Response** (`TX.TEXT`):
```json
{"params":{"_ID":269562593590},"type":"TX.TEXT","value":""}
```

---

### TX.SET_TEXT

Sets the text in the transmit message box.

**Request**:
```json
{"params":{},"type":"TX.SET_TEXT","value":"KJ4CTD: KJ4YQK HELLO"}
```

**Response** (`TX.TEXT`):
```json
{"params":{"_ID":269563119535},"type":"TX.TEXT","value":"KJ4CTD: KJ4YQK HELLO"}
```

---

### TX.SEND_MESSAGE

Enqueues a message for transmission in the next transmit cycle.

**Request**:
```json
{"params":{},"type":"TX.SEND_MESSAGE","value":"KJ4CTD: W1AW HELLO"}
```

**IMPORTANT**: If the message text box already has text displayed, the new message
may not be transmitted. The value is placed in the transmit queue via
`enqueueMessage()` and `processTxQueue()`.

**Response**: The app emits `RIG.PTT` (on), `TX.FRAME` (tones), `RIG.PTT` (off)
sequences as transmission occurs.

---

### TX.GET_QUEUE_DEPTH

Gets the number of messages remaining in the transmit queue. *API 2.6+*

**Request**:
```json
{"params":{},"type":"TX.GET_QUEUE_DEPTH","value":""}
```

**Response** (`TX.QUEUE_DEPTH`):
```json
{"params":{"DEPTH":2,"_ID":270440267253},"type":"TX.QUEUE_DEPTH","value":""}
```

| Field | Type    | Description                                           |
|-------|---------|-------------------------------------------------------|
| DEPTH | integer | Number of queued messages (1 if transmitting + empty queue) |

---

### MODE.GET_SPEED

Gets the current transmission speed mode.

**Request**:
```json
{"params":{},"type":"MODE.GET_SPEED","value":""}
```

**Response** (`MODE.SPEED`):
```json
{"params":{"SPEED":0,"_ID":269564224294},"type":"MODE.SPEED","value":""}
```

### Mode Speed Values

| Mode   | Number |
|--------|--------|
| Normal | 0      |
| Fast   | 1      |
| Turbo  | 2      |
| Slow   | 4      |
| Ultra  | 8      |

Ultra speed is **experimental** and unreliable.

---

### MODE.SET_SPEED

Sets the transmission speed mode.

**Request**:
```json
{"params":{"SPEED":0},"type":"MODE.SET_SPEED","value":""}
```

| Field | Type    | Description        |
|-------|---------|--------------------|
| SPEED | integer | Mode speed number  |

**Response** (`MODE.SET_SPEED`):
```json
{"params":{"SPEED":0,"_ID":270412242558},"type":"MODE.SET_SPEED","value":""}
```

May also trigger a `STATION.STATUS` message as a side effect.

---

### INBOX.GET_MESSAGES

Fetches all inbox messages. Can filter by callsign. **Warning**: response can be very large.

**Request**:
```json
{"params":{},"type":"INBOX.GET_MESSAGES","value":""}
```

Or filtered:
```json
{"params":{"CALLSIGN":"W1AW"},"type":"INBOX.GET_MESSAGES","value":""}
```

**Response** (`INBOX.MESSAGES`):
```json
{
  "type": "INBOX.MESSAGES",
  "value": "",
  "params": {
    "_ID": 269699597005,
    "MESSAGES": [
      {
        "type": "READ",
        "value": "",
        "params": {
          "CMD": " MSG ",
          "DIAL": 7078000,
          "FREQ": 7080318,
          "FROM": "KJ5MIW",
          "GRID": " EM15",
          "OFFSET": 2318,
          "PATH": "KJ5MIW",
          "SNR": -15,
          "SUBMODE": 0,
          "TDRIFT": 0.14,
          "TEXT": "F!104 100 ST[OK] GR[EM15] #ATTV",
          "TO": "@SITREP",
          "UTC": "2026-01-21 01:44:26",
          "_ID": "269660742003"
        }
      }
    ]
  }
}
```

Message types in the list can be `"STORE"`, `"READ"`, or `"UNREAD"`.

---

### INBOX.STORE_MESSAGE

Stores a message in your local inbox.

**Request**:
```json
{"params":{"CALLSIGN":"W1AW","TEXT":"Hello from the API"},"type":"INBOX.STORE_MESSAGE","value":""}
```

| Field    | Type   | Description              |
|----------|--------|--------------------------|
| CALLSIGN | string | "TO" callsign            |
| TEXT     | string | Message text to store    |

**Response** (`INBOX.MESSAGE`):
```json
{"params":{"ID":228,"_ID":269569847892},"type":"INBOX.MESSAGE","value":""}
```

| Field | Type    | Description                |
|-------|---------|----------------------------|
| ID    | integer | Database row ID of stored message |

---

### WINDOW.RAISE

Brings the JS8Call window to the foreground (OS permitting).

**Request**:
```json
{"params":{},"type":"WINDOW.RAISE","value":""}
```

**Response**: None.

---

## WSJT-X UDP Binary Protocol

JS8Call-Improved can optionally send and receive WSJT-X binary protocol messages
over UDP. This allows integration with logging programs (e.g., JTAlert, GridTracker,
Log4OM) that support the WSJT-X protocol.

### Configuration

- Enable in `Settings -> Reporting -> WSJT-X Protocol`
- Default port: **2237**
- Default server: `127.0.0.1`
- Supports multicast addresses with configurable TTL
- Schema negotiation: starts at schema 2, negotiates up to 3

### Outgoing Messages (JS8Call -> Client)

| Type         | ID | Description                                        |
|--------------|----|----------------------------------------------------|
| Heartbeat    | 0  | Periodic presence announcement (every 15s)         |
| Status       | 1  | Station status (freq, mode, TX state, callsigns)   |
| Decode       | 2  | Decoded message (SNR, time, freq, text)            |
| Clear        | 3  | Clear decodes notification                         |
| QSOLogged    | 5  | Logged QSO details                                 |
| Close        | 6  | Application shutting down                          |
| LoggedADIF   | 12 | ADIF record for logged QSO (with WSJT-X header)   |

### Incoming Messages (Client -> JS8Call)

| Type      | ID | Description                              | Mapping                   |
|-----------|-----|------------------------------------------|---------------------------|
| Reply     | 4  | Reply to a decode                        | TODO (not yet mapped)     |
| Clear     | 3  | Clear decodes request                    | Emits `clear_decodes`     |
| Close     | 6  | Close application                        | Emits `close`             |
| Replay    | 7  | Replay old decodes                       | Emits `replay`            |
| HaltTx    | 8  | Halt transmission                        | TODO (not fully mapped)   |
| FreeText  | 9  | Set free text and optionally send        | Maps to `TX.SET_TEXT` + `TX.SEND_MESSAGE` |
| Location  | 11 | Set grid location                        | Maps to `STATION.SET_GRID` |

### Binary Format

- **Magic number**: `0xadbccbda` (4 bytes, big-endian)
- **Schema number**: `quint32` (4 bytes)
- **Message type**: `quint32` (4 bytes)
- **Application ID**: `utf8` string (length-prefixed QByteArray)
- **Payload**: Type-specific fields serialized via QDataStream (Qt 5.4 format for schema 3)

Strings are serialized as QByteArray: `quint32` length followed by UTF-8 bytes.
Null strings use length `0xffffffff`.

**Source files**: `JS8_UDP/WSJTXMessageClient.cpp`, `JS8_UDP/WSJTXMessageMapper.cpp`,
`JS8_UDP/NetworkMessage.h`

---

## Experimental / TODO

These are message types found in the source code that are commented out,
partially implemented, or marked as TODO.

### MAIN.RX / MAIN.TX / MAIN.AUTO / MAIN.HB

Found in `networkMessage.cpp` as TODO comments. Inspired by FLDigi.
Not implemented.

```cpp
// TODO: MAIN.RX - Turn on RX
// TODO: MAIN.TX - Transmit
// TODO: MAIN.AUTO - Auto
// TODO: MAIN.HB - HB
```

### RX.DIRECTED.ME

Found in `processCommandActivity.cpp` (commented out). Would emit directed
messages specifically addressed to the local station as a separate message type.

```json
{
  "type": "RX.DIRECTED.ME",
  "value": "...",
  "params": {
    "_ID": -1,
    "FROM": "...",
    "TO": "...",
    "CMD": "...",
    ...same fields as RX.DIRECTED...
  }
}
```

### WSJT-X Reply mapping

The `handleReply()` in `WSJTXMessageMapper.cpp` has a TODO to map WSJT-X Reply
messages to JS8Call actions (e.g., double-click decode simulation).

### WSJT-X HaltTx mapping

The `handleHaltTx()` in `WSJTXMessageMapper.cpp` has a TODO to map WSJT-X
HaltTx messages to immediately stop TX in JS8Call.

---

## Complete Message Type Reference

### Messages emitted by JS8Call (App -> Client)

| Type              | Trigger                                  | Auto/Response |
|-------------------|------------------------------------------|---------------|
| `RIG.PTT`         | PTT state changes                        | Auto          |
| `RIG.FREQ`        | Band/frequency change; or RIG.GET_FREQ   | Both          |
| `RIG.PTT_STATUS`  | Response to RIG.GET_PTT                  | Response      |
| `RIG.SET_TUNE`    | Response to RIG.SET_TUNE                 | Response      |
| `RIG.TX_HALT`     | Response to RIG.TX_HALT                  | Response      |
| `RX.ACTIVITY`     | Every decoded frame                      | Auto          |
| `RX.DIRECTED`     | Directed/command message decoded         | Auto          |
| `RX.SPOT`         | Station spotted                          | Auto          |
| `RX.CALL_ACTIVITY`| Response to RX.GET_CALL_ACTIVITY         | Response      |
| `RX.CALL_SELECTED`| Response to RX.GET_CALL_SELECTED         | Response      |
| `RX.BAND_ACTIVITY`| Response to RX.GET_BAND_ACTIVITY         | Response      |
| `RX.TEXT`         | Response to RX.GET_TEXT                   | Response      |
| `TX.TEXT`         | Response to TX.GET_TEXT or TX.SET_TEXT    | Response      |
| `TX.FRAME`        | Each transmitted frame (tones)           | Auto          |
| `TX.QUEUE_DEPTH`  | Response to TX.GET_QUEUE_DEPTH           | Response      |
| `STATION.CALLSIGN`| Response to STATION.GET_CALLSIGN         | Response      |
| `STATION.GRID`    | Response to STATION.GET/SET_GRID         | Response      |
| `STATION.INFO`    | Response to STATION.GET/SET_INFO         | Response      |
| `STATION.STATUS`  | Periodic/state change; or GET_STATUS     | Both          |
| `STATION.CLOSING` | Application shutting down                | Auto          |
| `STATION.VERSION` | Response to STATION.VERSION              | Response      |
| `STATION.GET_OS`  | Response to STATION.GET_OS               | Response      |
| `STATION.SPOT`    | Response to STATION.GET/SET_SPOT         | Response      |
| `MODE.SPEED`      | Response to MODE.GET_SPEED               | Response      |
| `MODE.SET_SPEED`  | Response to MODE.SET_SPEED               | Response      |
| `INBOX.MESSAGES`  | Response to INBOX.GET_MESSAGES           | Response      |
| `INBOX.MESSAGE`   | Response to INBOX.STORE_MESSAGE          | Response      |
| `LOG.QSO`         | QSO logged                               | Auto          |
| `API.ERROR`       | JSON parse error or connection full      | Auto          |
| `PING`            | UDP client auto-ping every 15s           | Auto (UDP)    |
| `CLOSE`           | UDP client shutdown                      | Auto (UDP)    |

### Messages accepted by JS8Call (Client -> App)

| Type                   | Action                                 | Since |
|------------------------|----------------------------------------|-------|
| `PING`                 | Wake up / no-op                        | 2.5   |
| `RIG.GET_FREQ`         | Get frequency info                     | 2.5   |
| `RIG.SET_FREQ`         | Set dial frequency and/or offset       | 2.5   |
| `RIG.GET_PTT`          | Get PTT status                         | 2.6   |
| `RIG.SET_TUNE`         | Toggle tune on/off                     | 2.6   |
| `RIG.TX_HALT`          | Emergency stop transmitter             | 2.6   |
| `STATION.GET_CALLSIGN` | Get station callsign                   | 2.5   |
| `STATION.GET_GRID`     | Get grid square                        | 2.5   |
| `STATION.SET_GRID`     | Set dynamic grid square                | 2.5   |
| `STATION.GET_INFO`     | Get station info                       | 2.5   |
| `STATION.SET_INFO`     | Set dynamic station info               | 2.5   |
| `STATION.GET_STATUS`   | Get station status                     | 2.5   |
| `STATION.SET_STATUS`   | Set dynamic station status             | 2.5   |
| `STATION.VERSION`      | Get JS8Call version                    | 2.6   |
| `STATION.GET_OS`       | Get OS information                     | 2.6   |
| `STATION.GET_SPOT`     | Get spot button state                  | 2.6   |
| `STATION.SET_SPOT`     | Set spot button state                  | 2.6   |
| `RX.GET_CALL_ACTIVITY` | Get heard callsigns                    | 2.5   |
| `RX.GET_CALL_SELECTED` | Get selected callsign                  | 2.5   |
| `RX.GET_BAND_ACTIVITY` | Get band activity                      | 2.5   |
| `RX.GET_TEXT`          | Get RX text window contents            | 2.5   |
| `TX.GET_TEXT`          | Get TX text box contents               | 2.5   |
| `TX.SET_TEXT`          | Set TX text box contents               | 2.5   |
| `TX.SEND_MESSAGE`     | Queue message for transmission         | 2.5   |
| `TX.GET_QUEUE_DEPTH`  | Get TX queue depth                     | 2.6   |
| `MODE.GET_SPEED`      | Get current speed mode                 | 2.5   |
| `MODE.SET_SPEED`      | Set speed mode                         | 2.5   |
| `INBOX.GET_MESSAGES`  | Get inbox messages                     | 2.5   |
| `INBOX.STORE_MESSAGE` | Store message in inbox                 | 2.5   |
| `WINDOW.RAISE`        | Bring window to front                  | 2.5   |

---

## Quick Start Example

### Connect via TCP (telnet)

```bash
telnet 127.0.0.1 2442
```

### Get station callsign

```json
{"params":{},"type":"STATION.GET_CALLSIGN","value":""}
```

### Send a message

```json
{"params":{},"type":"TX.SEND_MESSAGE","value":"WM8Q: W1AW HELLO"}
```

### Monitor for decodes

Simply connect and listen. `RX.ACTIVITY` and `RX.DIRECTED` messages will stream
as stations are decoded.

---

## Source File Index

| File | Purpose |
|------|---------|
| `JS8_Main/Message.h` | Message class (JSON serialization) |
| `JS8_Main/MessageServer.h/.cpp` | TCP server (accepts client connections) |
| `JS8_Main/MessageClient.h/.cpp` | UDP client (sends to external server) |
| `JS8_Mainwindow/networkMessage.cpp` | Command router (all request handling) |
| `JS8_Mainwindow/processRxActivity.cpp` | RX.ACTIVITY emission |
| `JS8_Mainwindow/processCommandActivity.cpp` | RX.DIRECTED emission |
| `JS8_UI/mainwindow.cpp` | PTT, tones, spots, QSO log, status, sendNetworkMessage() |
| `JS8_UI/Configuration.cpp` | Port/address/enable settings |
| `JS8_UDP/WSJTXMessageClient.h/.cpp` | WSJT-X binary UDP protocol client |
| `JS8_UDP/WSJTXMessageMapper.h/.cpp` | Maps WSJT-X messages to JS8Call actions |
| `JS8_UDP/NetworkMessage.h` | WSJT-X binary protocol definition |
