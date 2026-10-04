# Community benchmark: 2x RX 9060 XT (16 GB), dual Xeon E5-2687W v4, OS tuning + prefill auto

Measured on 2026-10-04/05 by rhinoxy. This tests Strata 0.1.38 with the original
Flash-Next GSQ-RCO IQ3_S, two GPUs with an automatic layer split, and a
262,144-token context limit, on a dual-socket Xeon workstation with the model on
a spinning HDD. It also measures the effect of three OS-level fixes (memlock,
hugepages) and `--prefill 2048` -> `--prefill auto`.

Median decode throughput was **40.2 tok/s at 4,096 prompt tokens, 37.1 tok/s at
32,768, and 36.4 tok/s at 128,000**; median prompt throughput was **609 tok/s at
4K, 1,065 tok/s at 32K, and 1,174 tok/s at 128K** with `--prefill auto`. The
same runs with `--prefill 2048` measured **529 / 633 / 657 tok/s prompt and
41.3 / 39.7 / 34.0 tok/s decode**: chunk 8192 is +15% at 4K, +68% at 32K, and
+79% at 128K prompt speed, with decode unchanged. These are synthetic
code-explanation requests with greedy decoding and a 256-token output cap. They
do not establish general answer quality or performance on other workloads.

## Hardware and software

- 2x AMD Radeon RX 9060 XT, 16,311 MiB VRAM each (gfx1200, 16 CUs at 2.74 GHz),
  selected as `gpu: [1, 2]`. A third card (RX 6400, gfx1034) is present but not
  used by the engine. The engine's startup host-to-device probe measured
  12.7-13.3 GB/s (PCIe 4.0 x16 class), from which it chose `pcie_frac 0.35-0.37`
  (default 0.55). GPU clocks were not fixed for this test.
- 2x Intel Xeon E5-2687W v4 @ 3.00 GHz; 24 cores / 48 threads total; no AVX-512
  (the engine reported it runs the expert kernels on AVX-2). 23 expert-pool
  workers. The launcher runs the server under `numactl --interleave=all`.
- 188 GiB installed RAM; about 113 GiB available at benchmark start. Swap: an
  8 GiB swapfile. Storage: repository, packs, and GGUF shards on a Seagate
  ST2000DM001 2 TB HDD (rotational).
- Ubuntu, kernel 7.0.0-30-generic, ROCm 7.2.4 (HIP backend).
- Source commit `7462f3f` (v0.1.38-10-g7462f3f); engine 0.1.38 (reported by
  `/v1/status`). Note: this repository's history was rewritten on 2026-10-05 to
  remove a committed API key (config and docs files only); the engine sources
  and binaries are unchanged, and the pre-rewrite commit hash of the tested
  build is recorded here for reproducibility.
- Other workloads: an OpenClaw agent session shares the server during the runs
  (see Correctness and limitations).

## OS-level fixes measured

1. **PLE table mlock.** The PLE table is ~28.8 GiB (320,001,536 rows x 90 B).
   The session's memlock limit was 23.6 GiB, so the engine logged
   `PLE table mlock failed (Cannot allocate memory; raise ulimit -l)` and the
   table could be evicted under memory pressure (a long session was once
   observed collapsing to 4.3 tok/s decode). Fix: `LimitMEMLOCK=infinity` in
   the systemd user unit **plus** `/etc/security/limits.d/99-strata-memlock.conf`
   (`your_name soft/hard memlock unlimited`) - a user-level systemd unit clamps
   `LimitMEMLOCK` to the session's hard limit, so the limits.d entry is what
   actually raises it. The engine then logs `PLE table locked in RAM`.
2. **2 MiB hugepages for the expert arena.** The engine wants 23,983 x 2 MiB
   pages (~47 GiB). Setting `vm.nr_hugepages=24000` at runtime on the live
   system only reached 5,786/24,000 pages (fragmentation; the engine needs
   all-or-nothing and falls back to 4 KB pages). Persisting it in
   `/etc/sysctl.d/99-strata-hugepages.conf` and rebooting allocated all 24,000
   pages early in boot; the engine then logs `hugetlb 2 MB pages`.
3. **`--prefill auto`.** The engine picked chunk 8192 by itself
   (`prompt chunk auto: 8192 tokens`, borrowing 4.69 GiB of expert-cache slots
   per GPU for the prompt path).

After the fixes, the PLE table load at startup dropped from 45-592 s (cold) to
**0.7 s** (warm, locked + hugepages).

## Model and configuration

Model: `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF` IQ3_S (file names and
sizes are in [model-files.txt](model-files.txt)):

- `IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf` (54,817,524,224 B)
- `IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00002-of-00002.gguf` (28,800,138,432 B, the PLE shard)

Vision encoder: none (`images: false`). Custom pack
(`Strata-data/packs/iq3_s`) and an expert profile
(`data/expert-profile.bin`, 24,576 ranked pairs, from a previous `--calibrate`
on this machine); the MTP draft head is the released one
(`Strata-data/mtp/rt`).

- Context limit 262,144; KV int8 with a 32,768-token resident window (the rest
  streams through 1.55 GiB of pinned RAM; 96-99% of block reads hit VRAM).
- Expert cache auto: 6,055 slots on GPU0 + 4,731 on GPU1; decode hit rate
  84-96% across the runs.
- `--prefill auto` (chunk 8192) for the "after" runs; `--prefill 2048` for the
  "before" runs.
- MTP on (`--spec 4 --spec-min-p 0.5`); reasoning effort `none` in requests.
- Layer split auto across both 9060 XTs (`gpu: [1, 2]`): layers 0-23 on GPU0,
  layers 24-47 + head on GPU1; each card holds its weights.
