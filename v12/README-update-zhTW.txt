OptiScaler MultiGPU v12：XeFG 傳輸與 FPS 統計修正

修正同一幀匯入資源後，再為了 SDK 標記而等待同一組指令分配器的流程。
XeFG 現在合併匯入與標記、使用獨立緩衝池，並保留舊資源重用與解析度切換的同步。
保留 FSRFG 與 v11 的修正。實際效能仍須在你的遊戲及雙卡上測試。

更新目前 wwm.exe（原本使用 dxgi.dll）：
1. 完全關閉遊戲，將原 dxgi.dll 和 OptiScaler.ini 備份到其他資料夾。
2. 放入本包 dxgi.dll，取代原 OptiScaler 核心。
3. 在原 INI 的 [Log] 區段設定 LogLevel=2，保留其他遊戲與副卡設定。
4. 重啟後確認 6600 XT，先保持 Active 關閉，進入固定遊玩場景再啟用。

新版雙卡 XeFG 顯示：
Render FPS：應用程式呈現的幀率。
XeFG output est.：根據 SDK 實際回報排入顯示的幀數估算，包含生成幀。
XeFG output: --：資料收集中或 SDK 無法回報；不會直接把渲染幀率乘二。
SDK 估算值不等同獨立量測螢幕實際顯示的 FPS。

開啟後記錄 20 秒，保存 OptiScaler.log 並截取新 FPS 顯示。
日誌會定期出現 MultiGPU v12 XeFG perf，可比較 renderFPS、SDKqueuedFPS、
inputBatchCPU、reuseWait 與 interpolation 狀態。使用 Info 即可，不必再開 Trace。
若對比 FSRFG，切換後完全退出再啟動，使用相同解析度與場景。
不要單獨把 HighResMV 強制改成 false；它必須符合遊戲提供的動態向量與深度資料。

原始碼及建置流程：https://github.com/x9981727/OptiScaler-MultiGPU
