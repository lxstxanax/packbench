#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -d "$ROOT/.venv" ]]; then
    echo "Run ./install.sh first"
    exit 1
fi

# The serial port is exclusive. A second instance does not fail cleanly —
# the two fight over the port and each shows a connection that keeps
# dropping, which looks like a hardware fault and is not one.
running=$(pgrep -f "$ROOT/app.py" | grep -v "^$$\$" || true)
if [[ -n "$running" ]]; then
    echo "The dashboard is already running (PID $(echo "$running" | tr '\n' ' '))."
    echo "Close it first, or: kill $(echo "$running" | head -1)"
    exit 1
fi

exec "$ROOT/.venv/bin/python" "$ROOT/app.py" "$@"
