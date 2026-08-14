#!/usr/bin/env bash
# Live 115200-baud serial view of the ST-Link Virtual COM Port (auto-detects the device).
# This is the firmware's ANSI dashboard; keys: d j r c p h.
# Exit: Ctrl+A then Ctrl+X.

port=$(find /dev/serial/by-id -iname '*stlink*' 2>/dev/null | head -1)
[ -z "$port" ] && port=$(ls /dev/ttyACM* 2>/dev/null | head -1)

if [ -z "$port" ]; then
    echo "ST-Link VCP not found — is the board plugged in?"
    exit 1
fi

echo "Connected to $port @ 115200 (exit: Ctrl+A, Ctrl+X)"
# --imap lfcrlf is deliberately NOT used here: the firmware already sends
# CRLF and ANSI cursor commands, and remapping them breaks the redraw.
exec picocom -b 115200 "$port"
