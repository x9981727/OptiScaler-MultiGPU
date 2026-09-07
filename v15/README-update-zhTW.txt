OptiScaler MultiGPU v15 更新版

關閉遊戲並備份 dxgi.dll、OptiScaler.ini，再以新版 dxgi.dll 覆蓋舊核心。
完整包另附全套執行元件及安裝說明。
保留目前的顯卡設定，在既有 [FrameGen] 區段將 FTInput 設為 auto 或 2，
以視窗／無邊框模式啟動，測試副卡 XeFG。
不要使用舊開機階段的顯卡 LUID；以目前遊戲選單的 6600 XT 選擇為準。

v15 修正 FTInput 設定儲存後沒有正確讀回的問題，並在符合條件的副卡
背景呈現中省略 SDK 可選的幀時間估值。效能改善幅度仍待實機測試。
可將 [FrameGen] FTInput 改成 0 並重啟，恢復原本的輸入幀時間來源。
XeLL 與 GPU 同步保持啟用。FSRFG 保留既有流程。
回傳測試後的新日誌與目前的 INI。
