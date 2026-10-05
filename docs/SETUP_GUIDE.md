# 完全再現ガイド: デュアル RX 9060 XT + デュアルXeon (NUMA) 環境

このガイドは、次の環境で Qwen3.8-Flash-Next (IQ3_S) を本リポジトリの実測構成
（デコード 36〜40 tok/s、プリフィル 600〜1,170 tok/s、256Kコンテキスト）で
そのまま動かすための手順です。設定ファイルはすべて実稼働中のものを全文掲載しています
（APIキーのみプレースホルダ）。

対象環境（これと同等ならそのまま使えます）:

| 項目 | 構成 |
| :--- | :--- |
| CPU | デュアルソケット Xeon (E5-2687W v4 ×2, 24コア/48スレッド, AVX-512なし) |
| RAM | 188〜192 GB DDR4 |
| GPU | AMD Radeon RX 9060 XT ×2 (gfx1200, 16 GB) |
| OS | Ubuntu + ROCm 7.2.4 |
| ストレージ | HDD可（`--ple-io ram` によりHDDシークは発生しない） |
| 必要ディスク | モデル約84 GB + パック等 → 空き 120 GB 以上 |

---

## 1. ビルド

```bash
sudo apt install -y build-essential cmake git numactl rocm-dev hipblaslt-dev

git clone git@github.com:rhinoxy/strata.git
cd strata

cmake -B build-hip -S . \
  -DSTRATA_BACKEND=hip \
  -DCMAKE_HIP_ARCHITECTURES=gfx1200 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-hip -j$(nproc)

mkdir -p engine
cp build-hip/strata engine/strata

python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

GPUの番号は `rocm-smi --showproductname` で確認してください。この環境では
RX 6400 (gfx1034) が1枚混在しているため、9060 XT が GPU[1] と GPU[2] になり、
設定の `"gpu": [1, 2]` はこれに対応しています。自分の環境の番号に合わせてください。

## 2. モデルデータの配置

`~/Strata-data/`（Strataフォルダの隣に作成）に配置します。

```
~/Strata-data/models/IQ3_S/
  Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf   (54.8 GB)
  Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00002-of-00002.gguf   (28.8 GB, PLEテーブル)
~/Strata-data/packs/iq3_s/        ← tools/strata_pack.py で生成（または ./setup.sh）
~/Strata-data/mtp/rt/             ← tools/mtp_fetch.py fetch --out ~/Strata-data/mtp/rt
```

- GGUF本体: Hugging Face `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF` の IQ3_S
- エキスパートプロファイル `data/expert-profile.bin` はリポジトリに同梱済み
  （24,576 ranked pairs）。**マシンの異なる環境では `./setup.sh --calibrate`
  での再生成を推奨**します（ヒット率が上がります）。

## 3. OSレベルの修正（3件、最初にやるのが確実）

### 3a. memlock無制限（PLEテーブル約28.8 GiBのmlockに必須）

```bash
# /etc/security/limits.d/99-strata-memlock.conf
your_name soft memlock unlimited
your_name hard memlock unlimited
```

`your_name` は自分のユーザー名に。ユーザーレベルのsystemdはセッションの
ハード上限までしか上げられないため、**この limits.d が本体**です。
反映には再ログイン（または `sudo prlimit --pid $(pgrep -u $USER -x systemd | head -1) --memlock=unlimited`）。

### 3b. Hugepages 48 GiB（エキスパートアリーナ用、**ブート時確保が必須**）

```bash
# /etc/sysctl.d/99-strata-hugepages.conf
vm.nr_hugepages=24000
```

**再起動してください。** 稼働中の `sysctl -w` ではメモリ断片化で確保数が
足りなくなります（実測 5,786/24,000）。エンジンは全数（23,983ページ）確保で
ないとhugepagesを使いません。確認:

```bash
grep HugePages_Total /proc/meminfo   # 24000 と出ればOK
```

### 3c. systemdサービスのmemlock指定

下記のサービスファイルに `LimitMEMLOCK=infinity` を含めてあります（3aと両方必要）。

## 4. 設定ファイル `strata-iq3_s.json`（全文）

Strataフォルダ直下に作成。**`/home/your_name` を自分のパスに、
`api_key` にランダムな秘密文字列を設定**してください（`host: 0.0.0.0` で
公開するので必須）。

```json
{
  "exe": "/home/your_name/Strata/engine/strata",
  "args": [
    "--pack", "/home/your_name/Strata-data/packs/iq3_s",
    "--native", "/home/your_name/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf",
    "--ple-gguf", "/home/your_name/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00002-of-00002.gguf",
    "--expert-profile", "/home/your_name/Strata/data/expert-profile.bin",
    "--expert-cache", "auto",
    "--prefill", "auto",
    "--ple-io", "ram",
    "--spec", "4",
    "--spec-min-p", "0.5",
    "--mtp", "/home/your_name/Strata-data/mtp/rt",
    "--max-context", "262144",
    "--kv", "int8",
    "--kv-resident", "32768"
  ],
  "cwd": "/home/your_name/Strata",
  "tokenizer": "/home/your_name/Strata-data/packs/iq3_s/tokenizer",
  "model_name": "qwen3.8-flash-next-iq3_s",
  "log": "/home/your_name/Strata/strata-iq3_s.log",
  "lib_dirs": ["/opt/rocm/lib"],
  "host": "0.0.0.0",
  "port": 8081,
  "api_key": "<ここにランダムな秘密文字列>",
  "allowed_hosts": ["*"],
  "backend": "hip",
  "env": {
    "STRATA_HIPBLASLT_TUNING": "/home/your_name/Strata/tools/hip/gfx1200-hipblaslt-100202.txt",
    "HSA_FORCE_FINE_GRAIN_PCIE": "1",
    "STRATA_WATCHDOG_S": "300"
  },
  "gpu": [1, 2],
  "gpus_asked": true,
  "layer_split": "auto",
  "lazy_load": false,
  "idle_unload_s": 1800
}
```

各設定の要点:

- `--prefill auto`: エンジンがVRAM状況からチャンクを自動選択（この構成で8192）。
  2048固定比でプリフィル +68%（32K）〜 +79%（128K）。
- `--ple-io ram`: PLEテーブル28.8 GiBをRAM常駐+mlock。HDD構成でのシーク待ちを完全排除。
- `--kv int8 --kv-resident 32768`: 256KコンテキストをVRAM約11 GiBで保持
  （残りはpinned RAMへストリーミング、VRAMヒット率96〜99%）。
- `--spec 4 --spec-min-p 0.5` + MTP: ドラフト採択率72〜84%。
- `layer_split auto`: レイヤー0-23をGPU0、24-47+ヘッドをGPU1に自動分割。
- `idle_unload_s: 1800`: 30分アイドルでエンジン解放（省電力）。`lazy_load: false`
  と組み合わせると常駐モード。省電力重視なら `lazy_load: true` + `idle_unload_s: 300`
  （起動約60秒と引き換えに待機時VRAM 0%・GPU 9〜10W）。

## 5. 起動スクリプト `run-iq3_s.sh`（全文）

```bash
#!/bin/sh
cd "/home/your_name/Strata"
exec numactl --interleave=all "/home/your_name/Strata/.venv/bin/python" \
  "/home/your_name/Strata/serve/server.py" \
  "--engine" "strata" "--config" "/home/your_name/Strata/strata-iq3_s.json" \
  "--port" "8081" "$@"
