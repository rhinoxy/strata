# Project Maya (GLM-5.3-Flash) Dual-GPU Benchmark & Tuning Report

このドキュメントでは、**Project Maya (GLM-5.3-Flash / Maya-S24)** を Dual AMD Radeon RX 9060 XT (16GB × 2 = 32GB VRAM) および 188.8GB RAM (Dual Xeon NUMA) 環境において、**最大コンテキスト 512K (`524,288` tokens)** で運用した際の実測ベンチマーク結果と最適化チューニングの詳細を記録します。

---

## 🖥️ 測定環境スペック

- **OS**: Linux 7.0 (Ubuntu based x86_64)
- **CPU**: Dual Intel Xeon CPU E5-2687W v4 @ 3.00GHz (計 24 コア / 48 スレッド, Broadwell, PCIe 3.0)
- **RAM**: 188.8 GB (Dual-Socket NUMA Node 0/1 インターリーブ配置)
- **GPU**: 2 × AMD Radeon RX 9060 XT (各 16GB VRAM, RDNA4 / gfx1200)
  - `CUDA0`: GPU 1 (Layers 0〜23 担当, NUMA node 0 結合)
  - `CUDA1`: GPU 2 (Layers 24〜44 + NextN MTP 担当, NUMA node 1 結合)
- **Model**: GLM-5.3-Flash (Maya-S24 / IQ2_XXS_S / 12,096 Experts)
- **Context Length**: **524,288 tokens (512K)**
- **Runtime**: Project Maya 1.1.11 (HIP / ROCm backend)

---

## 📊 最適化チューニング前後の比較実測値

| メトリクス | 最適化前 (初期設定) | 最適化後 (今回チューニング) | 改善効果 |
| :--- | :---: | :---: | :--- |
| **コンテキスト長** | 524,288 (512K) | **524,288 (512K)** | **512K 完全維持** |
| **VRAM リザーブ** (`STRATA_GLM_RESERVE_MB`) | `3072` (計 6GB 未使用) | **`512`** | 各カードの死蔵 VRAM 2.5GB を解放 |
| **GPU Expert Pool 容量** | 9.21 GB / カード | **11.80 GB / 11.77 GB** | **+5.15 GB (+28%) 拡大** |
| **VRAM 常駐 Expert スロット数** | 2,763 スロット | **3,537 スロット** | **+774 スロット増加** |
| **VRAM キャッシュヒット率** | 51.5% 〜 57.4% | **61.7% 〜 73.8%** | **約 +15% 向上** |
| **RAM からの Expert フェッチ回数** | 64 〜 76 回/token | **32 〜 50 回/token** | **PCIe 転送負荷が半減** |
| **KV キャッシュサイズ (512K 時)** | 3.22 GB (FP16) | **1.71 GB (INT8)** | **容量・転送帯域が約 47% 圧縮** |
| **会話スロット保存先** (`STRATA_GLM_SLOT_DIR`) | `<pack>/slots` (HDD) | **`/dev/shm/maya-slots` (RAM)** | 遅い HDD から RAM ディスクへ移動 |
| **8万〜9万 token スロット保存時間** | 8.0 〜 9.9 秒 (I/O ストール) | **0.7 〜 0.8 秒** | **約 1/12 に短縮 (フリーズ完全解消)** |
| **Prefill チャンク / サブバッチ** | `auto` (8192) / `256` | **`4096` / `1024`** | GPU 並列度最大化 ＆ 逆量子化激減 |
| **Decode 推論速度** | 15.6 〜 16.9 tok/s | **20.8 〜 22.4 tok/s** | **+25〜35% 高速化** |

---

## 🔍 主要なボトルネックと施した対策

