OptiScaler MultiGPU v10：XeFG 顯卡切換修正

這個小包只更新以 dxgi.dll 載入的 OptiScaler 核心。
你這次 wwm.exe 日誌確認使用 dxgi.dll，適用此更新包。
保留現有 OptiScaler.ini、XeFG / FSRFG 執行庫與其他設定。

操作：
1. 完全關閉遊戲。
2. 在 wwm.exe 同一個資料夾，先把現有 dxgi.dll 備份到其他資料夾。
3. 將本包的 dxgi.dll 放入該資料夾，取代原本的 OptiScaler dxgi.dll。
4. 開啟遊戲，按 Insert。FG Output 選 XeFG；FG Input 沿用單卡測試正常的設定。
5. 勾選 Use a separate GPU for Frame Generation，FG GPU 選 RX 6600 XT。
6. 按 Save Settings，完全退出遊戲，再重新啟動。
7. 確認介面顯示 XeFG GPU this launch: AMD Radeon RX 6600 XT，才開啟 Active。

修改顯卡後，Active 暫時無法開啟是預期的防護，請先儲存並重啟。
若原本已開啟 Active，仍可關閉；本次啟動會保留原本的 XeFG 顯卡。
只回到主選單或重新讀取存檔，不等於完全重新啟動遊戲。

本版修正「舊主卡 XeFG 交換鏈混用新副卡資源」，不是硬體實測保證。
若重啟後仍卡住，請傳回該次 OptiScaler.log，並告知是在主選單還是實際遊玩時啟用。
不要在卡住後反覆啟動遊戲，以免覆蓋該次日誌。

新安裝、不同載入方式，或缺少執行庫時，請使用完整 v10 安裝包。
不要將本包 dxgi.dll 加到已有另一個 OptiScaler 載入檔的遊戲中。
完整原始碼與可重建流程：https://github.com/x9981727/OptiScaler-MultiGPU
