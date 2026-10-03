# Strata (ROCm / デュアル AMD GPU 最適化ビルド)

本リポジトリは、デュアルソケット CPU（QPI/NUMA跨ぎ）およびデュアル AMD Radeon RX 9060 XT（ROCm 7.2.4）環境において、**Qwen3.8-Flash-Next** を最高効率・省電力で動作させるためのインストール手順、チューニングカスタマイズ、および実測ベンチマーク結果をまとめたものです。

---

## 🖥️ 動作検証ハードウェア環境

- **CPU**: Dual Intel Xeon E5-2690 v4 (合計 28コア / 56スレッド、2ソケット NUMA)
- **RAM**: 192 GB DDR4 ECC Reg (96 GB / ソケット)
- **GPU**: Dual AMD Radeon RX 9060 XT 16 GB (`gfx1200`, 各CPUソケット直下のPCIeスロットに配置)
- **OS**: Linux (Ubuntu 24.04 LTS, カーネル 6.8)
- **ROCm**: ROCm 7.2.4 (`hipblaslt`, `hipruntime`)
- **対象モデル**: Qwen3.8-Flash-Next (IQ3_S 量子化、約 84 GB) + MTP ドラフトレイヤー

---

## 📦 インストール手順

### 1. 依存パッケージと ROCm 環境の準備
```bash
sudo apt update
sudo apt install -y build-essential cmake git numactl rocm-dev hipblaslt-dev
```

### 2. リポジトリのビルド (HIP / gfx1200)
```bash
git clone git@github.com:rhinoxy/strata.git
cd strata

# HIP バックエンドビルド
cmake -B build-hip -S . \
  -DSTRATA_BACKEND=hip \
  -DCMAKE_HIP_ARCHITECTURES=gfx1200 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-hip -j$(nproc)

# エンジンバイナリの配置
mkdir -p engine
cp build-hip/strata engine/strata
```

### 3. Python 仮想環境のセットアップ
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### 4. モデル・データファイルの配置
以下の配置構成を想定しています（実データは高速NVMe SSD上に配置）：
- モデル本体 (GGUF): `~/Strata-data/models/IQ3_S/`
- パック (エキスパート & トークナイザ): `~/Strata-data/packs/iq3_s/`
- MTP ドラフトレイヤー: `~/Strata-data/mtp/rt/`
- エキスパートプロファイル: `data/expert-profile.bin`

---

## ⚡ 施したカスタマイズ

### 1. デュアルソケット NUMA インターリーブ (`numactl --interleave=all`)
- **背景**: GPUが別々のCPUソケットに挿入されているため、単一NUMAノードにメモリが偏ると、他方のGPUアクセスがQPIバス（約30GB/s・高レイテンシ）経由となりボトルネックが発生。
- **対策**: 起動時に全NUMAノードへメモリを均等配置する `--interleave=all` を適用。
```bash
# run-iq3_s.sh
exec numactl --interleave=all /home/your_name/Strata/.venv/bin/python /home/your_name/Strata/serve/server.py ...
```

### 2. 投機的サンプリング (MTP) の同期レイテンシ最適化 (PR #508)
- **背景**: 投機ステップ数 (`--spec`) が大きすぎると、QPI跨ぎのGPU間検証通信ラウンドトリップが増加し速度低下を招く。
- **対策**:
  - `--spec`: `"4"` ➔ `"2"` に削減（QPI跨ぎの投機検証往復を抑制）
  - `--spec-min-p`: `"0.5"` ➔ `"0.6"` に引き上げ（採択率の低いトークンの無駄な検証通信をカット）
  - 環境変数 `"HSA_FORCE_FINE_GRAIN_PCIE": "1"` を追加（非P2P/NUMA環境でのROCm PCIe同期の安定化）

### 3. オンデマンド起動 & アイドル時自動VRAM解放（省電力化）
- **背景**: 125B規模のモデルをVRAMに常時保持すると、待機時でもGPU電力が常時消費される。
- **対策**:
  - `"lazy_load": true`: 起動時はHTTPリスナー（メモリ約数十MB）のみ立ち上がり、VRAM使用率は **0%**、GPU待機電力は最小の各 **9W〜10W** に抑制。
  - `"idle_unload_s": 300`: APIリクエスト完了後、5分間アクセスが途絶えると自動でC++エンジンを終了し、VRAMを完全解放。
  - クライアント（OpenClaw等）からリクエストを受信すると、自動的にC++エンジンが起動してモデルをロード（約60秒）。
  - `POST /unload` エンドポイントにより即時VRAM解放も可能。

