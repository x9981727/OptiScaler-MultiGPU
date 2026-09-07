# RDNA4 r5.1 — 完整固定計畫 Head 替換測試包

這是 **已編譯的 Windows x64 測試程式**，不是只提供原始碼。
**不是可覆蓋遊戲的 DLL；也不是完整 Swin 重寫完成版。**

## 執行方式

把整包解壓到新的、可寫入的獨立資料夾，關閉遊戲後，雙擊 `TEST_R5.cmd`。
第一次依序選三個你已有的檔案：

1. `DLSSNR-rewrite-source-checkpoint.zip`，或其中的 `plans/1080p.json`。
2. `dlssnr_amd_pass1.dll`，或包含同一原版 GPU 核心的 `version.dll`。
3. `dlssnr_on_amd_weights.bin`。

不用 Python、Visual Studio 或系統管理員權限。checkpoint ZIP 可直接選，不必解壓；原版 DLL 與 147 MB 權重檔也不必複製、移動或改名。仍需要你 r4 測試已使用的 AMD HIP 7。
程式自動尋找名稱包含 9070 的顯卡，不會在你的 6600 XT 上執行 gfx1201 核心。

本包 **不包含上述 checkpoint、原版 DLL、模型權重、HIP 驅動**。它們不是新檔案，也不能用 r1/r2/r4 測試包或 Forward「結果 JSON」冒充 checkpoint。
缺少 checkpoint 時，應取得本對話先前保存的 `DLSSNR-rewrite-source-checkpoint.zip`，約 247 KB；不要用 `rdna4-r4-result.json` 或 `forward-20260907-000844-775001.json` 替代。

程式會即時顯示進度，並自動寫入：

- `rdna4-r5-result.json`
- `rdna4-r5-console.txt`

成功或失敗都保留兩個檔案。不要覆蓋遊戲 DLL，也不要刪除原權重。

## 與 r4 的差別

r4 比較單一 Head。r5 在保存的整段呼叫計畫中，只把 Head 換成兩個已測通的 r4 候選，其餘 GPU 核心仍為原版。
預期計畫為 44 個配置、154 次核心呼叫、4 次裝置內複製；真正執行前會核對固定計畫 SHA256 與結構。輸入不是即時遊戲影格，而是明確定義的三種合成初始化。

每次比較從相同狀態開始，檢查前段原核心產生的 Head 輸入、替換後的 Head 輸出與全部配置的最終位元組，也檢查模型區域及記憶體保護區。
整段 GPU graph 計時包含所有原版呼叫與原版裝置內複製，不把單層的加速倍數當成整體收益。每次樣本之前會重設狀態；重設／CPU 準備／讀檔／上傳不列入 GPU graph 時間。

## r5.1 本輪新增

- 即時 C++ 主控台與文字檔雙重紀錄，不使用 PowerShell 執行原則變更。
- JSON 記錄目前階段、最近提交的命令、核心名稱、grid/block 與替換模式。
- 每一轮計時後保存所有已完成樣本，中斷時可保留部分進度。
- 強制使用與你 r4 實機測試相同 SHA256 的 GPU code object，而非接受任意重新編譯的核心。
- 每個原版 Head 測試案例都檢查 FP8 NaN，異常時不繼續解讀效能。

非同步 GPU 錯誤可能晚於真正出錯的命令才出現；「最近提交的命令」是定位線索，不是已證明的肇因。

## 必須知道的驗證邊界

CI 已執行的編譯與 CPU 自測詳見 `BUILD_STATUS.json`、`reports/`。
**本次 CI 沒有讀取真正的 checkpoint，也沒有你的 RX9070XT，不能聲稱原版完整 Forward 在 CI 已跑通。**
程式只有在本機成功讀取並驗證 checkpoint 後，才可能執行完整固定計畫。
因此本包是供實機驗證的可執行成果；目前沒有新的 GPU 實測數字或遊戲 FPS 改善保證。

通過固定計畫不等於恢復了遊戲前處理、歷史狀態、時序畫質、HDR、D3D12/HIP 接合或完整 Swin。
這版還會以暫時改動 Head 輸出的反向案例檢查後段觀察區域是否敏感；若不敏感會另行標示，不以最終輸出相同冒充畫質驗證。
完整算法範圍與狀態說明見 `FULL_PLAN_SCOPE_zh-TW.md`。

## 檔案驗證與重現

`SHA256SUMS.txt` 列出 ZIP 內檔案雜湊。
使用 r4 GPU code SHA256：
`62c8ecf66e4290ffbe22230a376b475ef482807fe05c46b957457bff5826cffe`

預期 **解壓後的 1080p.json** SHA256（不是外層 checkpoint ZIP 的 SHA）：
`0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93`

`source/` 包含原始 r5 程式、確切補丁腳本、實際編譯的 `forward_delivery.cpp`、r4 GPU 原始碼與第三方授權。
`prepare_delivery.py` 會確認輸入原始碼的 Git blob，任何不符版本會中止建置。
開發分支：`dlssnr-rdna4-forward-r5-delivery`。main、r4 及原 r5 分支均未被本輪覆蓋。
