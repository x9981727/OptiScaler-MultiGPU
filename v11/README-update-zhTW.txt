OptiScaler MultiGPU v11：遊戲啟動時切換解析度的修正

本小包包含已編譯的 dxgi.dll，適用你目前 wwm.exe 的 OptiScaler 安裝方式。
修正 ResizeBuffers 的重複鎖定、等待兩張卡完成工作，以及畫面緩衝區重建。
保留 v10 的顯卡切換防護與既有 FSRFG / XeFG 雙卡功能。

1. 完全關閉遊戲。
2. 把 wwm.exe 同一資料夾的原 OptiScaler dxgi.dll 備份到其他資料夾。
3. 放入本包的 dxgi.dll，取代原檔；保留目前 OptiScaler.ini 與執行庫。
4. 沿用 XeFG、6600 XT 與單卡成功時的 FG Input。
5. 第一次先保持 Active 關閉，確認能以原本解析度進入主選單與實際遊玩。
6. 若正常，再開啟 Active 測試副卡補幀。

若 INI 之前已儲存 Active=true，可在啟動前將 [FrameGen] 區段的 Enabled 改成 false。
不要改到 [MultiGPU] 的 Enabled；此次需要維持副卡設定來驗證啟動問題。
若要改換顯卡，仍需 Save Settings、完全退出並重新啟動。

新日誌應出現：
  MultiGPU v11: draining render and FG queues before swapchain resize
  MultiGPU v11: both queues idle; entering SDK resize with virtual backbuffers retained
  MultiGPU v11: entered FG resize hook 6677 with thread-scoped lock
  MultiGPU v11: ResizeBuffers completed and virtual backbuffers rebuilt

若仍崩潰或停住，請直接保存該次 OptiScaler.log，告知是在啟動、切換解析度、
載入遊玩場景，還是開啟 Active 時發生。不要反覆啟動而覆蓋失敗時的日誌。
本版通過編譯與回歸測試不代表已完成 9070 XT + 6600 XT 的實機驗證。

新安裝或缺少元件請用完整 v11 包。不同載入方式的遊戲不要額外安裝第二份核心。
原始碼與可重建流程：https://github.com/x9981727/OptiScaler-MultiGPU