```

`chmod +x run-iq3_s.sh`。**デュアルソケット環境では `numactl --interleave=all` が必須**
（未指定時 6.44 → 10.14 tok/s、実測1.57倍）。

## 6. systemdユーザーサービス（全文）

`~/.config/systemd/user/strata.service`:

```ini
[Unit]
Description=Strata OpenAI-Compatible Inference Server
After=network.target

[Service]
Type=simple
WorkingDirectory=/home/your_name/Strata
ExecStart=/home/your_name/Strata/run-iq3_s.sh
Restart=on-failure
RestartSec=5s
LimitMEMLOCK=infinity

[Install]
WantedBy=default.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now strata
```

ログアウト後も常駐させたい場合は `loginctl enable-linger $USER`。

## 7. 起動確認チェックリスト

`tail -f strata-iq3_s.log` で以下を順に確認:

```
strata generate: PLE table locked in RAM (--ple-io ram)     ← mlock成功（3a/3c）
strata generate: expert arena: ... hugetlb 2 MB pages       ← hugepages有効（3b）
strata serve: prompt chunk auto: 8192 tokens                ← prefill auto動作
strata generate: session is up (engine 0.1.38)              ← 起動完了
```

失敗時のログと対処:

| ログ | 原因 | 対処 |
| :--- | :--- | :--- |
| `PLE table mlock failed (raise ulimit -l)` | limits.d未反映 | 3a再適用→再ログイン→サービス再起動 |
| `MAP_HUGETLB unavailable (needed 23983 ... vm.nr_hugepages=N)` | N<23983 | 3b適用後に**再起動** |
| `this CPU has no AVX-512` | 正常（このCPUはAVX-2） | 対処不要 |

動作確認:

```bash
curl -s http://127.0.0.1:8081/v1/chat/completions \
  -H "Authorization: Bearer <api_key>" -H 'Content-Type: application/json' \
  -d '{"model":"qwen3.8-flash-next-iq3_s","messages":[{"role":"user","content":"こんにちは"}],"max_tokens":32}'
```

## 8. 期待パフォーマンス（同一環境実測、2026-10-04/05）

| プロンプト長 | プリフィル (tok/s) | デコード (tok/s) |
| :--- | :---: | :---: |
| 4,096 | 609 | 40.2 |
| 32,768 | 1,065 | 37.1 |
| 128,000 | 1,174 | 36.4 |

- 長文リコール（ニードル検査）: 32K/128K × 深さ10/50/90% で 6/6 正解
- Expert Cache ヒット率 84〜96%、KVストリーミングVRAMヒット率 96〜99%
- 詳細データ: [bench/results/2026-10-04-amd-dual-9060xt-tuning/](../bench/results/2026-10-04-amd-dual-9060xt-tuning/README.md)

## 9. OpenClaw連携（任意）

`~/.openclaw/openclaw.json` のプロバイダ設定は
[docs/OPENCLAW.md](OPENCLAW.md) を参照。`baseUrl: http://127.0.0.1:8081/v1`、
`apiKey` は strata-iq3_s.json と同一の値。
