# RDNA4 Core r2：資料重排與自動選擇實驗

**不是遊戲 DLL、不是完整 Swin／DLSS-NR，也沒有 NVIDIA 效能對比結果。**
r2 保留 r1 的三個線性算子，增加無損 GPU 資料重排與三個新算子。只在獨立資料夾執行，請不要覆蓋遊戲檔案。

## 執行

解壓完整 ZIP，關閉遊戲及其他高 GPU 負載程式，執行 `TEST_R2.cmd`。
程式只使用本機可載入的 AMD HIP 7（amdhip64_7.dll），不需要 Python、Visual Studio 或系統管理員權限。
自動選擇名稱含 9070 的裝置；不會在 6600 XT 執行 gfx1201 專用核心。
保留日常穩定的顯卡設定。本工具不修改驅動、時脈、電壓、登錄或遊戲。

結果為 `rdna4-r2-result.json` 和 `rdna4-r2-console.txt`。若中止也請保留檔案；不要把中途 running 或 failed 狀態視為完成。

本測試使用合成矩陣，不需要模型權重。**原本遊戲中的 dlssnr_on_amd_weights.bin 仍須保留。**

## r2 的變更

1. `pack_kmajor_fp8` 將 FP8 輸入無損重排為每個 wave/lane 所需的連續 8-byte 片段；不額外量化、不縮減模型。
2. `linear_fp8_packed16` 和 `linear_fp8_packed32` 直接載入整理好的片段，減少原版實验算子的逐 byte 索引和拼接。
3. `linear_fp8_packed16_w4` 測試每工作群組四個獨立 wave32，各算一個 16x16 tile；無跨 wave 通訊。
4. 固定保留 r1 的 tile16/tile32/FP16 對照，不假設大 tile 一定較快。

資料排列：[outerTile][kTile][lane][8 bytes]。Outer 補到 32 的倍數，K 補到 16 的倍數；GPU 重排結果對所有 bytes（含補零）與 CPU 獨立排列逐一比較，並檢查前後 guard。運算結果仍是 FP32。

## 計時規則

每個矩陣形狀先執行含 GPU 工作的暖身迴圈（至少 150 ms 的牆鐘時間，包含呼叫／同步時間），再以固定種子打亂所有版本順序，交錯量測 15 輪訓練樣本及 7 輪保留驗證樣本。
每筆樣本是 64 次相同 operation/pipeline 的 HIP graph replay 平均，單位 ms。**沒有鎖定或量測 GPU 時脈／功耗狀態，仍不能排除外部干擾。**

三種 packed 測試意義不同，不能混用：

| 報告 mode | 包含 | 不包含 |
|---|---|---|
| packed_core | 已重排的 X、W 做矩陣乘法及 epilogue | X/W 重排 |
| pack_x_and_core | 每次 X 重排 + 矩陣運算 | 可重用 W 的一次性重排 |
| pack_both_and_core | 每次 X、W 都重排 + 矩陣運算 | CPU 準備、傳輸等 |

raw_core 是 r1 原始未重排的算子。所有測試都不包含 CPU 資料準備、GPU 上傳、完整模型或 D3D12 整合。暖快取、重複相同矩陣的結果不等於真實網路時間。

正確性：小矩陣逐項比較，大矩陣檢查所有輸出有限性與約 4,100 個固定抽樣位置；每個圖形路徑也會檢查。測試數值沿用 r1 合成資料的範圍，沒有驗證所有 FP8 值組合或真實模型數值分布。

## 自動選擇如何避免誤判

報告中的 recommendations_operator_only 僅是單次測試的每形狀建議，不會修改遊戲設定或產生可用的遊戲調校檔。
候選只能是 raw FP8 或包含 X 重排的 packed 路徑；不拿免費排除重排成本的 packed_core 去勝出，也不把排除 FP16 轉換成本的版本混入選擇。

以 r1 FP8 tile16 為保守基準。先在訓練樣本檢查至少約 5% 的中位數改善、至少 12/15 輪勝出，以及中央樣本波動範圍；選擇之後才看 7 輪保留樣本。
保留樣本要求至少 5/7 輪超過 5% 改善、且中位數及穩定性均過門檻。失敗就保留基準，不輸出已驗證加速結論。
p90/p10 是排序樣本按整數索引取分位值之比，不是信賴區間；樣本數有限。即使通過，本建議也不是跨遊戲、跨解析度、跨驅動的通用最優值。

## 結果狀態

- cpu_tests_passed_gpu_not_run：只有 CPU 自測。
- operator_and_pack_tests_passed_not_model_validation：本機完成算子與 GPU packing 檢查，不代表模型／畫質一致性已通過。
- failed：停止，保留錯誤報告。

## 尚未完成

原版 Swin 的精確 layer/weight layout 對應、完整神經網路替換、影格歷史與 HDR 驗證、遊戲 DLL 整合、遊戲 FPS 與原生 NVIDIA DLSS-NR 比較都尚未完成。
原始 runtime 在 r1 靜態檢查中已確認包含 gfx1201 FP8 WMMA；本次測的是**資料搬運與工作分配**，不是宣稱第一次開啟 FP8。

## 來源與建置

分支：x9981727/OptiScaler-MultiGPU:dlssnr-rdna4-core-r2。
r2 以 fce9e0b87183647928b886216ccfa56bba0bf2ad 的 r1 作為基底，不修改 main 或 r1。
`packed_test.cpp` 直接包含 r1 的 `linear_test.cpp`，重用 HIP API 載入器、CPU reference 與數值轉換；入口僅為 r2 wmain。舊程式碼保留作為對照。
`source` 包含全部相依原始碼；`reports` 包含 GPU 交叉編譯檢查及 CI CPU 自測。CI 不具 RX9070XT GPU，不宣稱已完成 r2 GPU 測試。
技術參考：https://gpuopen.com/learn/wmma-guide-amd-rdna-4-gpus-part-2/

新寫的 r2 程式碼採 MIT License；見 LICENSE_r2.txt。
