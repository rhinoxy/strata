# OpenClaw Integration & On-Demand Power-Saving Guide

このドキュメントでは、Strata（`Qwen3.8-Flash-Next`）を [OpenClaw](https://github.com/openclaw/openclaw) から利用し、GPU待機電力を最小限に抑えるための**オンデマンド起動・自動VRAM解放（アイドルアンロード）**の設定手順と運用方法を説明します。

---

## ⚡ 背景と目的

- **課題**: 80GB規模のモデル（Qwen3.8-Flash-Next）をデュアルGPU環境で常時VRAMにロードしていると、待機時でもGPU電力が消費されます。
- **解決策**:
  - **遅延ロード (`lazy_load: true`)**: サーバー起動時はPythonのHTTPリスナー（数十MB RAM）のみが待機し、VRAMは0%、GPU消費電力は最小（各9W〜10W）。
  - **オンデマンド・ロード**: OpenClaw等のクライアントから `/v1/chat/completions` リクエストが届いた瞬間にStrataエンジンが自動起動し、モデルをロード（約60秒）。
  - **アイドル時自動アンロード (`idle_unload_s: 300`)**: リクエスト完了後、指定秒数（デフォルト5分）アクセスが途絶えると自動でC++エンジンを終了し、VRAMを完全解放。

---

## ⚙️ Strata 設定 (`strata-iq3_s.json`)

```json
{
  "exe": "/home/your_name/Strata/engine/strata",
  "args": [
    "--pack", "/home/your_name/Strata-data/packs/iq3_s",
    "--native", "/home/your_name/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf",
    "--ple-gguf", "/home/your_name/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00002-of-00002.gguf",
    "--expert-profile", "/home/your_name/Strata/data/expert-profile.bin",
    "--expert-cache", "auto",
    "--prefill", "2048",
    "--spec", "2",
    "--spec-min-p", "0.6",
    "--mtp", "/home/your_name/Strata-data/mtp/rt",
    "--max-context", "65536",
    "--kv", "int8",
    "--kv-resident", "32768"
  ],
  "cwd": "/home/your_name/Strata",
  "tokenizer": "/home/your_name/Strata-data/packs/iq3_s/tokenizer",
  "model_name": "qwen3.8-flash-next-iq3_s",
  "log": "/home/your_name/Strata/strata-iq3_s.log",
  "lib_dirs": [
    "/opt/rocm/lib"
  ],
  "port": 8081,
  "backend": "hip",
  "env": {
    "STRATA_HIPBLASLT_TUNING": "/home/your_name/Strata/tools/hip/gfx1200-hipblaslt-100202.txt",
    "HSA_FORCE_FINE_GRAIN_PCIE": "1",
    "STRATA_WATCHDOG_S": "300"
  },
  "gpu": [1, 2],
  "gpus_asked": true,
  "layer_split": "auto",
  "lazy_load": true,
  "idle_unload_s": 300
}
```

### 重要な最適化パラメータ
1. `"lazy_load": true`: 起動時にモデルをロードせず、最初のリクエスト受信時にロード。
2. `"idle_unload_s": 300`: アイドル5分で自動アンロード。
3. `"--prefill", "2048"`: OpenClawの巨大なシステムプロンプト（2万トークン超）処理時のSSD/PLE読み込みレイテンシを平滑化。
4. `"STRATA_WATCHDOG_S": "300"`: 大規模プロンプト処理時のウォッチドッグ誤検知（デフォルト60s）を回避。

---

## 🛠️ OpenClaw への登録 (`~/.openclaw/openclaw.json`)

`models.providers` セクションに以下を追加します：

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
      "input": [
        "text"
      ],
      "cost": {
        "input": 0,
        "output": 0,
        "cacheRead": 0,
        "cacheWrite": 0
      },
      "contextWindow": 65536,
      "contextTokens": 65536,
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

---

## 🚀 サービス常駐化 (systemd --user)

再起動後も常時待機できるように、ユーザーサービスとして登録します。

### 1. ユニットファイルの配置
```bash
mkdir -p ~/.config/systemd/user
cp service/strata.service ~/.config/systemd/user/strata.service
systemctl --user daemon-reload
```

### 2. 起動と自動起動の有効化
```bash
systemctl --user enable --now strata
```

### 3. ステータス確認
```bash
systemctl --user status strata
```
待機時は `VRAM 0%`、CPU使用率ほぼ `0%` でポート8081をリッスンします。

---

## 💻 実行例

### OpenClaw CLI からの実行
```bash
# ヘッドレス実行
openclaw agent exec --model strata/qwen3.8-flash-next-iq3_s "質問文"

# 対話セッション
openclaw agent --model strata/qwen3.8-flash-next-iq3_s -m "こんにちは"
```

### 手動での即座アンロード（VRAM即時解放）
```bash
curl -s -X POST http://127.0.0.1:8081/unload -H "Content-Type: application/json" -d "{}"
```
