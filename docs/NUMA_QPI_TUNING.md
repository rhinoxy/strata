# Multi-Socket / NUMA and Cross-Socket GPU Tuning

This guide documents performance optimization strategies for running Strata on multi-socket workstation/server hardware (e.g. dual Intel Xeon, dual AMD EPYC) where GPUs are distributed across PCIe slots belonging to different CPU sockets (crossing QPI/UPI or Infinity Fabric).

---

## 1. NUMA Memory Interleaving (`--interleave=all`)

### Problem
In multi-socket systems, Linux defaults to the "first-touch" local memory allocation policy. When Strata loads the model's expert arena (~40–55 GB into host RAM), the memory pages are allocated predominantly on the NUMA node where the server process initializes.
When two or more GPUs are split across sockets:
- The GPU on the remote socket must route every expert memory read through the inter-socket bus (Intel QPI/UPI or AMD Infinity Fabric).
- This creates severe bus saturation, memory latency doubling (e.g. NUMA distance 21 vs 10), and uneven memory channel utilization.

### Solution
Launch the Strata server with `numactl --interleave=all`:
```bash
numactl --interleave=all /path/to/.venv/bin/python serve/server.py --engine strata ...
```
This distributes allocated pages round-robin across all available NUMA memory nodes, engaging all memory channels (e.g. DDR4 4ch × 2 sockets = 8 channels) and eliminating remote-access asymmetry.

### Measured Impact (Dual Intel Xeon E5-2687W v4 + Dual AMD RX 9060 XT 16GB)
- **Model**: Qwen3.8-Flash-Next `IQ3_S` (125B MoE)
- **Topology**: GPU 1 on Socket 0 (NUMA Node 0), GPU 2 on Socket 1 (NUMA Node 1), QPI interconnect (NUMA distance 21).

| Configuration | Decode Speed | Wall Time (120 tok) | Draft Acceptance | Host RAM Load Bandwidth |
|---|---|---|---|---|
| **1. Default (no NUMA, spec 4, min-p 0.5)** | **6.4 tok/s** | 32.96 s | 79.3% (65/82) | 1.48 GiB/s (82 s) |
| **2. NUMA Interleaving only (`--interleave=all`)** | **10.1 tok/s** | **19.84 s (-40%)** | 83.5% (66/79) | **9.94 GiB/s (6.7x)** |
| **3. Full Optimization (NUMA + spec 2 + min-p 0.6 + HSA)** | **9.2–9.5 tok/s** | 20.61 s | **94.1% (48/51)** | **10.31 GiB/s** |

*Note: While Case 2 achieves peak burst decode throughput on simple Japanese text, Case 3 significantly reduces speculative rollbacks and GPU verification chatter across QPI, yielding a 94.1% draft acceptance rate and predictable latency on complex reasoning workloads.*

---

## 2. Speculative Decoding (MTP) over High-Latency Interconnects

### Observations
Strata uses Multi-Token Prediction (MTP) draft execution (`--spec 4 --spec-min-p 0.5` by default) to accelerate decode.
On multi-socket configurations where GPU-to-GPU handoffs must cross QPI/UPI:
- Inter-socket synchronization latency adds per-verification overhead.
- If draft acceptance rate dips, the synchronization round-trip can diminish or negate the speculative benefit.

### Recommendations
For multi-socket cross-QPI/UPI GPU setups:
- Test lower `--spec` values (e.g. `--spec 2`) to shorten the verification loop.
- Increase confidence threshold: `--spec-min-p 0.6` or `0.7` to only speculatively accept high-probability tokens.
- For maximum per-token deterministic latency, consider `--spec 0` (disabling MTP verification handoffs).

---

## 3. Why Pipeline Parallelism (`layer_split`) Outperforms Tensor Parallelism on PCIe/QPI

Unlike tensor-parallel frameworks (e.g. vLLM with `TP=2`) which require multiple All-Reduce collectives per layer (hundreds per token), Strata uses **Layer-Split Pipeline Parallelism**:
- Tokens pass across GPUs only once per verification window (a few hundred KB via host pinned RAM).
- This architecture avoids PCIe bandwidth saturation and deadlocks over QPI links, making it the ideal multi-GPU execution model for consumer GPUs in workstation motherboards.
