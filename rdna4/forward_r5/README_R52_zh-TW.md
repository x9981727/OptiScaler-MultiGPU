# RDNA4 r5.2 — 首尾特殊 Swin 欄位解析修正版

這是 r5.1 的 **Windows x64 執行檔修正版**，不是新的遊戲 DLL。
本次修復 `Required original pointer relocation absent`。這個錯誤發生於 CPU 計畫解析階段，還沒有初始化 GPU；它不是本次顯卡運算測試失敗，也不是 checkpoint 下載損壞。

## 如何執行

完整解壓到新的獨立資料夾，例如 `RDNA4-r52`。不要混用舊 EXE，也不要放進遊戲資料夾。
關閉遊戲，雙擊 **`TEST_R5.cmd`**。主控台第一行應顯示 **r5.2**。

依序選擇：
1. 你剛才已成功選取的 `DLSSNR-rewrite-source-checkpoint.zip`，或其中的 `plans/1080p.json`。
2. 既有的 `dlssnr_amd_pass1.dll`，或含同版 GPU 程式的原版 `version.dll`。
3. 既有的 `dlssnr_on_amd_weights.bin`。

**不用重新下載 checkpoint、重跑 r4、安裝 Python、重裝 HIP 或搬動大型權重檔。**
仍需你既有的 AMD HIP 7，程式會選擇名稱包含 9070 的 GPU，而非 6600 XT。
原版 DLL 和權重唯讀，不修改遊戲設定、登錄、時脈或電壓。

執行後回傳 `rdna4-r5-result.json` 與 `rdna4-r5-console.txt`。失敗時也保留兩份。

## 修正的真正原因

r5.1 錯把整段計畫的首尾當成一般內部 Swin：第一個輸入固定取參數 offset 0，最後觀察區域固定取 offset 8。

你的回傳資料顯示，首尾皆是特殊的 `_Z10k_swin_varILi32ELb1EEv9VarParams`：
- 第一個呼叫的 offset 0 原本就是 null；本次合成初始化所用的邊界欄位為 offset 64，指向 allocation 1。
- 最後一個呼叫的 offset 8 原本也是 null；最後觀察區域改取 offset 112，指向 allocation 2。
- Head 仍保留原本的 24-byte by-value 結構，實際為 40 組，權重 upload offset 22,493,628。

r5.2 只修正 CPU 解析與邊界選擇，**不向原始 null 欄位硬塞指標、不更改原始參數 bytes、不停用 SHA256 或記憶體範圍檢查**。
特殊欄位的 kernel 名稱、168-byte 參數長度、尺寸、模式值、grid/block、完整指標位置及配置大小均納入核對。
一般 0/8 形式僅保留給 CPU 測試 fixture；正式執行仍限定已知的固定原始計畫。

## 額外功能

錯誤訊息現在會列出 kernel 名稱、要求的參數 offset，以及實際可用的 offsets。
新增 `CHECK_PLAN_ONLY.cmd`，只讀取和驗證真正的 checkpoint，不載入模型或初始化 GPU。正常測試直接使用 `TEST_R5.cmd` 即可，不必先跑另一個程式。

對應命令：
```bat
rdna4-r5-test.exe --validate-plan-only --plan "D:\Tests\DLSSNR-rewrite-source-checkpoint.zip" --output rdna4-plan-check.json
```
只有選取的完整 checkpoint 真正通過時，才會記錄 `original_plan_cpu_validation_passed_gpu_not_run`。這不是 GPU 通過狀態。

## 已驗證與未驗證

以 `BUILD_STATUS.json` 和 `reports/` 的實際建置紀錄為準。
本版的回歸測試使用此次錯誤報告中的 **第一個、Head、最後一個共三筆呼叫**及44個配置尺寸：舊解析器必須重現相同錯誤，新解析器必須正確選到 64/112，並拒絕刻意移除指標、改動 null、錯誤尺寸等案例。

這個三筆呼叫 fixture **不是完整154次GPU呼叫計畫**。它無法通過正式原始計畫的 SHA256／呼叫數檢查，也不可以拿 `reports/reported-boundary-fixture.json` 當作 checkpoint。
Linux 使用 AddressSanitizer／UndefinedBehaviorSanitizer 檢查 CPU 回歸；Windows 執行同一套邊界回歸、ZIP及負向雜湊測試。`ci-rejected-*` 報告中的 failed 是預期的拒絕測試，不是你本機的結果。

**建置環境没有執行完整原始 checkpoint，也沒有 RX9070XT GPU 測試。** 本包修復已重現的解析錯誤，不代表完整 Forward 已在 CI 跑通；沒有新增GPU效能或遊戲FPS數字。

本版 GPU binary 強制保持與 r4 實測相同 SHA256：
`62c8ecf66e4290ffbe22230a376b475ef482807fe05c46b957457bff5826cffe`

正式原始 `1080p.json` SHA256 仍為：
`0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93`
這是解壓後 JSON 的雜湊，不是 ZIP 外層雜湊。

## 完整比較的範圍沒有變成遊戲驗證

讀取原始計畫後，程式執行原版及兩種 Head 替換版，其他核心仍為原版。每個案例重設至相同的明確合成狀態，檢查 Head 前後張量、模型、保護區及全部配置的最終 bytes。
這些輸入 bit pattern 不是即時遊戲圖像、前處理或歷史狀態重建。
整段計時包含154次核心呼叫與4次裝置內複製，但排除狀態重設、CPU準備、上傳、遊戲hook及D3D12/HIP接合。

後段觀察 offset112 的區域會另作反向敏感性測試；仍不以「最終觀察區域相同」冒充遊戲畫質验证。完整Swin重寫、時序品質、HDR與遊戲DLL尚未完成。不要用本EXE或GPU程式檔覆蓋遊戲DLL。

## 來源與重現

開發分支：`x9981727/OptiScaler-MultiGPU:dlssnr-rdna4-r52-boundary-fix`。
main、r4、原r5與r5.1分支不被本輪覆蓋。
`source/` 包含實際編譯的修正版、原始解析器備份、固定來源補丁腳本及第三方依賴授權；`SHA256SUMS.txt` 包含套件檔案雜湊。
從此分支乾淨 checkout 執行 `.github/workflows/rdna4-r52-fix.yml` 可重現建置。`prepare_r52.py` 只接受對應原始 parser Git blob；不要重複對已修正的 header 套補丁。
