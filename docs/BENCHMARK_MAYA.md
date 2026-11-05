# Project Maya Multi-Model Dual-GPU Benchmark & Architecture Report

このドキュメントでは、**Project Maya (v1.1.53)** を Dual AMD Radeon RX 9060 XT (16GB × 2 = 32GB VRAM) および 384GB RAM (Dual Xeon NUMA) 環境において、**GLM-5.3-Flash** および **DeepSeek-V4.1-Flash** をオンデマンド排他運用した際の実測ベンチマーク結果と最適化チューニングの詳細を記録します。

---

## 🖥️ 測定環境スペック

- **OS**: Linux 7.0 (Ubuntu based x86_64)
- **CPU**: Dual Intel Xeon CPU E5-2687W v4 @ 3.00GHz (計 24 コア / 48 スレッド, Broadwell, PCIe 3.0)
- **RAM**: **384 GB (377 GiB 実効)** (Dual-Socket NUMA Node 0/1 インターリーブ配置)
- **GPU**: 
  - `CUDA0`: AMD Radeon RX 9060 XT (16GB VRAM, RDNA4 / gfx1200, 16GB Full BAR 有効)
  - `CUDA1`: AMD Radeon RX 9060 XT (16GB VRAM, RDNA4 / gfx1200, 16GB Full BAR 有効)
  - 表示用: AMD Radeon RX 6400 (4GB VRAM)
- **PCIe / MMIO**: BIOS にて Memory Mapped I/O Above 4GB 有効化（Resizable BAR 16GB 開放）
- **Runtime**: Project Maya **v1.1.53** (ROCm 7.2.4 HIP backend)

---

## 🚀 マルチモデル・オンデマンド提供構成

各モデルは systemd ユーザーサービスによりオンデマンド（`lazy_load: true`）で待機し、初回リクエスト受信時に排他スクリプト（`tools/ensure_exclusive.sh`）経由で相互アンロードとメモリ調整を行って起動します。

| ポート | サービス名 | モデル | コンテキスト長 | 主要アーキテクチャ & 特徴 |
| :---: | :--- | :--- | :---: | :--- |
| **8082** | `maya.service` | GLM-5.3-Flash (Maya-S24) | 512K (524,288) | GPU Layer-Split (24/21), NextN MTP, INT8 KV |
| **8083** | `maya-ds4.service` | DeepSeek-V4.1-Flash (Maya-S) | 1024K (1,048,576) | **完全インメモリ (Large-RAM)**, TP=2, DSpark ドラフター |
| **8085** | `maya-m-derisked.service` | GLM-5.3-Flash (Maya-M-Derisked) | 1024K (1,048,576) | Derisked 量子化, Vision 画像入力対応 |
| **8086** | `maya-l.service` | GLM-5.3-Flash (Maya-L) | 1024K (1,048,576) | 最上位 Tier, **Vision (画像理解) 統合**, INT8 KV |

---

## 📊 モデル別ベンチマーク実測値

### 1. DeepSeek-V4.1-Flash (`maya-ds4`) Large-RAM 完全インメモリ運用

384GB RAM の大容量を活かし、SSD へのフォールバックをゼロにして全 Engram テーブルおよび全 CPU-lane Expert を DRAM にロック常駐させた結果：

* **設定パラメータ**:
  * `"--mmap-experts", "off"`
  * `"DS4_ENGRAM_RAM": "1"`
  * `"DS4_REQUIRE_ALL_RAM": "1"`
* **常駐ログ確認**:
  * `ds4: Engram RAM ONLY: 2 complete tables (Q6_K/Q5_K/IQ4_NL/FP8), 161.285 GB locked; reads use memcpy`
  * `ds4: strict RAM: all 14451 CPU experts selected, no disk fallback`
  * `ds4: 14451 of the CPU lane's 14451 experts copied into RAM (159.1 GB, 2 NUMA parts)`
* **実効メモリ消費**:
  * 物理 RAM 使用量: 約 **321 GB**（スワップ使用ゼロ、空き RAM 約 50 GB を維持）
* **推論パフォーマンス**:
  * **Decode 生成速度**: **14.7 〜 18.3 tokens/sec**
  * **DSpark ドラフト承認率**: **93.8%**（SSD I/O ストール完全解消）

---

### 2. GLM-5.3-Flash (`Maya-S24` / `Maya-L`) 512K〜1M コンテキスト運用

Dual RX 9060 XT にレイヤーを 24 / 21 で分割配置し、KV キャッシュ INT8 量子化と tmpfs スロット保存を組み合わせたチューニング成果：

