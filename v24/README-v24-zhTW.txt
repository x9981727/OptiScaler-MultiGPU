OptiScaler MultiGPU v24 — 保留原始幀的 XeFG 2x 測試版

唯一目標：在相同場景與畫質下，RX 9070 XT 維持約 67 FPS，RX 6600 XT 每張來源畫面插入一張，實際呈現接近 134 FPS。
這是驗收目標，不是已完成的實機效能保證。

改動
1. 全解析度 Color 從原本 FG DIRECT 佇列的跨卡匯入，改為獨立 COPY 佇列預先搬入既有 staging texture。
2. 前一個 XeFG Present 確實完成後，才取得新的 destination backbuffer index，再將 staging texture 原樣複製到 SDK backbuffer。
3. COPY、DIRECT 各自使用 fence 保護槽位。沒有跳過來源幀，沒有提前推進 cursor，沒有無限制擴張佇列。
4. 不更動 FSR 4、輸入/輸出解析度、HDR、Depth/MV、FT Input、補幀倍數或 XeLL 設定。
5. COPY 初始化不可用時使用舊傳輸；提交後發生錯誤不偽造 Present 成功。

限制
COPY 與計算能重疊多少取決於驅動與硬體，新增的本地 copy 也有成本。編譯或 WARP 測試成功不等於已達 134 FPS。
舊 fgWaitImportGPU 在此版只涵蓋最終 DIRECT handoff，不能將它降低當成總傳輸時間已降低的證據。
SDKqueued 與 queuedMultiplier 是 SDK 排程計數，不是螢幕掃描輸出。實際 Displayed FPS、丟幀與延遲需要額外呈現追蹤。

安裝
完整關閉遊戲，備份原 dxgi.dll，只用本包 dxgi.dll 覆蓋。
不要把整份 OptiScaler.ini 換掉，保留 GPU LUID 與所有現有畫質設定。

既有設定保持：
[FrameGen]
FTInput=0
[XeFG]
AsyncPresent=true

本版新增（預設 true；可在既有 [XeFG] 區段加入）：
AsyncColorTransfer=true

A/B 驗證
使用同一個 v24 DLL，只切換 AsyncColorTransfer=true / false，每次完整重開遊戲。
兩次在同一場景、同一視角，等待資源載入完成，先關閉 XeFG 10 秒，再啟用並量測 60 秒。
不得同時切換升頻倍率、画質或鎖幀。保留每次完整 OptiScaler.log。
false 回到舊的 Color 傳輸方式，但仍保留目的 backbuffer 索引修正。

啟用紀錄
MultiGPU v24: dedicated COPY queue ready
MultiGPU v24 color: prefetch=true
MultiGPU v24 exact2x: SDKsources=... SDKqueued=... queuedMultiplier=...
sourceShedding=disabled 是此傳輸策略的狀態，不代表實際顯示端一定沒有丟幀。

成功條件
不能只把 60 -> 120 稱為達成 67 -> 134。
應同時接近同場景未補幀的來源速率、每個來源幀約 1 張新增畫面、實際顯示接近 2 倍，而且延遲不持續累積。
請保留順暢度、快速轉視角/UI 穩定度及延遲觀察；FPS 較高但明顯卡頓不是成功。

來源
分支 fix/v24-xefg-exact-2x。
解碼後 unified diff 位於 v24/exact-2x.patch，SHA256：
38becc44e0545b0fa467fb9fbd04bf9316be112ac2e1c9f9d5fcd439b06b8d43
BUILD-MANIFEST.txt 記錄編譯所用 commit 及 DLL SHA256。
實驗性遊戲模組：僅於允許使用模組的離線環境測試，不用於多人/反作弊環境。
