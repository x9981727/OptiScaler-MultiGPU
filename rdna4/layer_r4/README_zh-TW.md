# RDNA4 r4 — 原版內部 Head 層／重寫核心差分測試

**這不是可安裝到遊戲的 DLL，也不是完整 Swin 或 DLSS-NR 重寫完成版。**
這次不再只比較自行定義的 r1/r2 GEMM。已根據固定原版 GPU 程式的介面、位元組排列及數值轉換，實作三個 `k_final_head` 內部投影層候選核心，並提供直接執行「原版 GPU 核心 vs 新核心」的 Windows 測試 EXE。

CI 已做的驗證以 `BUILD_STATUS.json` 和 `reports/` 為準。CI 沒有 RX9070XT，**不能宣稱新核心已在 GPU 上正確、比較更快，或已改善遊戲 FPS**。

## 一次執行

1. 把完整 ZIP 解壓到新的、可寫入的獨立資料夾，不要解壓到遊戲資料夾，也不要混用 r1/r2 檔案。
2. 關閉遊戲及其他 GPU 高負載程式，保留平常穩定的顯卡設定。
3. 雙擊 `TEST_R4.cmd`。
4. 第一個選檔視窗選你既有的 **`dlssnr_amd_pass1.dll`**，或原版 **`version.dll`**。
5. 第二個選檔視窗選既有的 **`dlssnr_on_amd_weights.bin`**。不必複製或移動這個大檔案。
6. 程式結束後，保留 `rdna4-r4-result.json`、`rdna4-r4-console.txt`。失敗時也保留這兩份。

如檔案恰在測試 EXE 同一資料夾，程式會先找到它們而不開對應的選檔視窗。沒有檔案則使用 Windows 選檔視窗，不會掃描你的磁碟或自動下載 DLL／模型。

不需要 Python、Visual Studio 或系統管理員權限。需要可載入的 **AMD HIP 7 / `amdhip64_7.dll`**；會嘗試既有預設載入目錄及 `HIP_PATH/bin`。
自動選擇名稱含 9070 的 GPU，不會誤在你的 6600 XT 執行 gfx1201 核心。

選檔、讀取與執行屬於本地獨立測試。原版 Windows DLL 只被當作位元組讀取，**不會對它呼叫 LoadLibrary、執行其 DllMain、注入遊戲或改寫 DLL**。從中找到的 AMDGPU ELF 必須符合固定 SHA256 才會交給 HIP 執行。
權重檔唯讀，模型的 CPU 檔案及 GPU 權重 buffer 不會被候選核心改寫；測試有檢查 GPU 輸入與權重是否保持相同。

進階命令（裝置編號以程式清單為準）：

```bat
rdna4-r4-test.exe --runtime "D:\Game\dlssnr_amd_pass1.dll" --weights "D:\Game\dlssnr_on_amd_weights.bin" --output rdna4-r4-result.json
```

若出現 hash mismatch，停止使用不符版本的檔案，不要改名騙過檢查，也不要移除雜湊保護。

## r4 實際新增內容

- `head_direct16_w1`：每 wave32 處理一個 16×16 輸出片段，直接讀取原版排列。
- `head_direct16_w4`：每工作群組四個獨立 wave32，每 wave 一個輸出片段。
- `head_direct32_w4`：每 wave 共用輸入片段，計算兩個相鄰的 16×16 輸出片段。
- 三者使用 24-byte by-value 參數：input、output、weights 三個 GPU 指標；呼叫 grid/block 不同，**不是把函式地址寫進原版 DLL 的即插即用補丁**。
- `head_contract.hpp` 提供獨立的張量／權重位址公式、FP16 round-to-nearest-even 及 FP8 參考轉換；CPU 檢查完整位址覆蓋與重複映射。

## 必須保留的原版數值規則

依固定原版反組譯重建的 Head 契約是：每個空間群組包含 16 個 token，512 個輸入通道投影到 1024 個輸出通道；輸入與輸出均為 FP8 的特定 byte-swizzle 排列。

**每 32 個輸入通道**先執行兩個 FP8 WMMA，該區塊用 FP32 計算，再與前一區塊的 FP16 累積值相加並轉回 FP16。不是全部 512 個通道都用 FP32 累積到最後。
最終再依原程式處理飽和值與 FP8 轉換。直接把 r2 的 FP32 輸出 GEMM 換進去，並不能保持這個契約。

