# RDNA4 r6.1 — 計時修正與最後輸出邊界隔離

本包是 Windows x64 可執行測試版，不是遊戲加速 DLL。由使用者在 RX9070XT 實機執行；開發者負責修改、編譯及提供 ZIP。不要覆蓋遊戲內任何檔案。

## 執行

將完整 ZIP 解壓到新的可寫入資料夾，關閉遊戲後，雙擊 `RUN_AUTOLAB.cmd`。
依序選取已經使用過的 checkpoint ZIP、原版 `dlssnr_amd_pass1.dll`／`version.dll`、`dlssnr_on_amd_weights.bin`。
不必重新下載 checkpoint、不必搬移大型權重、不需要 Python、Visual Studio 或系統管理員權限。沿用既有 HIP 7；程式自動尋找 9070 顯卡，不會選 6600 XT 執行 gfx1201 核心。

結果改用新檔名，以免覆蓋 r6 舊紀錄：

- `rdna4-r61-result.json`
- `rdna4-r61-console.txt`

成功或失敗都保留這兩個檔案。原版 DLL、模型檔及 checkpoint 只讀；本包不包含它們，也不安裝遠端控制服務。

## 這輪根據什麼修改

使用者 r6.0 紀錄顯示三組初始化、原版與兩個 Head 替換版的 byte comparison 均通過，因果追蹤也已完成。失敗是在後面的 `Invalid instrumented timestamp`；最後提交命令為 101 / `k_repack`。
舊程式要求每一筆時間都必須是有限且嚴格大於零，卻沒有保存被拒絕的數值。**因此目前不能斷言實際那筆一定是零、負數或非有限值，更不能據此說顯卡壞掉或遊戲崩潰。**

r6 的有效追蹤也顯示：命令 156 後的 allocation26 受到 Head 改動影響，但命令157後 allocation2仍與原版相同。這把下一個調查點縮小到最後的 special Swin 與其不同資料來源；它還不能證明哪個參數或指標有錯。

## 計時修正

每個命令邊界使用不同 HIP event，先將整段排入同一條 stream，完成後才同步並讀回，不在每一個命令之間插入主機等待。
先暖身一次，再進行七次完整原版計畫測量；每次開始都重設相同狀態。**不會為了取得較漂亮的時間而在原地連續重跑有狀態的單一 kernel。**

所有原始時間、IEEE754 bits、HIP 回傳碼、命令編號及樣本分類會保存。
零值保留為 `zero_unresolved_not_zero_cost`，不當成零成本、不換成 epsilon、不偷偷丟掉。任何包含零值的命令不提供嚴格中位數，也不把它排入最快／最慢的完整量測排行；family 統計會標示未解析覆蓋範圍。
負數、NaN、無限值或 HIP API 錯誤仍會保存具體資訊並中止。

這是有插入 event 的診斷時間，仍會改變排程；不能直接與無插樁 graph 時間相除當成精確瓶頸百分比，也不能與 r6 舊同步式 profile 不加區別地比較。

## 新增最後輸出邊界測試

只使用原版最後一個 GPU 核心，保持全部 168-byte 參數、指標、旗標、控制數值、grid/block 不變。
每次試驗都先從相同初始化重跑完整原版前段，然後一次只暫時改動一個既有 buffer：

| 最後呼叫參數位置 | 配置 | 試驗值 |
|---|---:|---|
| offset152 | 26 | 全零及有限 FP8 位元組圖樣 |
| offset0 | 5 | 全零及有限 FP8 位元組圖樣 |
| offset120 | 1 | 全零及有限 float32 圖樣 |

這些圖樣是刻意的探測输入，不代表重建了真實遊戲影格或模型輸入語意。
每個案例確認 GPU buffer 確實已被覆寫、與原 buffer 的差異數量，再執行原版最後一層，比較全部輸出位元組；每個案例重複兩次，檢查可重現性。也用兩種不同輸出初始填充值，檢查觀察區域是否依賴未寫入部分。
會保存輸出 SHA256、差異位元組數、第一個差異位置與診斷用 float32 統計。**不匯出模型權重、完整張量或影像。**
所有探測結束後重跑原版整段，檢查完整配置內容已恢復到基準。這些探測不能替代原始 Head→最終輸出的敏感性測試，也不會把失敗的發布門檻改為通過。

## 結果解讀

`autolab_blocked_output_sensitivity` 可以表示這次診斷已完整跑完，但原始敏感性仍未通過；此時請看 `boundary_probe` 與 `instrumented_profile`，不是重裝 HIP。
`instrumented_profile.status=completed_with_unresolved_zero_intervals` 表示保留了未解析的零值，其他測量及診斷仍可讀；不等於那些命令無成本。
`failed` 請保留錯誤與原始時間欄位。**不要只看行程退出碼就認定可以發布遊戲 DLL。**
`release_gate.approved` 仍為 false。即使某個邊界探測有效，也沒有完成完整 Swin 重寫、遊戲接合、歷史狀態、HDR 或最終畫質驗證。

## 編譯與驗證

檢查 `BUILD_STATUS.json`、`reports/` 和 `SHA256SUMS.txt`。新增的計時分類、零值／负數／NaN／無限值與探測圖樣已設計為可在 CPU 上回歸測試。
故障注入測試會人工在命令101放入零樣本，測試它不會被當成零成本或導致錯誤排名；**這是合成回歸案例，不是宣稱已重現你原本那筆未保存的實際時間值。**
GPU 核心保持你 r4 測通的 SHA256：
`62c8ecf66e4290ffbe22230a376b475ef482807fe05c46b957457bff5826cffe`
沒有本次實機結果前，不能宣稱新的 GPU 正確性、整体速度或遊戲 FPS 提升。

來源分支：`dlssnr-rdna4-r61-timing-boundary`。main 與舊測試分支不被覆蓋。
`source/` 保留實際編譯的 `forward_r61.cpp`、原始累積補丁及所有需要的頭檔；第三方授權在 `Licenses/`。

HIP event 參考：
https://rocmdocs.amd.com/projects/HIP/en/develop/reference/hip_runtime_api/modules/event_management.html
