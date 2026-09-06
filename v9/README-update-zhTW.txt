OptiScaler MultiGPU v9 ASI 更新版（實驗版本，未簽章）

適用於目前以 OptiScaler.asi 載入的 RDR2 安裝方式。

安裝：
1. 關閉遊戲，將原有 OptiScaler.asi 備份到遊戲資料夾以外。
2. 將本 ZIP 中的 OptiScaler.asi 複製到 RDR2.exe 所在資料夾，覆蓋舊檔。
3. 保留原本 OptiScaler.ini、ASI loader 與所有補幀 DLL。
4. 使用原本 XeFG + 6600 XT 設定重測。新的 OptiScaler.log 應包含 MultiGPU v9。

此次修改：
- 避免副卡裝置建立時覆寫遊戲主卡的裝置指標。
- 隔離 XeFG 初始化期間的 DXGI factory 建立與顯卡列舉。
- 移除 XeFG 初始化期間強制改寫驅動內部顯卡參數的做法。
  選定的副卡裝置與命令佇列仍明確傳入 XeFG。
- 修正初始化錯誤被 bool 回傳值誤判為成功的處理。
- 加入初始化異常紀錄，記錄例外代碼、模組路徑、位址與呼叫堆疊。

如果仍崩潰：
請提供新 OptiScaler.log，以及 RDR2.exe 旁若有生成的
OptiScaler-MultiGPU-exception.txt。
當程式只卡住、直接終止或在不同執行緒故障時，可能不會生成該檔。
紀錄只寫在本機，不會自動上傳。

驗證範圍：Windows MSVC x64 Release 編譯、二進位版本標記檢查、
異常紀錄與例外傳遞測試。尚未在 RX 9070 XT + RX 6600 XT 實機驗證成功。

還原：關閉遊戲，將備份的 OptiScaler.asi 複製回遊戲資料夾。
