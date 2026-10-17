# Community benchmark on AMD Radeon + AMD Radeon

Measured on 2026-10-17 with `tools/strata_bench.py` (suite 1, 3 runs per test, medians with ranges). Speeds are the engine's own timings; first-token times are measured by the client over HTTP. Speed only: no correctness check was run.

## Hardware and software

- GPU and VRAM: AMD Radeon + AMD Radeon, 32 GiB VRAM
- CPU: Intel(R) Xeon(R) CPU E5-2687W v4 @ 3.00GHz (24 cores, 48 threads); installed RAM: 188.8 GiB; storage: not measured
- OS: Linux 7.0.0-30-generic; GPU driver: 6.19.4; CUDA: not read
- Strata: project 0.1.43.1, commit abe114f6; engine 0.1.43.1
- Background workloads and power limits: not recorded

## Model and configuration

- Model and quantization: qwen3.8-flash-next-iq3_s
- Context: 262144 tokens; parallel slots: 2
- Settings: the install's own configuration, unchanged; thinking off for the suite; temperature 0

## Method

Run by `python tools/strata_bench.py --long` against the HTTP API of a Strata server (already running); model loading is not timed. One warm-up chat and a 1K prompt are discarded first. Every prompt starts with a unique tag, so the conversation cache cannot help the fresh-prompt tests.

- short chat: the three questions of `tools/ab_engine.py`, up to 100 new tokens, 3 runs
- 4K / 16K prompts: generated word lists sized by a calibration request, 24 new tokens, 3 fresh prompts each
- agent session: system prompt and four tools, then 5 turns that each add about 1500 words of a project file and continue from the model's own earlier answers (the way agent clients resend a conversation), 64 new tokens per turn, 3 sessions; the cache numbers come from the answer's usage (`cached_tokens`) and timings (`prompt_n`)
- concurrency (when run): N requests sent at the same moment, total tokens over the wall time of the burst, latency over all requests

## Results

| Configuration | Actual prompt tokens | Reused tokens | Generated tokens | Runs | Prompt tok/s median and range | Decode tok/s median and range | TTFT seconds median and range |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| short chat | 46 (43-51) | 0 | 100 (100-100) | 9 | not measured | 37.8 (25.1-45.2) | 0.80 (0.67-1.49) |
| 4K prompt, fresh | 4019 (4019-4019) | 0 (0-0) | 24 cap | 3 | 658.9 (658.2-661.2) | not measured | 6.15 (6.12-6.15) |
| 16K prompt, fresh | 16058 (16058-16058) | 0 (0-0) | 24 cap | 3 | 877.6 (872.9-882.4) | not measured | 18.35 (18.26-18.46) |
| 32K prompt, fresh | 32049 (32049-32049) | 0 (0-0) | 24 cap | 3 | 1113.4 (1112.0-1115.0) | not measured | 28.86 (28.82-28.90) |
| agent session, follow-up turns | 15663 (15656-15675) at the last turn | see below | up to 64 | 3 | see below | 38.9 (11.0-51.2) | 5.70 (4.54-7.12) |

Ranges are the lowest and highest of the runs; `Runs` counts requests that returned a token.

### Agent session (conversation cache)

Cache hit over the follow-up turns: 70.1 % (70.1-70.1); earlier-turn tokens read again per session: 3 (3-5) (earlier turns that the cache did not hold; the 14851 (14850-14851) new tokens of the files are read once); first turn (nothing cached) first token: 3.77 s (3.66-3.81).

| Turn (run 1) | Prompt tokens | Reused | Read | Prompt tok/s | First token (s) | Decode tok/s |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 609 | 0 | 609 | 168.5 | 3.658 | 31.2 |
| 1 | 4929 | 632 | 4297 | 653.7 | 6.648 | 37.3 |
| 2 | 6462 | 4993 | 1469 | 327.2 | 4.542 | 11.0 |
| 3 | 9906 | 6525 | 3381 | 507.9 | 6.715 | 44.2 |
| 4 | 12792 | 9932 | 2860 | 507.6 | 5.697 | 44.6 |
| 5 | 15656 | 12810 | 2846 | 502.8 | 5.725 | 30.3 |

Per-request data: `results.json`. Engine log lines from the start: `engine-log-excerpt.txt` (when this run started the server).

## Correctness and limitations

Not measured: this is a speed suite. The conversation uses the model's own replies, so token counts differ a little between runs. Machine details come from the server, `nvidia-smi`/`rocm-smi` and the OS; background load and power limits are not recorded. Total time of the suite: 349 s.
