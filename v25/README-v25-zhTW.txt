OptiScaler MultiGPU v25 — XeFG 呈現節奏與逐幀輸入修正測試版

目標
同場景、同畫質下，RX 9070 XT 原始幀率接近未補幀基準；RX 6600 XT 插入一張有效生成畫面，且顯示節奏均勻。67 → 134 FPS 是目標，不是此版本已驗證的實機結果。

本次修改
1. 副卡使用自己的 XeLL 消費端時序：Sleep、Simulation、RenderSubmit、Present 標記與 XeFG Present ID 共用同一個遞增 SDK ID。Sleep 在有界背景工作執行，不直接把這個等待加到主卡渲染執行緒。
2. Depth、Motion Vector、相機常數、來源 ID 與對應指令串列建立快照，再由同一個工作提交到 XeFG。CPU 尚未提交的 input slot 與 GPU 尚未完成的 slot 分別受保護。
3. 新模式不再以短 CPU 間隔凍結／壓低 frameRenderTime，也不把外層 Present 的含等待時間反覆送回 SDK。使用 SDK 的 0=未知時間提示，配合實際 XeLL 消費端標記；不是聲稱渲染只需 0 ms。
4. 在副卡模式把遊戲既有的 ForceFGRenderSizeMVs 修正套到 SDK 初始化旗標。這是運動向量的尺寸描述修正，不是降低遊戲／補幀解析度。
5. 相機 near/far 取副本，不在 Present 修改生產端資料；處理有限與無限遠反向深度的投影。
6. GPU 診斷時間戳預設每 16 個來源幀抽樣，降低診斷指令提交負擔；呈現數量統計仍逐幀記錄。
7. 共用 Reflex hook 的例外加上 XeFG 後端限制，避免 XeFG 的 AsyncPresent 選项意外控制 FSRFG 的 Sleep。

保留
v24 全解析度 Color 獨立 COPY 傳輸、原有 Color ring、必要 GPU fence、正確目的 backbuffer 索引、完整 HRESULT 回傳、原始畫面與實際 SDK 提交計數。沒有 v21 的來源丟棄／假 Present 成功策略，沒有降低 FSR4／輸出解析度，也沒有強制鎖 67 FPS。

限制與風險
這是新的實驗性副卡時序整合，不是 Intel 已驗證的跨顯卡模式。consumer latencyScope 只描述補幀消費端，不是遊戲 input-to-photon 延遲，不能拿它宣稱已改善端到端低延遲。
仍使用有界的單一待完成消費工作，並保留三個 Color／四個 input 槽位；不是無限制佇列，也沒有宣稱完成三工作深度的全管線重構。
新增標記、WARP 測試成功、編譯成功不代表 AMD 實機已均勻達到 134 FPS。跨卡傳輸與補幀有實際成本；本版可能改善、無差異或回歸。
缺少必要輸入時會真正呈現原始画面而不沿用錯誤的歷史插幀；這種安全退回會降低瞬時倍數，不能當成完整 2 倍。
雙卡沿用現有 backbuffer 插幀方式，並未新增獨立 HUD/UI 消除殘影後端。UIComposition 請維持原本關閉；有更多生成輪廓問題仍需實機影像對照。
舊 sdkQueueSpanGPU 是抽樣佇列區間，含等待並可能不涵蓋 SDK 內部所有佇列，不是純著色器時間。sleepCalls/reflexSleepCPU 是外部 hook 計數；新 consumer sleepCPU 才是本版工作呼叫的 XeLL 等待。

安裝
1. 完全關閉遊戲，備份目前 dxgi.dll 與 OptiScaler.ini。
2. 只覆蓋本包 dxgi.dll，不更換其他 Intel/AMD DLL，也不整份覆蓋 INI。
3. 原有設定保持：
[FrameGen]
FTInput=0
[XeFG]
AsyncPresent=true
AsyncColorTransfer=true
4. 新增選項預設已啟用，不必再改檔。需要明確指定時，加入既有 [XeFG] 區段：
ConsumerPacing=true
TimingSamplePeriod=16
ConsumerPacing 於 context 建立時讀取，修改後必须完整重開遊戲。
FTInput 在 ConsumerPacing=true 時不再決定 SDK 提示；保留 0 是供退回舊模式使用。
不要降低解析度、增加補幀倍率或同時換其他參數。此測試目標是一張來源配一張生成。

驗證
同一場景、同一視角，分別記錄關閉和開啟 XeFG 的來源 FPS、Displayed FPS、生成畫面的顯示間隔、丟幀和延遲，使用相同錄製方式。
正常應見：
MultiGPU v25: consumer-owned XeLL
MultiGPU v25 inputs: SDKhighResMV=false, forceRenderSizeQuirk=true, consumerPacing=true
MultiGPU v25 consumer: frames=..., sleepCPU=..., missingPackets=..., errors=..., latencyScope=FGconsumer
SDK sources/queued 是提交數，不是實際螢幕掃描。成功條件不只是 2.000 multiplier，更要沒有原始幀長時間停留而生成幀閃過的長短幀交替。
不要把純 CPU present 回傳速度当成主卡真正完成了更多獨特渲染畫面。

退回
發生啟動失敗、明顯掉速、黑畫面或更嚴重殘影，完全關閉遊戲，換回備份的 v24 dxgi.dll。
同一 v25 DLL 的 ConsumerPacing=false 可作程式路徑 A/B，但仍保留 MV 旗標與诊斷抽樣修正，不等於逐位元相同的 v24。

僅在允許模組的測試環境使用，不用於多人／反作弊環境。CI 沒有 RX 9070 XT、RX 6600 XT 或你的遊戲，實際效果待實機驗證。
