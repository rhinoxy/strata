#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
exec numactl --interleave=all "$SCRIPT_DIR/.venv/bin/python" "$SCRIPT_DIR/serve/server.py" "--engine" "strata" "--config" "$SCRIPT_DIR/strata-iq3_s.json" "--port" "8081" "$@"
