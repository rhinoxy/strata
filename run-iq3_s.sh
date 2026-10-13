#!/bin/sh
cd "/home/k-takeda/Strata"
exec numactl --interleave=all "/home/k-takeda/Strata/.venv/bin/python" "/home/k-takeda/Strata/serve/server.py" "--engine" "strata" "--config" "/home/k-takeda/Strata/strata-iq3_s.json" "--port" "8081" "$@"
