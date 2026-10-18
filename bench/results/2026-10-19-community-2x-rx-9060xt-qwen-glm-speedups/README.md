# Qwen3.8-Flash-Next and GLM-5.3-Flash on dual RX 9060 XT - speedup verification round (2026-10-19)

Follow-up to this host's two earlier reports, one per model stack on the same machine:

- Qwen: `../2026-10-17-community-2x-unknown-qwen3-8-flash-next-iq3-s/` (full suite baseline)
- GLM: `../../project-maya/2026-10-17-maya-s24-7910-tuned-ctx-sweep/` (tuned-config context sweep baseline)

This note records the two optimizations applied on 2026-10-18/19 and their immediate measurements,
plus a source-code audit of which speed flags the engines already keep on by default on gfx1200
(so they are not double-set by hand).

## Hardware and software

- Dual Xeon (24c/48t each socket, NUMA 2 nodes), 188 GiB RAM, 2x AMD Radeon RX 9060 XT 16 GiB (gfx1200), layer split `gpu:[1,2]`
- Linux (7.0.0-30-generic), ROCm 7.2.4; models on NVMe
- Strata 0.1.45 (qwen3.8-flash-next-iq3_s), project-maya 1.1.15 (Maya-S24 / GLM-5.3-Flash)
- The two stacks are mutually exclusive at load time on this box (each config's `before_load` unloads the other); every number below was measured with one engine loaded

## Qwen: `STRATA_HIP_WMMA=1` (opt-in RDNA4 matrix-core prompt attention)

A/B on this box, same config, only this flag flipped. A ~2.1k-token prompt with a unique
per-run suffix (so the conversation/prompt cache cannot help), `max_tokens=1`, speed from
`usage.prompt_tokens / curl wall time`, 3 runs per arm; the first run of each arm is dropped as
warm-up (the WMMA=0 arm's first run included the engine load: 12.9 tok/s).

| arm | run 2 | run 3 | mean |
| --- | ---: | ---: | ---: |
| WMMA=1 | 347.5 | 347.0 | **347.3 prompt tok/s** |
| WMMA=0 | 329.9 | 329.8 | 329.8 prompt tok/s |

**+5.3%.** The engine's own hint quotes about 30% (measured on an R9700, PR #1318); at this card
and prompt length the gain is +5.3% - small but repeatable to 0.1 tok/s. Kept on. Note the
documented trade-off: the kernel's output differs in the last rounding bits, it is not
bit-identical to the default path.

Propagation check: `STRATA_HIP_WMMA=1` read from the live engine's `/proc/<pid>/environ`. (The
test's log-side hint grep matched a stale pre-restart block; the /proc check is the decisive one.)
Raw output: `wmma-ab-result.txt`.

## GLM: `STRATA_GLM_SPEC_CANCEL=1` + `STRATA_GLM_SPEC_SAMPLE=1`

Added to the Maya config's env; the engine log confirms both: "a wrong draft cancels the head's
speculative pass at its next layer (STRATA_GLM_SPEC_CANCEL)" and "speculative sampling ON
(STRATA_GLM_SPEC_SAMPLE)". Draft acceptance after the restart: 75-100% per request.

No controlled bench yet. Production observation only (server log, per-request decode tok/s from
the agent traffic, so the workloads are not identical): median 18.9 tok/s before (n=17,866 lines,
several days) vs 21.3 tok/s after (n=694, first ~25 min). Treat as indicative; the controlled
baseline for comparison is the 2026-10-17 sweep's 20.1-21.4 tok/s decode band.

## Speed flags this engine already keeps on by default on gfx1200 (do not set these)

Source audit of Strata 0.1.45 / project-maya 1.1.15, so a tuned config does not fight the defaults:

- `STRATA_ROUTE_TAIL_SKIP`: on by default on CUDA and on HIP gfx1200/gfx1201 since 0.1.43, and
  auto-off when the VRAM expert cache holds over the threshold (60% on a HIP layer split) - where
  forcing `=7` measured a net loss (Q2_0 story: -6.2% at 70% cached).
- `STRATA_GFX12_DEFAULTS` (`GDN_HEAD`, `GDN_PP`, `GDN_CONVL2`, `GDN_NOY`, `CVEC_FUSE`): applied
  automatically on gfx12; the startup log prints "gfx12 (RDNA4): 5 exact speed switches on by
  default". `=0` turns them off; setting `=1` is a no-op.
- `STRATA_GLM_CPU_PIN`, `STRATA_GLM_DROP_CACHE`: default on any host with >= 2 NUMA nodes.
- `STRATA_HC_Q8`: single-GPU only (ignored with a layer split); the multi-GPU variant
  `STRATA_HC_REQ8` does not support a layer split yet, so neither applies on this rig.
- The GLM engine accepts a hipBLASLt tuning table only for its own architecture; Maya ships a
  gfx1201 GLM table and gfx1200 runs plain hipBLAS. Measuring a gfx1200 table with
  `tools/hip/tune_hipblaslt` is the untried candidate on the GLM prompt path (the Strata-side
  gfx1200 table `gfx1200-hipblaslt-100202.txt` has been applied and is in this fork).

## Limitations

Quick single-point checks (3 runs), not a suite re-run; the GLM before/after comes from
production logs, not a controlled benchmark; background agent traffic on the box was not
quiescent for the GLM observation. A full suite re-run under the new flags is TODO.