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
以下の配置構成です（実データはHDD `/dev/sda2` 上に配置）：
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

### 4. PLE Main RAM常駐化 & HDDランダムシークボトルネックの完全排除
- **背景**: モデルファイルはHDD（`/dev/sda2`, ST2000DM001）上に配置されています。Strataのデフォルト（`--ple-io direct`）はディスクから都度ダイレクトI/OでPLEテーブル（27GB）を読み込むため、HDDの物理的な磁気ヘッドのシーク待ち（10〜15ms/回、約75〜100 IOPS）が大量に発生。数万トークンの超長文プロンプト処理時にHDDシーク待ちで15分以上CPU/GPUが完全待機（アイドル）してしまう深刻なボトルネックが発生していました。
- **対策**:
  - `--ple-io ram`: 27GBのPLEテーブルをMain RAMに常駐（起動時にシーケンシャル読込してmlock/全ページタッチ）させ、HDDの物理ランダムアクセスを完全排除（メモリアクセス直参照化）。搭載192GB RAMの潤沢なメモリ帯域をフル活用。
  - `--prefill auto`: チャンクサイズをエンジンがVRAM状況から自動選択（この構成では8192を選択）。2048固定比で32Kプロンプト約+68%、128Kで約+79%（[ベンチマーク](bench/results/2026-10-04-amd-dual-9060xt-tuning/README.md)）。
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
  "apiKey": "<your-api-key>",
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
| **③ 投機・PCIe最適化** (`--spec 2`, `min-p 0.6`, `fine-grain`) | **9.24〜9.70 tok/s** | **94.1%** (+3.2pt) | **9.0W / 10.0W** (アイドル時) | **0% / 0%** (完全解放) |
| **④ 現在構成** (`--ple-io ram`, 256K, int8 KV, on-demand) | **34.6〜38.1 tok/s** (初期比 **5.9倍**) | **92.3〜98.0%** | **9.0W / 10.0W** (アイドル時) | **0% / 0%** (完全解放) |

- **NUMAインターリーブ**: メモリアクセス競合とQPIバスのレイテンシを解消し、生成速度が **6.44 ➔ 10.14 tok/s (約 1.57 倍)** に向上。
- **投機・PCIe最適化**: `--spec 2 --spec-min-p 0.6` により無駄な投機検証通信を削減し、ドラフト採択率が **94.1%** に向上。QPIバス負荷を抑制。
- **PLE RAM常駐化 (`--ple-io ram`)**: 27GBのPLEテーブルをMain RAMに常駐させることで、PLEテーブル行参照に伴うHDD物理シーク待ち（約75〜100 IOPS）を完全解消。生成速度が **34.6〜38.1 tok/s (約 3.8 倍)** へ跳ね上がり、大規模Prefillも超高速化。
- **オンデマンド省電力**: アイドル5分でVRAMを完全解放（0%）、**待機時電力を各GPU 9〜10Wに最小化**。

### 2. プレフィル（Prefill）速度の比較 (超長文プロンプト)

PLEテーブルをHDDから直接読むデフォルト（`--ple-io direct`）と、Main RAMに常駐させた現在構成（`--ple-io ram`）の実測比較：

| プロンプト長 | `--ple-io direct` (HDD ダイレクト) | 現在構成: `--ple-io ram` (Main RAM常駐) | 速度向上比 |
| :--- | :---: | :---: | :---: |
| **約 2.3K トークン** (2,291 tokens) | 約 25〜35 秒 | **6.0 秒** (379.1 tok/s) | **約 4〜5 倍** |
| **約 20K トークン** (20,070 tokens) | 479 秒 (約8分) | **約 12 秒** (~1,600 tok/s) | **約 40 倍** |
| **約 68K トークン** (68,231 tokens) | 15分以上 (タイムアウト寸前) | **32 秒** (~2,130 tok/s) | **約 30 倍以上** |

### 3. OpenClaw 実稼働ベンチマーク (68Kトークン処理)

