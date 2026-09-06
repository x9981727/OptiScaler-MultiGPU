OptiScaler MultiGPU v13 雙卡 XeFG 診斷版

這是加入跨卡 GPU 與 CPU 分段量測的診斷版，尚未宣稱解決效能下降。
完整關閉遊戲並備份舊的 dxgi.dll 後，再替換為此檔。
保留目前的 OptiScaler.ini；[Log] LogLevel 使用 2。
確認仍由 9070 XT 渲染、6600 XT 補幀。
同一場景關閉補幀 10 秒、啟用 XeFG 15 秒、再關閉 10 秒。
回傳新的 OptiScaler.log 與 OptiScaler.ini。
此版每秒會輸出 MultiGPU v13 timing:，並在初始化記錄顯卡與螢幕輸出的 LUID。
不需要重新做單卡測試。
來源及測試範圍見 README-v13.txt 與該次 Windows CI 紀錄。
