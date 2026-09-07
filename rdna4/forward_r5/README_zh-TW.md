# RDNA4 r5 — 固定完整呼叫計畫中的 Head 替換對照

**不是遊戲 DLL；不是完整 Swin 重寫；不是已證明遊戲效能提升。**
這次將 r4 的 `head_direct16_w4`、`head_direct32_w4` 放進先前保存的固定 1080p 呼叫計畫，由同一份計畫執行原版／替換版，檢查 Head 的前後張量與所有配置區域，再量測整段 GPU 流程。
除了可選的 Head 替換，其他核心仍是原版。計畫必須符合固定 SHA256；不會猜測不同版本的參數或修改原版 DLL。

## 必要檔案與下載內容

本 ZIP 已包含 Windows EXE、r4 GPU code object、原始碼、CPU 測試／編譯報告及第三方授權。
**本包沒有包含原始 checkpoint ZIP、原版 DLL、模型權重或 HIP 驅動。**
還需要你已有的：

- `DLSSNR-rewrite-source-checkpoint.zip`，或其中的 `plans/1080p.json`。
- `dlssnr_amd_pass1.dll`，或包含相同原始 gfx1201 code object 的 `version.dll`。
- `dlssnr_on_amd_weights.bin`。

checkpoint 不是 r1／r2／r4 核心測試 ZIP。這是之前保存的完整 Forward 原始碼／計畫 checkpoint，約 247 KB。
程式可以直接從 checkpoint ZIP 讀取 `1080p.json`，**不用解壓它、不用 Python**。也可直接選已解壓的 JSON；雜湊必須相同。

## 執行

1. 把本測試包完整解壓到新的獨立資料夾，不要放進遊戲。
2. 關閉遊戲與其他 GPU 高負載程式，維持平常穩定的顯卡設定。
3. 雙擊 `TEST_R5.cmd`。
4. 第一個視窗選原始 checkpoint ZIP 或 `1080p.json`。
5. 第二個視窗選原版 `dlssnr_amd_pass1.dll`／`version.dll`。
6. 第三個視窗選原本的 `dlssnr_on_amd_weights.bin`。

同名檔案若已在 EXE 同一資料夾，會先使用它而省略對應視窗。不要為了通過檢查把不相符檔案改名。
不需要 Python、Visual Studio、管理員權限或新增遊戲設定。仍需要本機 AMD HIP 7；GPU 清單自動尋找 9070 系列，不會選 6600 XT。
計畫配置約 806.5 MB GPU 記憶體，另有驅動／保護區成本；系統 RAM 也會保存原版結果作逐 byte 比較。

```bat
rdna4-r5-test.exe --plan "D:\Tests\DLSSNR-rewrite-source-checkpoint.zip" --runtime "D:\Game\dlssnr_amd_pass1.dll" --weights "D:\Game\dlssnr_on_amd_weights.bin" --output rdna4-r5-result.json
```

結束或失敗後，保留 `rdna4-r5-result.json` 與 `rdna4-r5-console.txt`。JSON 包含正規化的原始呼叫計畫，方便檢查參數，不含模型權重 bytes、即時遊戲畫面或所選檔案完整路徑。

## 本輪驗證的確切範圍

固定計畫應有 44 個配置、154 次 GPU 核心呼叫、4 次裝置內複製，以及一次 Head。
先依原模型資料塊的名稱排序與對齊規則建立完整權重 upload，核對 Head 的指標是否指向命名資料塊起點及大小是否足夠。

**這是保存下來的呼叫計畫中的綁定，不是對正在執行的遊戲或原版主機端程式做即時攔截。**
`head_weight_binding_in_recovered_plan_verified` 與 `live_runtime_weight_binding_verified` 是不同欄位，後者仍為 false。

在每個合成案例中，先執行原版完整計畫，取得前面各原版核心真正產生的 Head 輸入，再執行兩種替換版。
檢查 Head 輸入、Head 輸出與所有配置的最終 bytes；模型區域也必須保持原 upload bytes，所有配置前後的保護區都要完整。
這次不是只比較單層，也不是只比較幾千個抽樣點或單一最終 hash。若有不一致，就記錄第一個位置並停止，不會啟用到遊戲。

## 合成初始化不是遊戲前處理

