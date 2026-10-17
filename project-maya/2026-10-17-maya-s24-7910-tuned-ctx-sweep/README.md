# Maya-S24 (GLM-5.3-flash) context-length sweep, tuned config, on the 7910 host - 2026-10-17

Re-run of the 2026-10-15 sweep on the **tuned** engine configuration: `--prefill 4096 --kv int8
--kv-streaming on`, `/dev/shm` slot dir (`STRATA_GLM_SLOT_DIR`), `STRATA_GLM_RESERVE_MB=512`,
`STRATA_GLM_PREFILL_SUB=1024` (dual RX 9060 XT, gfx1200, `gpu:[1,2]`, 16 GB each). The 2026-10-15
run used the pre-tuning config (`--prefill auto`, no int8 KV, reserve 3072), so its numbers are not
directly comparable; this directory measures the same fresh-prompt sweep on the tuned state.

Fresh prompts only (unique nonce, `reused = 0`), greedy (`temperature 0`), 512 output tokens per
request on a story-continuation task; every request ended on `length`. Measured through the
engine's own `/metrics` request records plus the log's `glm spec: A of B drafts accepted` lines.
Script: `bench.py` (identical to the 2026-10-15 one; run with `MAYA_OUT=... MAYA_TARGETS=2048,8192,32768,131072`).

## Sweep

| context | prompt tok | prompt raw tok/s | marginal tok/s (minus the ~5.4 s per-request overhead) | prompt ms | decode tok/s | MTP drafts accepted | VRAM expert hit |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ~2.2k | 2,185 | 247 | ~630 | 8.86 s | 20.2 | 62.4% (318/510) | 0.739 |
| ~8.3k | 8,316 | 426 | ~590 | 19.53 s | 21.4 | 65.7% (335/510) | 0.770 |
| ~32.6k | 32,592 | 546 | ~600 | 59.65 s | 21.0 | 70.3% (376/535) | 0.753 |
| ~130k | 129,696 | 572 | ~586 | 226.84 s | 20.1 | 69.3% (371/535) | 0.766 |

- `prompt_ms` carries a fixed ~5.4 s per-request overhead (275-token calibration request:
  5.42 s; the 2026-10-15 calibration matched at 5.0-5.6 s), so the raw rate understates short
  prompts; the marginal rate (tokens / (prompt_ms - 5.4 s)) is roughly constant at ~586-630 tok/s
  at every length - an improvement over the pre-tuning ~440-520 tok/s.
- Decode is flat at 20.1-21.4 tok/s from 2k to 128k. The pre-tuning run dropped ~13% at 128k
  (19.7 -> 17.1-17.8 tok/s); the tuned config removes that long-context decode penalty.
- MTP acceptance on this task is 62-70%.
- The gateway session that serves this agent runs on this same server and was parked during the
  sweep (its ~15.5k-token conversation stayed cached in slot 1); no foreign requests overlapped
  the measured points.

## Idle power and VRAM (engine loaded, server state idle)

| metric | GPU1 (card0, RX 9060 XT) | GPU2 (card2, RX 9060 XT) |
| --- | ---: | ---: |
| idle power (before -> after) | 25.9 -> 20.0 W | 24.8 -> 24.1 W |
| idle VRAM used | 16.13 -> 16.09 GB / 16.30 GB (~99%) | 15.99 -> 15.99 GB / 16.30 GB (~98%) |

60 s averages (2 s x 30 samples) before and after the sweep, via `amd-smi metric -p` socket_power
and sysfs `mem_info_vram_used` (Maya's `/metrics` GPU series are all null on this host). Unlike the
2026-10-15 idle readings (11.3/9.3 W, ~85% VRAM), the tuned config keeps KV int8 + resident state
that fills VRAM nearly to the top, and idle power sits ~20-26 W rather than ~10 W; the parked
gateway session is included in this server's idle state.

## Files

- `bench.py` - the sweep script (`MAYA_TARGETS`, `MAYA_IDLE_ITERS`, `MAYA_OUT` env overrides)
- `results.json`, `idle_before.json`, `idle_after.json` - this run