### 4. PLE Main RAM常駐化 & 大規模プロンプト最適化
- **背景**: Strataのデフォルト（`--ple-io direct`）はSSDから都度ダイレクトI/OでPLEテーブル（27GB）を読み込むため、数万トークンの超長文プロンプト処理時にSSD I/O待ちでCPU/GPUが待機してしまうボトルネックが存在。
- **対策**:
  - `--ple-io ram`: 27GBのPLEテーブルをMain RAMに常駐（mlock/全ページタッチ）させ、SSD I/Oレイテンシを完全排除（メモリアクセス直参照化）。搭載192GB RAMの潤沢なメモリ帯域をフル活用。
  - `--prefill 2048`: チャンクサイズを8192から2048に最適化し、プロンプト処理を平滑化。
  - `"STRATA_WATCHDOG_S": "300"`: ウォッチドッグ判定時間を300秒に延長し、大規模プロンプトの安定処理を実現。

### 5. コンテキスト長の 256K (262,144 tokens) 拡張 & KV Streaming
- **背景**: 長大なドキュメントや対話履歴を処理できるようにコンテキストを最大化。
- **対策**:
  - `--max-context 262144`: Qwen3.8-Flash-Next のネイティブ最大長である 256K に設定。
  - `--kv int8 --kv-resident 32768`: 直近32,768トークンのみをVRAMに保持し、残りはホストRAM（192GB）へストリーミング退避することで、VRAM不足を起こさずに256Kを保持可能。

### 6. OpenClaw 連携設定 (`~/.openclaw/openclaw.json`)
```json
"strata": {
  "baseUrl": "http://127.0.0.1:8081/v1",
  "api": "openai-completions",
  "apiKey": "strata-local",
  "models": [
    {
      "id": "qwen3.8-flash-next-iq3_s",
      "name": "Qwen 3.8 Flash Next (Strata IQ3_S)",
      "reasoning": true,
      "input": ["text"],
      "contextWindow": 262144,
      "contextTokens": 262144,
      "maxTokens": 8192,
      "compat": {
        "supportsUsageInStreaming": true,
        "supportsTools": true,
        "supportsJsonSchemaResponseFormat": true
      }
    }
  ]
}
```

### 7. 常駐サービス化 (`systemd --user`)
マシンの再起動後も自動で軽量待機（VRAM 0%）できるように、ユーザーサービスとして登録：
```bash
# 配置と有効化
mkdir -p ~/.config/systemd/user
cp service/strata.service ~/.config/systemd/user/strata.service
systemctl --user daemon-reload
systemctl --user enable --now strata

# サービス状態確認
systemctl --user status strata
```

---

## 📊 ベンチマーク結果

### 1. 同一ハードウェアにおける設定別パフォーマンス比較

同一プロンプト（`bench/bench_prompts.jsonl`、256トークン生成）における実測結果：

| 測定条件 | 生成速度 (tok/s) | MTP ドラフト採択率 | 待機時消費電力 (GPU1/GPU2) | 待機時 VRAM 使用率 |
| :--- | :---: | :---: | :---: | :---: |
| **① 初期デフォルト** (`--spec 4`, NUMA未調整) | **6.44 tok/s** | 90.9% | 常時ロード時: 約 15〜20W/枚 | 98% / 99% (常時専有) |
| **② NUMAインターリーブのみ** (`--spec 4`, `numactl`) | **10.14 tok/s** (+57.5%) | 90.7% | 常時ロード時: 約 15〜20W/枚 | 98% / 99% (常時専有) |
| **③ 最適化設定** (`--spec 2`, `min-p 0.6`, `fine-grain`, on-demand) | **9.24〜9.70 tok/s** | **94.1%** (+3.2pt) | **9.0W / 10.0W** (アイドル時) | **0% / 0%** (完全解放) |

- **NUMAインターリーブ**: メモリアクセス競合とQPIバスのレイテンシを解消し、生成速度が **6.44 ➔ 10.14 tok/s (約 1.57 倍)** に大幅向上。
- **最適化設定**: `--spec 2 --spec-min-p 0.6` により無駄な投機検証通信を削減し、ドラフト採択率が **94.1%** に向上。QPIバス負荷を抑えつつ、オンデマンド待機により**待機時電力を各GPU 9〜10W（VRAM 0%）に最小化**。

### 2. OpenClaw 連携実測 (大規模コンテキスト処理)

- **入力プロンプト長**: 20,070 トークン (OpenClaw 全システムプロンプト & ツール定義)
- **プレフィル時間**: 479 秒 (チャンクサイズ 2048、平均 100 tok/s、ストールなし)
- **生成速度**: **8.5 〜 9.7 tok/s**
- **動作結果**:
  - オンデマンド自動ロード ➔ 20,070 トークンの安定プレフィル ➔ 正常応答完了 ➔ アイドル5分後に VRAM 0% へ自動復帰を確認。
