# DLSSNR-RDNA4-Core r1 — 核心算子實驗版

**這不是可替換遊戲 DLL，不是完整 Swin，也不是已加速的 DLSS-NR。**
目前交付的是針對 RX 9070 / 9070 XT（gfx1201）編譯的獨立矩陣算子、Windows 測試程式、原版核心靜態檢查及可重現建置流程。請在獨立資料夾執行，不要放進遊戲或覆蓋任何 DLL。

## 已實作

- `linear_fp8_tile16`：16x16 分塊對照實作。
- `linear_fp8_tile32`：單一 wave32 計算四個 16x16 區塊；同一組 A/B 暫存器片段重複用於多次矩陣乘法。
- `linear_fp16_tile32`：FP16 輸入的對照實作。
- 三者使用 FP32 累積與輸出，合併縮放、bias 與 residual。FP8 使用 gfx1201 E4M3FN，而非 FNUZ。
- GPU 程式碼交叉編譯；建置檢查生成的原生 WMMA 指令、參數位置、wave32、零静態 LDS、零 scratch 及零 VGPR spill。
- Windows EXE 動態使用本機 AMD HIP 7，不需要 Python、Visual Studio 或另外安裝編譯器。
- 測試先檢查 CPU 數值轉換與分塊索引，再檢查 GPU 輸出、邊界 guard 與非有限值，最後使用 HIP graph replay 計時。

## 必須更正的原版判斷

固定檢查 MatheusGViana/dlss-5-amd-project 的 commit `04d8b83f26adcbdd489137ea27a3ce59fc75eab4`。
原版 `version.dll` SHA256：
`106223723fd9266c44d38dc2fb77933948ab37803f46bfcea2bae3a0a474ac84`

這版原始 DLL **已有 gfx1201 專用 code object，五個 `k_swin_var` 均有 FP8 WMMA 指令**。
所以不能把「改成 FP8／開啟 AI 加速器」當作尚未實施的優化，也不能據此保證兩倍加速。
這版 Swin 靜態 LDS 約 15,616–19,200 bytes、VGPR 89–134，與其他版本的分析數據不能混用。
完整的版本限定結果在 `audit/runtime-audit.json`。

注意：靜態指令數不是實際運算利用率；静態 LDS 也不包含動態 shared memory。
之前 23.586 ms 的測試為固定合成 Forward；逐算子插樁時間與 graph replay 中位數不是同一次測量。
不應把兩者直接相除作為精確瓶頸百分比，也不能把本包的線性算子數字與 23.586 ms 直接比較。

## 操作

1. 把完整 ZIP 解壓到可寫入的獨立資料夾。
2. 關閉遊戲與其他 GPU 高負載程式，保持日常穩定的顯卡設定。
3. 雙擊 `TEST_9070XT.cmd`。
4. 檢查 `rdna4-result.json` 及 `rdna4-console.txt`。將這兩份檔案提供給開發者。

程式自動尋找名稱包含 9070 的顯卡，不假定 HIP 裝置 0 是主卡。若同時有 6600 XT，不會自動在其上執行這些 gfx1201 核心。
需要指定時執行：

```bat
rdna4-test.exe --device 1 --output rdna4-result.json
```

裝置編號以程式列出的清單為準，不應照抄上面的 1。
若找不到 `amdhip64_7.dll`，會中止並記錄錯誤；保留原有 HIP 7 安裝，或由 `HIP_PATH` 指向正確 HIP 7 安裝目錄。
本測試不下載驅動、不修改登錄、不關閉安全軟體、不要求系統管理員權限，也不需要 `dlssnr_on_amd_weights.bin`。
最後一句僅適用於本獨立算子測試；**你原本遊戲中的 DLSS-NR 仍需要權重檔，不可因而刪除。**

## 如何解讀結果

`cpu_tests_passed_gpu_not_run`：僅 CPU 測試，不代表 GPU 正確。
`operator_tests_passed_not_model_validation`：本機獨立算子測試通過，不代表原始模型或遊戲畫質驗證通過。
`failed`：請保留錯誤報告，不應把本版接入遊戲。

小型／不規則矩陣會逐項比較，大型矩陣檢查所有輸出的有限性並對最多約 4,100 個位置作數值比較；實際檢查個數記在 `elements_checked`。
GPU 計時每筆為 32 次相同算子的 graph replay 平均，共 7 筆樣本。這是暖快取、獨立算子測試，不含模型張量配置、CPU pack、傳輸、D3D12/HIP 同步、完整網路或遊戲渲染。
只允許比較同一 shape 下 tile16/tile32 的候選表現。較小的暫存器／LDS 用量不能直接換算為整個 Swin 的加速倍數。

## 尚未完成，不能宣稱已完成的部分

- 原始 VarParams（168-byte by-value）內部欄位與每層張量／權重 layout 的完整重建。
- 用本版算子替换原始 Swin 的 QKV、attention、MLP 與其正規化／位置偏移／激活／量化細節。
- 全模型數值、時間穩定性、HDR、實際遊戲影格的驗證。
- D3D12/HIP 接合、drop-in DLL、遊戲 FPS、NVIDIA 同條件比較。

採用逐算子替換，不用未驗證的函式簽名寫入原版 runtime，也不移除原版雜湊檢查強行載入。

## 可重現來源與建置

開發分支：`x9981727/OptiScaler-MultiGPU:dlssnr-rdna4-core-r1`。
獨立檔案只位於 `rdna4/` 及 `.github/workflows/rdna4-core.yml`；不合併或覆蓋原來的 MultiGPU 主分支。
Linux 使用 LLVM/Clang 19 建置 gfx1201 code object，Windows 使用 MSVC x64 `/MT` 建置測試程式。
`source/` 包含本包工具與原始碼；`reports/` 包含編譯結果和 CI CPU 自測。
本包不包含原版 runtime、模型權重或 AMD HIP 驅動 DLL。

技術參考：
- https://gpuopen.com/learn/using_matrix_core_amd_rdna4/
- https://clang.llvm.org/docs/AMDGPUBuiltinReference.html
- https://rocm.docs.amd.com/projects/HIP/en/docs-7.2.0/doxygen/html/group___graph.html
- https://github.com/MatheusGViana/dlss-5-amd-project/tree/04d8b83f26adcbdd489137ea27a3ce59fc75eab4
