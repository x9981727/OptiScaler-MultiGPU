# RDNA4 r6.2 — 最後輸出控制值對照

這是 Windows x64 獨立測試程式，**不是可覆蓋遊戲的最終加速 DLL**。
由你執行 GPU 實測；本包不需要遠端控制、不安裝服務。

## 執行

完整解壓到新的可寫入資料夾，關閉遊戲後雙擊 `RUN_AUTOLAB.cmd`，確認開頭是 r6.2。
依序選原本的 checkpoint ZIP、`dlssnr_amd_pass1.dll`／原版 `version.dll`、`dlssnr_on_amd_weights.bin`。
不用重新下載 checkpoint，不用搬移模型，不需要 Python、Visual Studio 或系統管理員權限。沿用既有 HIP 7。
自動選擇名称含 9070 的顯卡；不會把 gfx1201 核心送到你的 6600 XT。

回傳新檔案：`rdna4-r62-result.json` 與 `rdna4-r62-console.txt`。成功、未通過或報錯都保留。
原 DLL、模型與 checkpoint 全部唯讀，不包含在本包。

## 為什麼不是再重跑 r6.1

目前取得的最新實機報告是 r6.0，**沒有把 r6.1 當成已通過**。
r6 的 Head 擾動一路影響到命令156之後的 allocation26，但最後 allocation2不變。
本版核對 r6 報告中的最後一個 168-byte 參數區塊：offset136 的 float32 位元是正零。
固定原版 ISA 中有從0x88載入 s12 的指令，輸出區又使用 s12 乘上模型投影結果。這支持「零倍率可能掩蓋網路差異」的假說，**不是完整靜態資料流證明，也還不是實機驗證**。
來源片段與 CPU 核對結果在 `reports/reported-terminal-fixture.json`、`control136-static-report.json`、`control136-isa-evidence.txt`。

## 實際執行的新增實驗

先保留既有三種初始化的完整原版／Head替換版逐位元組檢查。
接著使用同一個變動 float32 初始化，分開測試最後呼叫的 offset136 = **0、0.125、1**。

**0 是保存計畫的原值；0.125 與1是刻意的實驗值，並非已恢復的遊戲預設值。**
每次試驗都從相同狀態重新執行完整前段，只在最後一個呼叫暫時改四個主機端參數位元組。
GPU程式、其他參數、所有指標及啟動形狀保持不變；GPU完成後原地還原參數，不讓參數指標因重新配置失效。

每個控制值都會做：

- 原版重複執行，確認輸出與中間張量指紋可重現。
- 兩種輸出初始填充值，檢查觀察範圍是否依賴未覆寫內容。
- Head輸出改成有限FP8交錯值，以及逐位元組翻轉符號，分別執行兩次。確認覆寫讀回正確，檢查差異是否到達 allocation26 和最終輸出，保留完整輸出差異數量及有限值統計。
- 若控制值1產生有限、完整覆寫且對Head敏感的輸出，再比較两個既有r4候選在這個**實驗控制值**下的全部配置內容。這是新增的有條件GPU驗證，不是未測先通過。

最後還原原始零控制值並重新執行完整原版，核對全部配置與模型區域，再取得沿用r6.1方法的逐命令診斷時間。
本版用這組目標明確的實驗取代逐命令大型SHA追蹤及r6.1的單一buffer掃描，不要求你先交r6.1結果才能執行。

## 如何判讀

結果中的 `output_control_experiment` 是新實驗。
`original_control_head_to_output_sensitive` 與 `experimental_control1_head_to_output_sensitive` 必須分開看。
即使非零實驗敏感，**也不會覆寫原始 `negative_control` 的結果、不會放行遊戲發布**。

`zero_control_suppression_hypothesis_supported_by_trials=true` 只表示本次既定試驗支持零控制值抑制差異的假說。
這仍不能證明1就是遊戲應用值、完整前處理與歷史狀態正確、畫質正確，或可以省略／刪除任何模型運算。

`r62_diagnostics_complete_original_gate_closed` 表示診斷已完成，但原始輸出敏感性門檻仍關閉；不是要求重裝HIP。
`instrumented_profile` 保留r6.1的原始時間／零值未解析處理；不把單層插樁時間直接換算成遊戲FPS。
`failed` 請回傳JSON/TXT；不要混用不同版本EXE。

## 本版的邊界

沒有新的GPU核心優化、不宣稱更高FPS、不宣稱追上NVIDIA。
保留同一r4 GPU code SHA256：`62c8ecf66e4290ffbe22230a376b475ef482807fe05c46b957457bff5826cffe`。
本輪Linux/Windows測試範圍以 `BUILD_STATUS.json` 及 `reports/` 為準；CI沒有9070XT，也沒有實際跑完整checkpoint。
測試報告只保留雜湊、計數、控制值和時間，不匯出原模型權重、完整張量或遊戲圖片。

主機端完整Forward綁定、真實影格、時序/HDR畫質、主要Swin核心重寫及遊戲DLL仍待後續驗證。
`source/` 有实际編譯的程式、累積補丁、原始碼及授權；`SHA256SUMS.txt` 可驗證檔案。
開發分支 `dlssnr-rdna4-r62-output-control`；main與舊測試分支不被覆蓋。