### 1. 会話スロットキャッシュのディスク I/O ストール解消
- **現象**: 6万〜18万トークン超の会話時、1スロットあたり 1.0GB〜2.76GB のキャッシュが自動退避（`glm slots: saved`）されます。デフォルトでは HDD 上の `<pack>/slots` に書き込まれるため、毎ターン 8〜10 秒間のディスク I/O 待ちフリーズが発生していました。
- **対策**: 環境変数 `"STRATA_GLM_SLOT_DIR": "/dev/shm/maya-slots"` を指定し、Linux の tmpfs (RAM ディスク) に退避。
- **結果**: 8万〜9万トークンのスロット退避・復元が **0.7〜0.8 秒（実質ゼロレイテンシ）** で完了するようになりました。

### 2. VRAM 死蔵領域の解放と Expert Pool の最大化
- **現象**: `"STRATA_GLM_RESERVE_MB": "3072"` により、各 GPU で 3GB（計 6GB）の VRAM が未使用のまま空けられていました。
- **対策**: リザーブを `"512"` に引き下げ、残りの VRAM をすべて Expert Pool に割り当て。
- **結果**: Expert Pool が 9.21GB → **11.80GB / 11.77GB** へ拡大。VRAM 常駐スロットが **3,537 スロット** に増加し、VRAM ヒット率が最大 **73.8%** まで向上しました。

### 3. KV キャッシュの INT8 量子化
- **現象**: 512K コンテキストにおいて、FP16 の KV キャッシュは 3.22 GB を消費し、PCIe 3.0 の転送帯域を圧迫していました。
- **対策**: `"--kv", "int8"` を追加。
- **結果**: 1トークンあたり 1024 bytes → 544 bytes に縮小。KV キャッシュ全体が **1.71 GB に半減** し、超長文 Prefill 時のバス帯域ボトルネックが大幅に緩和されました。

### 4. Prefill 演算スループットの適正化
- **現象**: AMD ROCm 環境における Prefill サブバッチサイズ（`STRATA_GLM_PREFILL_SUB`）のデフォルトが 256 に制限されており、GEMM の演算コアが余っていました。
- **対策**: `"--prefill", "4096"` および `"STRATA_GLM_PREFILL_SUB": "1024"` を設定。
- **結果**: GPU Tensor コア（WMMA）の並列計算効率が最大化され、重みの逆量子化回数が 1/4 に削減されました。

---

## ⚙️ 最適化済み設定ファイル例 (`maya-maya-s24-hip.json`)

```json
{
 "exe": "/path/to/project-maya/build-hip/strata",
 "args": [
  "--glm-pack", "/path/to/models/Maya-S24/pack",
  "--max-context", "524288",
  "--expert-cache", "auto",
  "--prefill", "4096",
  "--kv", "int8",
  "--kv-streaming", "on",
  "--expert-profile", "/path/to/project-maya/data/expert-profile-glm.bin"
 ],
 "cwd": "/path/to/project-maya",
 "tokenizer": "/path/to/models/Maya-S24/pack/tokenizer",
 "model_name": "glm-5.3-flash",
 "lazy_load": true,
 "before_load": "/path/to/Strata/tools/ensure_exclusive.sh strata",
 "model_aliases": [
  "glm-5.3-flash",
  "maya/glm-5.3-flash",
  "glm5.3-flash",
  "Maya-S24"
 ],
 "gpu": [1, 2],
 "sampling": {
  "temperature": 1.0,
  "top_p": 0.95
 },
 "reasoning_effort": "high",
 "lib_dirs": [
  "/opt/rocm/lib"
 ],
 "port": 8082,
 "host": "0.0.0.0",
 "api_key": "<your-strata-api-key>",
 "backend": "hip",
 "env": {
  "STRATA_GLM_RESERVE_MB": "512",
  "STRATA_GLM_RAM_HEADROOM_GB": "16",
  "STRATA_GLM_RAM_RESIDENT": "1",
  "STRATA_GLM_SLOT_DIR": "/dev/shm/maya-slots",
  "STRATA_GLM_PREFILL_SUB": "1024"
 }
}
```