- `--ple-io ram` (PLE table resident and mlocked in host RAM).
- Environment: `HSA_FORCE_FINE_GRAIN_PCIE=1`,
  `STRATA_HIPBLASLT_TUNING=tools/hip/gfx1200-hipblaslt-100202.txt`.
- Sampling: temperature 0. Calibration: yes (a previous run kept the settings).
  No experimental speed projection.

```text
~/Strata/run-iq3_s.sh   # numactl --interleave=all .venv/bin/python serve/server.py
  --engine strata --config strata-iq3_s.json --port 8081
# the config passes to the engine:
#   --pack .../packs/iq3_s --native <shard 1> --ple-gguf <shard 2>
#   --expert-profile data/expert-profile.bin --expert-cache auto
#   --prefill auto --ple-io ram --spec 4 --spec-min-p 0.5 --mtp .../mtp/rt
#   --max-context 262144 --kv int8 --kv-resident 32768 --layer-split auto
# (the full JSON is kept local: it contains the server's API key)
```

## Method

The benchmark script is [the serial fresh-prompt script from the RTX 5090
community report](../2026-09-30-community-rtx-5090/benchmark.py), API-key
variant ([benchmark.py](benchmark.py)), run against this server's tokenizer
pack. For each of 4,096 / 32,768 / 128,000 prompt tokens it performs 3 runs;
each request carries a unique nonce so the conversation cache never reuses a
previous prompt (`reused: 0` on all measured runs, confirmed by the engine's
per-request metrics). One warm-up request (16 output tokens) preceded the
measured runs; model loading is not included in any timing.

The "after" set ran first with `--prefill auto`; the server was then restarted
with `--prefill 2048` and the identical script re-run into
[before-prefill-2048/](before-prefill-2048/). The 128K run 3 of the "before"
set was re-run once with a different nonce after the first attempt was
interrupted; its engine timings are in the same range as its siblings.

Requests are greedy (`temperature 0`), `reasoning_effort: "none"`, with a
256-token output cap; all measured runs ended on the cap
(`finish_reason: "length"`, 256 generated tokens each). Prompt and decode tok/s
are computed from the engine's per-request timings (`prompt_ms`, `decode_ms`),
not from total request time. TTFT and elapsed time are client-side streaming
measurements. The expert cache warmed across the runs (per-request hit rate
84-96%; values in [results.json](results.json)). Canceled or failed requests:
none.

Per-run data: [results.json](results.json) and [before-prefill-2048/results.json](before-prefill-2048/results.json)
(one row per run with engine timings, usage, and response text), per-run
`tokens-*-raw.json` and `tokens-*-request.json`, engine timing lines in
[engine-timing-lines.txt](engine-timing-lines.txt) and
[before-prefill-2048/engine-timing-lines.txt](before-prefill-2048/engine-timing-lines.txt),
system details in [environment.txt](environment.txt).

## Results

| Configuration | Actual prompt tokens | Reused tokens | Generated tokens | Runs | Prompt tok/s median (min-max) | Decode tok/s median (min-max) | TTFT s median (min-max) |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| IQ3_S, 2 GPUs, `--prefill auto` (chunk 8192), 4,096-token prompt | 4,096 | 0 | 256 | 3 | 609 (604-609) | 40.2 (37.2-41.4) | 6.8 (6.8-71.4) |
| IQ3_S, 2 GPUs, `--prefill auto` (chunk 8192), 32,768-token prompt | 32,768 | 0 | 256 | 3 | 1,065 (1,063-1,065) | 37.1 (37.0-38.2) | 138.7 (99.6-172.1) |
| IQ3_S, 2 GPUs, `--prefill auto` (chunk 8192), 128,000-token prompt | 128,000 | 0 | 256 | 3 | 1,174 (1,173-1,176) | 36.4 (36.0-38.1) | 177.7 (170.6-237.9) |
| IQ3_S, 2 GPUs, `--prefill 2048`, 4,096-token prompt | 4,096 | 0 | 256 | 3 | 529 (522-531) | 41.3 (39.5-41.9) | 7.8 (7.8-135.1) |
| IQ3_S, 2 GPUs, `--prefill 2048`, 32,768-token prompt | 32,768 | 0 | 256 | 3 | 633 (627-634) | 39.7 (31.1-40.4) | 179.9 (177.0-351.4) |
| IQ3_S, 2 GPUs, `--prefill 2048`, 128,000-token prompt | 128,000 | 0 | 256 | 3 | 657 (637-658) | 34.0 (32.1-34.9) | 334.5 (201.2-359.1) |

Total client-side request time (median): 13.6 s at 4K, 145.5 s at 32K, 184.7 s
at 128K with `--prefill auto` (14.2 / 186.2 / 342.4 s with 2048). Speculative
drafts were accepted at 66-79% of offers across the runs. RAM/VRAM use during
inference: not sampled.

## Correctness and limitations

Long-context recall was checked with
[`tools/needle_bench.py`](../../../tools/needle_bench.py) at 32K and 128K
prompt tokens (31,969-31,970 and 123,011-123,013 actual prompt tokens), needle
at 10% / 50% / 90% depth: **all six checks found the code word** (exact answer
each time); see [needles.json](needles.json). Per-request times: 31-102 s at
32K, 178-181 s at 128K, including generation.

Limitations: one quantization (IQ3_S), one workload (synthetic code
explanation, greedy), output capped at 256 tokens, GPU clocks not fixed, and
**an OpenClaw agent session used the same server during the benchmark**, so
client-side TTFT/elapsed show outliers from queueing behind interactive
requests (for example 135 s TTFT on one 4K run whose engine-side prompt time
was 7.7 s). The tok/s columns use the engine's per-request timings and are not
affected by that queueing. The "before" and "after" sets ran at different times
of day on the same machine. These numbers do not establish answer quality
beyond the needle checks or performance on other workloads.
