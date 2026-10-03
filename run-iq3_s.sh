#!/bin/sh
cd "/home/your_name/Strata"
exec numactl --interleave=all "/home/your_name/Strata/.venv/bin/python" "/home/your_name/Strata/serve/server.py" "--engine" "strata" "--config" "/home/your_name/Strata/strata-iq3_s.json" "--port" "8081" "$@"
