# AMD Radeon RX 6400 Display GPU Isolation & Clean ROCm/HIP Verification Report

## 概要 (Overview)

本ドキュメントは、表示用 GPU（AMD Radeon RX 6400, 4GB）と計算用 GPU（Dual AMD Radeon RX 9060 XT, 各 16GB）が混在するマルチ GPU 環境において、OS および `amdgpu-lib` パッケージ更新後に発生したホストメモリピン留め（USERPTR）エラー・起動失敗の根本原因の解明、ならびに **エンジン C++ ソースコード（`src/`）への改造を一切行わないクリーンビルド** と **「表示用 GPU 除外シム（`libkfd_ignore_rx6400.so`）」のみによる完全動作検証結果** を記録するものです。

---

## 🖥️ ハードウェア・ソフトウェア構成

- **OS**: Ubuntu 24.04 LTS (x86_64, Linux kernel)
- **CPU**: Dual Intel Xeon CPU E5-2687W v4 @ 3.00GHz (Dual-Socket, 計 24 コア / 48 スレッド, PCIe 3.0, SWIOTLB 64MB)
- **RAM**: 377 GiB (Dual-Socket NUMA Node 0 / Node 1)
- **ROCm**: ROCm 7.2.4 (`/opt/rocm-7.2.4`, Clang++ 24.0.0, hipBLASLt 1.2.0)
- **GPU トポロジー (KFD Topology)**:
  - **Node 2**: AMD Radeon RX 6400 (gfx1034, 4GB, PCI `05:00.0`, `gpu_id=21283`, NUMA Node 0) → **表示用 (Display / GUI)**
  - **Node 3**: AMD Radeon RX 9060 XT #1 (gfx1200, 16GB, PCI `08:00.0`, `gpu_id=59575`, NUMA Node 0) → **計算用 #1**
  - **Node 4**: AMD Radeon RX 9060 XT #2 (gfx1200, 16GB, PCI `a3:00.0`, `gpu_id=42888`, NUMA Node 1) → **計算用 #2 (独立 PCIe バス)**

---

## 🔍 発生していた障害と根本原因の分析

直前のパッケージ更新以降、推論リクエスト受信時に以下のエラーが発生し、推論エンジンが起動完了できない状態になっていました：

1. **ROCm / `libhsa-runtime64` のホスト USERPTR 割り当て先バインドバグ**:
   - `HIP_VISIBLE_DEVICES` や `"gpu"` 設定で RX 6400 を除外していても、ROCm の HSA ランタイム内部で `default_gpuid_` が KFD トポロジー先頭の GPU ノード（RX 6400, `gpu_id=21283`）に束縛されたままになります。
   - すべてのホストピン留め（`hipHostMalloc` / `hipHostRegister`）において RX 6400 の `gpu_id` が KFD `ALLOC_MEMORY_OF_GPU` (0x16) ioctl に渡され、RX 6400 は VRAM が 4GB しかないため、4GB を超えるホストピン留めがドライバにより拒否されます。

2. **カーネル更新に伴う SWIOTLB (64MB) / IOMMU 制約の厳格化**:
   - Node 3 (`08:00.0`) はマザーボード上の PCIe スイッチ配下にあり、更新後のカーネル/ドライバでは 64KB を超えるホスト USERPTR の DMA マッピングが SWIOTLB バウンスバッファ上限により `amdgpu: DMA map userptr failed: -22 (EINVAL)` で拒否されるようになりました。
   - 従来の `kfd_ignore_rx6400.c` はリダイレクト先として Node 3 (`59575`) をハードコードしていたため、322MB のホストピン留め（`cudaHostAlloc`）が `-22 (EINVAL)` で失敗し、`cannot pin 322 MiB: no ROCm-capable device is detected` や `cudaHostAlloc for the staging buffers failed` が発生していました。
   - 一方、Node 4 (`a3:00.0`) は CPU Socket 1 直結の独立 PCIe バスに配置されており、48GB〜100GB を超える巨大なホスト USERPTR もカーネルレベルで完全にピン留め・DMA 転送可能です。

3. **HSA キュー作成時の一括マルチデバイスマップ失敗による SIGSEGV**:
   - キュー作成時、HSA ランタイムが表示用 GPU と計算用 GPU の両方に一度にリングバッファをマップしようとした際、カーネルが拒否してキュー作成が `EINVAL (22)` で失敗し、プロセスがクラッシュしていました。

