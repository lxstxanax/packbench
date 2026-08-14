#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -d "$ROOT/.venv" ]]; then
    echo "Run ./install.sh first"
    exit 1
fi

exec "$ROOT/.venv/bin/python" "$ROOT/app.py" "$@"
