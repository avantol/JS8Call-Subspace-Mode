#!/bin/bash
# msg-rotator.sh — thin launcher for the Python rotator (TODO #155:
# event-driven js8client library replaced the nc/polling implementation;
# the old shell version is preserved as msg-rotator.sh.bak).
# Usage unchanged: msg-rotator.sh [MSG_FILE] [INTERVAL_SEC|MIN-MAX]
exec python3 "$(dirname "$(readlink -f "$0")")/tools/msg-rotator.py" "$@"