| メトリクス | 最適化前 (初期設定) | 最適化後 (チューニング済み) | 改善効果 |
| :--- | :---: | :---: | :--- |
| **コンテキスト長** | 524,288 (512K) | **524,288 〜 1,048,576 (1M)** | **超長文コンテキストを完全維持** |
| **VRAM リザーブ** (`STRATA_GLM_RESERVE_MB`) | `3072` (計 6GB 未使用) | **`512`** | 各カードの死蔵 VRAM 2.5GB を解放 |
| **GPU Expert Pool 容量** | 9.21 GB / カード | **11.80 GB / 11.77 GB** | **+5.15 GB (+28%) 拡大** |
| **VRAM 常駐 Expert スロット数** | 2,763 スロット | **3,537 スロット** | **+774 スロット増加** |
| **VRAM キャッシュヒット率** | 51.5% 〜 57.4% | **61.7% 〜 73.8%** | **約 +15% 向上** |
| **RAM からの Expert フェッチ回数** | 64 〜 76 回/token | **32 〜 50 回/token** | **PCIe 転送負荷が半減** |
| **KV キャッシュサイズ (512K 時)** | 3.22 GB (FP16) | **1.71 GB (INT8)** | **容量・転送帯域が約 47% 圧縮** |
| **会話スロット保存先** (`STRATA_GLM_SLOT_DIR`) | `<pack>/slots` (HDD) | **`/dev/shm/maya-slots` (RAM)** | 遅いディスクから RAM ディスクへ移動 |
| **8万〜9万 token スロット保存時間** | 8.0 〜 9.9 秒 (I/O ストール) | **0.7 〜 0.8 秒** | **約 1/12 に短縮 (フリーズ完全解消)** |
| **Prefill チャンク / サブバッチ** | `auto` (8192) / `256` | **`4096` / `1024`** | GPU 並列度最大化 ＆ 逆量子化激減 |
| **Decode 推論速度** | 15.6 〜 16.9 tok/s | **20.8 〜 22.4 tok/s** | **+25〜35% 高速化** |

---

### 3. GLM-5.3 Multimodal (Vision) 画像理解の実装

GLM-5.3-Flash Maya-L (:8086) および Maya-M-Derisked (:8085) において、CPU 版 Vision ヘルパーを連携させ、画像入力（Base64 URL）に対応：

* **Vision ヘルパー構成**:
  * バイナリ: `tools/vision` を CPU 版としてビルドした `maya-vision`
  * モデル: `mmproj-GLM-5.3-Flash-F16.gguf` (1.13 GB) + `GLM-5.3-Flash-vocab.gguf` (9.4 MB)
* **実測テスト**:
  * 画像認識レイテンシ: 初回認識約 600 ms、以降 Decode 16.2 tok/s で正確な画像認識・回答を出力。

---

## 🛠️ ハードウェア固有の制約と解決策

### 1. Above 4G MMIO 有効化時の AMDKFD 競合回避 (`libkfd_ignore_rx6400.so`)
- **課題**: BIOS で Above 4GB MMIO を有効化（16GB Full BAR 開放）した際、画面表示用 GPU (RX 6400, 4GB VRAM) の BAR も拡大される。ROCm カーネルドライバ (KFD) はシステムメモリをピン留めする際、全 AMD GPU のページテーブルにマッピングを作成しようとするため、4GB を超える巨大メモリ（70GB〜160GB）の確保時に RX 6400 側で `failed to validate BO` となり全 Expert 常駐がクラッシュする。
- **解決策**: `tools/hip/kfd_ignore_rx6400.c`（Shim ライブラリ）を作成。KFD IOCTL `AMDKFD_IOC_ALLOC_MEMORY_OF_GPU` において RX 6400 宛ての巨大ピン留め要求を計算用 GPU へリダイレクトすることで、Full BAR の広帯域を享受しつつ 160GB 超のシステムメモリピン留めを完全両立。

### 2. HugePages の動的制御
- **課題**: Strata (Qwen3.8 等) は 2 MiB HugePages (約 24,000 ページ = 48GB) をホスト Expert アリーナとして要求するのに対し、Project Maya (GLM / DS4) は通常のピン留めメモリ (`mmap` + `mlock`) を使用するため、HugePages が予約されたままだと使用可能ホスト RAM が圧迫される。
- **解決策**: `tools/ensure_exclusive.sh` 内で呼び出し元モデルに応じ、Strata 起動時は `nr_hugepages=24000`、Maya 起動時は `nr_hugepages=0` を自動設定してホスト RAM を 100% 解放。