- **入力プロンプト長**: 68,231 トークン (OpenClaw 超長文システムプロンプト & 履歴 & ツール定義)
- **プレフィル時間**: **32 秒** (HDDシーク待ちゼロ、CPU/GPUフル稼働)
- **生成速度 (Decode)**: **35.2 〜 37.5 tok/s** (ツール呼び出し・推論含む)
- **Expert Cache ヒット率**: **92.4% 〜 93.4%**
- **動作フロー**:
  - OpenClawからのリクエスト受信 ➔ C++エンジン自動起動＆27GB PLEテーブルRAMロード (約60秒) ➔ 68Kトークン即座にプレフィル (32秒) ➔ 高速生成 (35〜37 tok/s) ➔ 応答完了後5分で自動アンロード (VRAM 0%、GPU電力 9W/10W)。


---

## 🆕 2026-10-04 更新: OSレベル修正3件と `--prefill auto`

上記の構成に、OSレベルの修正3件を追加した。詳細・全データは
[ベンチマークレポート](bench/results/2026-10-04-amd-dual-9060xt-tuning/README.md)（コミュニティ形式、before/after各9ラン + リコール検査）。

### 8. PLEテーブルのmlock失敗解消 (`LimitMEMLOCK` + `limits.d`)
- **背景**: PLEテーブルは約28.8 GiB（320,001,536行 × 90 B）だが、セッションのmemlock上限は約23.6 GiBで `PLE table table mlock failed` が発生し、メモリ圧迫時にテーブルがスワップアウトしてデコードが 4.3 tok/s まで崩落した。
- **対策**（ユーザーレベルのsystemdはセッションのハード上限までしか上げられないため、`limits.d` が本体）:
```bash
# /etc/security/limits.d/99-strata-memlock.conf
your_name soft memlock unlimited
your_name hard memlock unlimited

# ~/.config/systemd/user/strata.service の [Service] に
LimitMEMLOCK=infinity
```
- **結果**: エンジンログが `PLE table locked in RAM` に変化。退避によるストールが解消。

### 9. 2 MiB Hugepages（エキスパートアリーナ）
- **背景**: エキスパートアリーナは 23,983 × 2 MiB（約47 GiB）の巨大ページを要求するが、`vm.nr_hugepages=0` で4 KBページにフォールバックしていた。
- **注意**: 稼働中の `sysctl -w` ではメモリ断片化で 5,786 / 24,000 ページしか確保できず（エンジンは全数確保でないとhugepagesを使わない）、**ブート時確保が必須**:
```bash
# /etc/sysctl.d/99-strata-hugepages.conf
vm.nr_hugepages=24000
```
- **結果**: 再起動後 `hugetlb 2 MB pages` を確認。`HugePages_Free: 17`（23,983ページをエンジンが確保）。

### 10. `--prefill 2048` ➔ `--prefill auto`（チャンク8192自動選択）
- エンジンがVRAM状況から最大のチャンクを自動選択（この構成では8192、expert cacheから4.69 GiB/GPU借用）。
- 実測（同一スクリプト・毎回新規プロンプト、中央値）:

| プロンプト長 | `--prefill 2048` | `--prefill auto` (8192) | 向上 |
| :--- | :---: | :---: | :---: |
| 4,096 トークン | 529 tok/s | **609 tok/s** | +15% |
| 32,768 トークン | 633 tok/s | **1,065 tok/s** | **+68%** |
| 128,000 トークン | 657 tok/s | **1,174 tok/s** | **+79%** |

- デコード速度は変化なし（36〜40 tok/s、設計通り）。ニードルリコール検査は 32K/128K × 深さ10/50/90% で **6/6 正解**。
- 起動時のPLEテーブルロードは 45〜592 秒 ➔ **0.7 秒**（RAM常駐+hugepages、ウォーム時）。

### 補足（現状構成の是正）
- 現在の稼働構成は `--spec 4 --spec-min-p 0.5`（上記「2. 投機的サンプリング」時点の `--spec 2 / 0.6` から戻した。ドラフト採択率 72〜84% で問題なし）。
- 実測ハード仕様: CPUは **Xeon E5-2687W v4 ×2（24コア/48スレッド）**、RAM **188 GiB**、カーネル **7.0.0-30-generic**（冒頭の環境表は旧計測値）。
