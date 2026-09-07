# 原模型接入檢查：尚非新的 DLSS-NR 後端

這個分支以 r2 commit `cf7ad8cd930c70cad0e81474cc5233aa1615d07b` 為基底。
新增的是唯讀呼叫／權重對照工具，**沒有新的 GPU 核心、遊戲 DLL 或加速版完整模型，也沒有新的 GPU 執行結果**。main、r1、r2 未被合併或覆蓋。

## 已對照的邊界

來源是先前保存的 `DLSSNR-rewrite-source-checkpoint/plans/1080p.json`，而非根據 M/N/K 人工猜測網路結構。
計畫 SHA256：`0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93`。
模型權重 SHA256：`6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab`。

此固定合成 Forward 計畫具有 44 個配置、154 個 GPU 核心呼叫及 4 個裝置內複製；其中 46 次為 `k_swin_var`，使用 168-byte by-value 參數。
原權重檔有 153 個命名資料塊。按原 runtime 的排序和對齊規則重建 upload 位址後，46 次 Swin 呼叫在參數 offset 16 的權重參照皆對應到一個命名資料塊起點。

例如，去除 stage 標記後的 command 0 對應 `block0.layer0.layer`；command 1 對應 `block1.layer0.layer`；command 157 對應 `block70.layer0.layer`。
以上是呼叫／資料塊對照，**不是資料塊內部的矩陣解碼，也不是模型畫質正確性的證明**。

輸出中的其他欄位維持 `u32_at24`、`i32_at32_36` 等 offset 名稱。不能只憑看起來像寬高、位移或 flags，就把所有語意當成已驗證。

## 為何不能直接插入 r2 的矩陣算子

r2 的輸入是明確定義的合成矩陣與 E4M3FN 位元組排列；原 Swin 的參數、內部資料配置、每層縮放及輸出格式仍須逐項確認。
相同的 128/256 通道數並不能證明它與某個 r2 M/N/K 測例是同一個運算。原版也已經使用 gfx1201 FP8 WMMA，不能把「啟用 FP8」再計為新增收益。

採用候選核心前的必要驗證：同一份真實的層輸入、同一資料塊解碼、相同運算與量化規則下，與原版 GPU 核心差分比較；再放入原 Forward 檢查總時間，最後才是遊戲影格與時序品質。
不能拿兩個獨立矩陣算子的速度比，直接乘上整個 Forward 或遊戲 FPS。

## 工具（開發用途，非玩家必要步驟）

`audit_contract.py` 只讀取計畫和權重，先驗證 SHA256，檢查所有 launch relocation 與 copy bounds，再輸出不含權重內容的 JSON 對照表。
它不載入或執行 DLL、不接觸遊戲程序、不更動模型檔，也不移除現有 runtime 的雜湊檢查。

```text
python audit_contract.py --plan <checkpoint 的 plans/1080p.json> --weights <現有 dlssnr_on_amd_weights.bin> --out swin-contract.json
```

輸出檔若已存在會拒絕覆寫。原始 checkpoint 和權重不隨此分支重新散布。
本分支不提供 r3 測試 EXE，也不需要玩家重新跑 r2。這是原模型接入前的證據整理，不是假裝完成的神經網路重寫。
