#!/bin/sh
cd "$HOME/Strata"
exec numactl --interleave=all "$HOME/Strata/.venv/bin/python" "$HOME/Strata/serve/server.py" "--engine" "strata" "--config" "$HOME/Strata/strata-iq3_s.json" "--port" "8081" "$@"
