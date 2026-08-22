#!/bin/bash
# js8api.sh — send a command to JS8Call TCP API (port 2442)
# Usage: js8api.sh COMMAND [VALUE]
# Example: js8api.sh RX.GET_BAND_ACTIVITY
#          js8api.sh TX.SEND_MESSAGE "WM8Q: CQ CQ CQ"

PORT=${JS8API_PORT:-2442}
HOST=${JS8API_HOST:-localhost}

CMD="${1:?Usage: js8api.sh COMMAND [VALUE]}"
VAL="${2:-}"

printf '{"type":"%s","value":"%s","params":{}}\n' "$CMD" "$VAL" | \
  nc -w3 "$HOST" "$PORT"
