# Maya-S24 (GLM-5.3-flash) context-length sweep on the 7910 host - 2026-10-15

Dual RX 9060 XT (gfx1200, `gpu:[1,2]`, 16 GB each), Maya-S24 pack, `--max-context 524288`,
`--expert-cache auto`, `--prefill auto`, `--kv-streaming on`, MTP draft enabled. Fresh prompts only
(unique nonce, `reused = 0`), greedy (`temperature 0`), 512 output tokens per request on a
story-continuation task; every request ended on `length`, i.e. the full 512 tokens were generated.
Measured through the engine's own `/metrics` request records plus the log's
`glm spec: A of B drafts accepted` lines; script: `bench.py` (also in this directory).

## Sweep

| context | prompt tok | prompt raw tok/s | marginal tok/s (minus the ~5 s per-request overhead) | prompt ms | decode tok/s | MTP drafts accepted | VRAM expert hit |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ~2.2k | 2,184 | 229 | ~490 | 9.5 s | 19.7 | 71.6% (365/510) | 0.708 |
| ~8.2k | 8,231 | 396 | ~520 | 20.8 s | 19.7 | 67.3% (343/510) | 0.706 |
| ~32.4k | 32,422 | 446 | ~480 | 72.7 s | 19.9 | 69.6% (355/510) | 0.705 |
| ~129k (run 1) | 129,192 | 489 | ~500 | 264 s | 17.8 | 70.8% (361/510) | 0.713 |
| ~130k (run 2, clean) | 129,697 | 439 | ~450 | 295 s | 17.1 | 65.3% (333/510) | ~0.71 |

- `prompt_ms` carries a fixed ~5.0-5.6 s per-request overhead (three 275-token calibration requests:
  5.08 / 5.15 / 5.58 s), so the raw rate understates short prompts; the marginal rate
  (tokens / (prompt_ms - 5.1 s)) is roughly constant at ~440-520 tok/s at every length.
- Decode holds 19.7-19.9 tok/s up to 32k and drops ~13% at 128k (17.1-17.8 tok/s).
- MTP acceptance on this task is flat at 65-72%; live chat traffic in the log ranges 63-100%
  (copy-like continuations reach 97-100%).
- Run 1's 128k request had one foreign 69k-token request from another session partly overlapping
  it; run 2 re-measured the same point under a quiet server (that is the last row).

## Idle power and VRAM (engine loaded, server state idle)

| metric | GPU1 (card0, RX 9060 XT) | GPU2 (card2, RX 9060 XT) |
| --- | ---: | ---: |
| idle power | 11.3 -> 11.2 W | 9.3 -> 9.1 W |
| idle VRAM used | 13.92 -> 13.52 GB / 16.30 GB (~85%) | 13.78 -> 13.44 GB / 16.30 GB (~84%) |

60 s averages (2 s x 30 samples) before and after the sweep, via `amd-smi metric -p` socket_power
and sysfs `mem_info_vram_used` (Maya's `/metrics` GPU series are all null on this host, so the
monitor path cannot supply them). The RX 6400 (not used by Maya) idled at 6 W / 0.95 GB.

## Files

- `bench.py` - the sweep script (`MAYA_TARGETS`, `MAYA_IDLE_ITERS`, `MAYA_OUT` env overrides)
- `results.json`, `idle_before.json`, `idle_after.json` - run 1
- `rerun_results.json` - clean 128k re-measurement