每次執行前，非權重配置歸零，第一個核心的 input 指標所在剩餘區域分別填入全零、固定有限 float32 bit pattern、變動有限 float32 bit pattern。
這些是供差分測試的明確初始化條件，**不是已重建的 D3D12 輸入、時序 history、曝光、motion／depth 或遊戲前處理**。
沒有任意修改保存計畫中的縮放／混合 scalar 參數，沒有宣稱已完成它們的語意還原。也沒有聲稱所有混合格式緩衝區都能統一解讀為 FP32 RGB。
因此本輪時間不能直接與先前的 23.586 ms、r4 的單層時間或實際遊戲 FPS 當成同一測量相比。

## 防止最終輸出掩蓋錯誤

反向測試只在獨立測試的 GPU 記憶體中，將 Head 輸出暫時替換成有限 FP8 模式，再執行後續原版命令。
觀察最後一次核心之參數 offset 8 指標所指的配置區域是否改變；這個觀察區域**尚未被本工具證明就是最終遊戲 RGB**。

若沒有改變，報告會標示 `output_sensitivity_unconfirmed`，提醒最終觀察可能掩蓋 Head 錯誤，例如後續混合或未解讀參數。即使位元組對照全部相同，也不應視為遊戲畫質已驗證。
反向案例不參與效能計時，不會寫入模型檔，也不會成為採用中的核心。

## 整段時間如何測量

每個 HIP graph 包含一次完整計畫：154 個原核心／替換核心呼叫及 4 個原版裝置內複製。
每次樣本先重設合成狀態，再以 GPU event 量測一次 graph；狀態重設、CPU 準備、讀檔及輸入上傳不計入時間。
三種模式暖身後，交錯量測前段 15 輪及後段 7 輪，保留全部樣本，不挑最快的一筆。
計時前、計時後都重新驗證 graph 輸出與原版 direct 執行結果一致。

這是固定合成核心流程，沒有遊戲 hook、D3D12/HIP 接合或 display export，也沒有鎖定 GPU 時脈或測量功耗狀態。Head 只占完整計畫的一部分，即使 Head 大幅加速，整體差距仍可能很小或被量測波動淹沒。
程式不自動選擇或部署任何核心，也不修改遊戲設定。

## 狀態解讀

`cpu_tests_passed_original_plan_and_gpu_not_tested`：只通過 CPU 解析／ZIP／Head 參考測試。

`fixed_synthetic_full_plan_passed_not_game_validation`：本次固定合成計畫差分通過，反向測試觀察到指定區域改變；仍不是遊戲或畫質驗證。

`fixed_synthetic_full_plan_equal_but_output_sensitivity_unconfirmed`：差分相同，但反向測試未確認最後觀察區域的敏感性。

`failed`：保留 JSON/TXT，不要覆蓋遊戲 DLL 或刪除原權重。

## CI 已做與未做

Linux 交叉編譯 unchanged r4 Head，檢查 GPU ISA／ABI；Linux 與 Windows 執行 CPU 支援測試，包括刻意損壞的計畫與 ZIP CRC，以及 stored／deflate ZIP。
**建置環境沒有讀到真正的原 checkpoint 計畫，也沒有 RX9070XT，所以本包發佈前沒有在 CI 實跑完整 Forward。**
`BUILD_STATUS.json` 和 `reports/` 保留這些邊界，不把測試 fixture 當成原模型驗證。

## 授權與重現

獨立程式碼採 MIT。JSON 解析使用 Niels Lohmann 的 nlohmann/json 3.12.0，commit `55f93686c01528224f448c19128836e7df245f72`。
ZIP raw-deflate 使用 Mark Adler 的 puff 2.3，取自 zlib v1.3.1；原始授權與作者聲明保留在 `Licenses/` 及 source/deps。沒有聲稱這些第三方程式是本專案自行撰寫。

checkpoint SHA256：`0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93`。
原 GPU code SHA256：`dd38e6ede167c7a5886ae5d079022f63065b6775384d7588f185036b55877afb`。
模型 SHA256：`6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab`。

開發分支 `x9981727/OptiScaler-MultiGPU:dlssnr-rdna4-forward-r5`；r4 與 main 不被覆蓋或合併。