這些是由固定版本靜態程式推導、由新程式實作的規則；是否逐位元組符合真實原版 GPU 執行，正是本次測試的驗證目標。
原版 `k_final_head` 名稱中的 final 並不表示本工具已還原最終遊戲 RGB 影格。這是網路內部投影層，亦不是既有 profile 中最主要的 Swin 瓶頸。

## 如何驗證，不靠漂亮的矩陣數字

先驗證原始 code object、新核心與模型檔案雜湊。
測試從真實模型 archive 讀取 `block30.layer4.layer`，保留其原始 524,304 bytes，其中本次矩陣契約存取前 524,288 bytes。它是本次原版與候選共同使用的真實權重資料塊；**該資料塊在完整 Forward 的主機端綁定尚未重新驗證**，報告不會把這件事寫成已完成。

基底向量測試分 32 次，每次 16 個不同輸入通道，覆蓋全部 512 通道。原核心輸出先對上獨立 CPU 權重索引預期，總共檢查 524,288 個係數對應，再檢查三個候選核心是否與原核心逐 byte 相同。若原核心都與此參考不符，工具會停下，說明重建的契約需修正，不會硬算加速結果。

另測全零、帶符號零、稠密合成內部張量及較廣的有限 FP8 數值。每個版本以兩種不同初始填充值執行兩次，確認沒有未覆寫輸出，並比較**全部**輸出 bytes，而不是只抽樣約 4,100 個值；同時檢查 buffer 前後保護區。

這些仍是合成的內部張量，**不是擷取自真實遊戲的層輸入，不構成遊戲画質、時序品質或 HDR 驗證**。測試不匯出模型權重或遊戲畫面到報告，只有 hash、計數、錯誤位置及時間。

## 效能數字的邊界

只有已通過相同案例數值對照後，才量測 17／256 個空間群組的稠密案例，使用同一原版 Head 作基準。
四個版本暖身後交錯量測 15 輪及額外 7 輪樣本；每個樣本包含 16 次相同 Head 的 HIP graph replay，記錄每次平均耗時。

計時不含 CPU 讀檔、上傳、其他層間的資料格式轉換、完整 Forward、D3D12/HIP 遊戲接合及渲染。沒有控制 GPU 時脈或量測功耗狀態。**本工具不會自動選用較快的核心、不會改遊戲設定**；某個 Head 更快不能換算成整體 DLSS-NR 或遊戲的加速倍數。
這個階段主要驗證「原版實際運算能否被正確替換」，不是宣稱小型 Head 已解決 Swin 瓶頸。

## 結果狀態

- `cpu_tests_passed_gpu_not_run`：只有 CPU 契約測試。
- `running_original_head_differential`：尚未完成，不能當成通過。
- `original_head_differential_passed_not_full_model`：本輪原版 Head 與三個候選通過既定差分測試；仍不是完整模型通過。
- `failed`：保留 JSON/TXT；依第一個 mismatch、hash 或 HIP 錯誤修正，不應接入遊戲。

## 尚未完成

完整 Swin 的 QKV／attention／MLP／正規化／模型狀態、完整 Forward 重建與替換、原 Head 資料塊的完整主機端綁定驗證、真實層输入、時間穩定性、遊戲 DLL 與 NVIDIA 原生 DLSS-NR 同條件比較都未完成。
不要刪除或覆蓋你原本遊戲中的 DLL 與權重。本包不重新散布原版 runtime、model weights 或 AMD 驅動。

## 版本與重現

原版 repo commit：`04d8b83f26adcbdd489137ea27a3ce59fc75eab4`。
原版 gfx1201 code SHA256：`dd38e6ede167c7a5886ae5d079022f63065b6775384d7588f185036b55877afb`。
權重檔 SHA256：`6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab`。
新 code hash 由每次建置產生並編入 EXE；見 `reports/compile-report.json`。

開發分支：`x9981727/OptiScaler-MultiGPU:dlssnr-rdna4-layer-r4`。main、r1、r2、r3 不被覆蓋或合併。
source/layer_r4 包含新增程式；source/tests/linear_test.cpp 是重用的 HIP API 載入器。Linux 使用 Clang/LLVM 19 編譯 gfx1201，Windows MSVC x64 `/MT` 編譯EXE。完整建置流程在原 repo `.github/workflows/rdna4-layer-r4.yml`。
