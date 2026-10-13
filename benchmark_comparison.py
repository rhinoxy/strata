#!/usr/bin/env python3
import subprocess
import time
import json
import requests
import os
import signal
import sys

PROMPT = "マルチソケットワークステーションにおけるNUMA最適化のメリットを簡潔に教えてください。"
MAX_TOKENS = 120
PORT = 8081

CASES = [
    {
        "name": "1. 初期デフォルト (Default: spec 4, min-p 0.5, no NUMA, no HSA)",
        "cmd": ["/home/your_name/Strata/.venv/bin/python", "/home/your_name/Strata/serve/server.py", 
                "--engine", "strata", "--config", "/home/your_name/Strata/strata-benchmark-default.json", "--port", str(PORT)],
        "use_numactl": False
    },
    {
        "name": "2. NUMAインターリーブのみ (NUMA only: spec 4, min-p 0.5, numactl, no HSA)",
        "cmd": ["numactl", "--interleave=all", "/home/your_name/Strata/.venv/bin/python", "/home/your_name/Strata/serve/server.py", 
                "--engine", "strata", "--config", "/home/your_name/Strata/strata-benchmark-default.json", "--port", str(PORT)],
        "use_numactl": True
    },
    {
        "name": "3. フル最適化 (Optimized: spec 2, min-p 0.6, numactl, HSA_FORCE_FINE_GRAIN_PCIE=1)",
        "cmd": ["numactl", "--interleave=all", "/home/your_name/Strata/.venv/bin/python", "/home/your_name/Strata/serve/server.py", 
                "--engine", "strata", "--config", "/home/your_name/Strata/strata-iq3_s.json", "--port", str(PORT)],
        "use_numactl": True
    }
]

def kill_existing():
    subprocess.run(["pkill", "-9", "-f", "engine/strata"], stderr=subprocess.DEVNULL)
    subprocess.run(["pkill", "-9", "-f", "serve/server.py"], stderr=subprocess.DEVNULL)
    time.sleep(3)

def wait_for_server(timeout=180):
    start = time.time()
    while time.time() - start < timeout:
        try:
            r = requests.get(f"http://127.0.0.1:{PORT}/v1/models", timeout=2)
            if r.status_code == 200:
                return True
        except Exception:
            pass
        time.sleep(2)
    return False

def run_case(case):
    print(f"\n=======================================================", flush=True)
    print(f"Running: {case['name']}", flush=True)
    print(f"Command: {' '.join(case['cmd'])}", flush=True)
    print(f"=======================================================", flush=True)
    
    kill_existing()
    
    # Launch process
    log_file = open(f"/home/your_name/Strata/bench_{case['name'][:1]}.log", "w")
    proc = subprocess.Popen(case['cmd'], cwd="/home/your_name/Strata", stdout=log_file, stderr=subprocess.STDOUT)
    
    print("Waiting for model loading...", flush=True)
    if not wait_for_server():
        print("ERROR: Server failed to start within timeout!", flush=True)
        proc.kill()
        kill_existing()
        return None
    
    print("Server ready! Sending inference request...", flush=True)
    time.sleep(2)
    
    payload = {
        "model": "qwen3.8-flash-next-iq3_s",
        "messages": [{"role": "user", "content": PROMPT}],
        "max_tokens": MAX_TOKENS
    }
    
    req_start = time.time()
    try:
        resp = requests.post(f"http://127.0.0.1:{PORT}/v1/chat/completions", json=payload, timeout=180)
        req_elapsed = time.time() - req_start
        data = resp.json()
    except Exception as e:
        print(f"Request failed: {e}", flush=True)
        data = None
    
    print("Stopping server...", flush=True)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    kill_existing()
    log_file.close()
    
    if data and "timings" in data:
        t = data["timings"]
        c = data.get("choices", [{}])[0].get("message", {})
        res = {
            "name": case["name"],
            "wall_time_s": round(req_elapsed, 2),
            "predicted_per_second": t.get("predicted_per_second"),
            "predicted_n": t.get("predicted_n"),
            "predicted_ms": t.get("predicted_ms"),
            "draft_n": t.get("draft_n", 0),
            "draft_n_accepted": t.get("draft_n_accepted", 0),
            "acceptance_rate": round(t.get("draft_n_accepted", 0) / max(1, t.get("draft_n", 1)) * 100, 1),
            "content_preview": c.get("content", "")[:40] + "..."
        }
        print(f"Result: {json.dumps(res, ensure_ascii=False, indent=2)}", flush=True)
        return res
    else:
        print(f"Raw response: {data}", flush=True)
        return None

def main():
    results = []
    for c in CASES:
        r = run_case(c)
        if r:
            results.append(r)
        time.sleep(5)
        
    print("\n\n" + "="*70, flush=True)
    print("BENCHMARK SUMMARY", flush=True)
    print("="*70, flush=True)
    print(json.dumps(results, ensure_ascii=False, indent=2), flush=True)
    with open("/home/your_name/Strata/benchmark_summary.json", "w") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)

if __name__ == "__main__":
    main()