4. **ROCm 7.x におけるホスト登録ポインタ属性の誤認**:
   - HIP ランタイムの `hipDeviceGetAttribute(&ok, hipDeviceAttributeCanUseHostPointerForRegisteredMem, dev)` が誤って `0` を返します。
   - これにより、Project-Maya の大容量 RAM tier ピン留め（`numa_pinned`）が即座にバイパスされ、フォールバックの単一 `cudaHostAlloc` に落ちて `the pinned RAM tier did not allocate` を引き起こしていました。

---

## 🛠️ 施した対策とアーキテクチャ

### 1. エンジン C++ ソースコードの完全クリーン復元
- 一時的に検証していた個別バイパス（`src/core/native_head.cpp` の chunked copy 等）はすべて破棄（`git restore`）し、公式コミットと完全一致（差分ゼロ）の状態でビルドしました。

### 2. 最小限・明快な「表示用 GPU 除外シム」([`tools/hip/kfd_ignore_rx6400.c`](file:///home/k-takeda/Strata/tools/hip/kfd_ignore_rx6400.c))
約 140 行のクリーンな shim として整理しました：

```c
// 1. 表示用 GPU (21283) 宛ての USERPTR 割り当てを、大容量ピン留めが可能な Node 4 (42888) にリダイレクト
if (a->gpu_id == display_gpu_id && (a->flags & KFD_IOC_ALLOC_MEM_FLAGS_USERPTR) && a->size > 65536) {
    a->gpu_id = compute_gpu_id; // Node 4 (42888)
}

// 2. MAP_MEMORY_TO_GPU 失敗時のデバイス個別マップフォールバック
// (複数デバイス一括マップが弾かれた場合、各ターゲットデバイスごとに n_devices = 1 でマップ)

// 3. hipDeviceAttributeCanUseHostPointerForRegisteredMem (4) に対する 1 のエミュレーション返却
```

### 3. 設定ファイルの統一
- `strata-iq3_s.json`: `"gpu": [2]` (Node 4)
- `maya-maya-m-derisked-hip.json`: `"gpu": [2]` (Node 4)
- `tools/ensure_exclusive.sh`: サービス間のオンデマンド相互排他アンロードを管理。

---

## 📊 クリーン環境での動作・推論検証結果

### ① Strata (Qwen3.8-Flash-Next IQ3_S, ポート 8081)
- **ビルド状態**: `src/` 差分なし、Clean CMake + Ninja 再ビルド
- **推論テスト (`What is the capital of France?`)**:
  ```
  strata serve: prompt 59 tokens read in 3264 ms (18.1 tok/s), 16 generated in 1172 ms (13.7 tok/s)
  data: {"choices": [{"delta": {"reasoning_content": "We need to respond to user: \"What is the capital of France?\" Simple..."}, ...}]}
  ```
- **結果**: 正常にストリーミングトークン生成を確認。

### ② Project-Maya (GLM-5.3-Flash Maya-M-Derisked, ポート 8085)
- **ビルド状態**: `src/` 差分なし、Clean CMake + Ninja 再ビルド
- **モデルロード & ウォームアップ**:
  - Pinned RAM Tier: **100.27 GB (12,432 スロット)** 割り当て成功
  - ウォームアップ: **全 12,096 エキスパート (104.8 GB)** を 35 秒 (2.97 GB/s) で VRAM / RAM にロード完了
  - `glm fast: CUDA0 RAM-resident verified: all 12096 experts are in VRAM or pinned RAM`
- **推論テスト (`What is 2+2?`)**:
  ```
  data: {"choices": [{"delta": {"content": "2 + 2 = **4**\n\nIt"}, ...}]}
  timings: prompt 19 tokens (5.2 tok/s), predicted 16 tokens (10.2 tok/s)
  ```
- **結果**: 正常に推論・ストリーミング出力を確認。

---

## 結論

C++ ソースコード本体にパッチを当てる必要はなく、**「表示用 GPU（RX 6400）除外シム（`libkfd_ignore_rx6400.so`）」の適切なリダイレクト（Node 4 指定）とフォールバック処理のみで、Strata および Project-Maya の両エンジンが完全なクリーン状態から正常に起動・推論可能** であることが実証されました。
