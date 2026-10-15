# OpenClaw Integration & Multi-Model Coexistence Guide

このドキュメントでは、Strata（`Qwen3.8-Flash-Next`）および Project Maya（`GLM-5.3-Flash`）を [OpenClaw](https://github.com/openclaw/openclaw) から利用し、GPU待機電力を最小限に抑えつつ、相互排他切り替えで共存運用するための設定手順と運用方法を説明します。

---

## ⚡ 背景と目的

- **課題**: 80GB〜90GB規模のモデル（Qwen3.8-Flash-Next / GLM-5.3-Flash）をデュアルGPU環境で運用する場合、2つのモデルを同時にロードするとVRAM/RAMが枯渇します。
- **解決策**:
  - **遅延ロード (`lazy_load: true`)**: サーバー起動時はHTTPリスナーのみが待機し、VRAMは0%、GPU待機電力を最小化。
  - **相互排他ロード (`before_load`)**: Qwen起動時はGLMを自動アンロードし、GLM起動時はQwenを自動アンロード。相手が確実にアンロードされてVRAMが解放された後に自身のロードを開始。
  - **常駐性とS3サスペンド耐性**: アイドル時間による強制アンロードを行わないことで、作業中の再ロード待ち（10〜15分）を排除。Linuxカーネル（amdgpu）のVRAM退避機能により、モデルが載ったままS3サスペンドに入っても復帰後にそのまま推論を継続可能。

---

## 🔄 相互排他アンロードスクリプト (`tools/ensure_exclusive.sh`)

一方のモデルがロードを開始する直前に、もう一方のモデルを確実にアンロード（VRAM 0MBに解放）します。

```bash
# Strataが起動する直前: Mayaをアンロード
tools/ensure_exclusive.sh maya

# Mayaが起動する直前: Strataをアンロード
tools/ensure_exclusive.sh strata
```

---

## ⚙️ サーバー設定

### 1. Strata (Qwen 3.8 Flash Next / Port 8081)
`configs/strata-iq3_s.json.example` を参照：
```json
{
  "model_name": "qwen3.8-flash-next-iq3_s",
  "port": 8081,
  "lazy_load": true,
  "before_load": "/path/to/Strata/tools/ensure_exclusive.sh maya",
  "gpu": [1, 2]
}
```

### 2. Project Maya (GLM-5.3-Flash / Port 8082)
`configs/maya-maya-s24-hip.json.example` を参照：
```json
{
  "model_name": "glm-5.3-flash",
  "port": 8082,
  "lazy_load": true,
  "before_load": "/path/to/Strata/tools/ensure_exclusive.sh strata",
  "gpu": [1, 2],
  "env": {
    "STRATA_GLM_RAM_RESIDENT": "1",
    "HSA_FORCE_FINE_GRAIN_PCIE": "1"
  }
}
```

---

## 🛠️ OpenClaw Gateway への登録 (`openclaw.json`)

OpenClaw Gateway（別マシンまたは同一マシン）のプロバイダ設定に両モデルを登録します：

```json
{
  "models": {
    "providers": {
      "strata": {
        "baseUrl": "http://<host-ip>:8081/v1",
        "apiKey": "<your-strata-api-key>",
        "api": "openai-completions",
        "models": [
          {
            "id": "qwen3.8-flash-next-iq3_s",
            "name": "Qwen 3.8 Flash Next (Strata)"
          }
        ]
      },
      "maya": {
        "baseUrl": "http://<host-ip>:8082/v1",
        "apiKey": "<your-strata-api-key>",
        "api": "openai-completions",
        "models": [
          {
            "id": "glm-5.3-flash",
            "name": "GLM-5.3-Flash (Maya)"
          }
        ]
      }
    }
  }
}
```

---

## 🚀 systemd ユーザーサービスとしての常駐化

```bash
mkdir -p ~/.config/systemd/user
cp service/strata.service ~/.config/systemd/user/
cp service/maya.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now strata maya
```

---

## 💻 運用と動作検証

### 1. 手動でのアンロード確認
```bash
# Strata (Qwen) のアンロード
curl -s -X POST http://127.0.0.1:8081/unload -H "Content-Type: application/json" -H "Authorization: Bearer <your-key>" -d "{}"

# Maya (GLM) のアンロード
curl -s -X POST http://127.0.0.1:8082/unload -H "Content-Type: application/json" -H "Authorization: Bearer <your-key>" -d "{}"
```

### 2. S3サスペンドについての補足
- AMD GPU（amdgpuドライバ）環境では、モデルがVRAM/RAMにロードされた状態でS3サスペンド（Suspend to RAM）に入っても、ドライバによるメモリ保護機能により、レジューム復帰後に正常に推論を再開できます。
