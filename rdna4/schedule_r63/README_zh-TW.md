# RDNA4 r6.3 — Swin 硬體排程候選測試

Windows x64 可執行測試包。**不是遊戲 DLL；尚未宣稱加速成功，也不是完整 Swin 數學核心重寫。**
本版接續你實際執行的 r6.2 Output-Control 分支，不混用另一份同名 r6.2 Neural-Contribution。

## 執行

完整解壓到新的、可寫入的獨立資料夾，關閉遊戲後雙擊 **RUN_R63.cmd**。
依序選原本的 checkpoint ZIP、dlssnr_amd_pass1.dll／原版 version.dll、dlssnr_on_amd_weights.bin。
不用重下載 checkpoint，不用搬移權重，不需 Python、Visual Studio 或管理員權限；沿用先前使用的 HIP 7。
只允許名稱含 9070 的顯卡，不在 RX6600XT 上使用這個 GFX12 排程位元。
成功或失敗都保留 **rdna4-r63-result.json** 與 **rdna4-r63-console.txt**。
原 DLL、權重、checkpoint、遊戲設定、驅動設定、時脈、電壓均不修改。
本包不含原模型、原 DLL、checkpoint 或 HIP 安裝程式。

## 這次不再只是檢查同一個零值

你回傳的 r6.2 記錄顯示：最後控制值0時，Head改動不能影響最後觀察輸出；0.125與1時，已能影響。
控制值1下，既有兩個Head候選也通過完整配置內容比對。這些只證明指定合成試驗的結果，不代表1是遊戲預設值，也不證明真實影格／HDR／歷史狀態正確。

r6.3 改為測試真正的 **Swin 排程候選**；所有計時版本使用相同實驗控制值1，而且 **Head全部保留原版**，避免把Head改動與Swin排程效果混在一起。

## 候選究竟改了什麼

對固定 gfx1201 原GPU模組中的五個Swin核心描述子，切換 COMPUTE_PGM_RSRC1 第21位（GFX12 WG_RR_EN）。
LLVM對GFX12的定義：0為較老wave優先、1為round-robin。這個位元在舊GPU有不同語意，不能套用到6600XT。
參考： https://releases.llvm.org/19.1.0/docs/AMDGPUUsage.html#compute-pgm-rsrc1-for-gfx6-gfx12

切換方向以原模組實際值為準，記錄在 reports/scheduling-manifest.json 與執行結果的 scheduler_module.edits；不先假定原版是0。
建置會用llvm-mc gfx1201分別組譯排程0/1，驗證這個位元位置，再解析真正的原版ELF與核心描述子。

**整個候選模組只改五個位元組、每個只切換一個位元。所有可執行指令區、浮點模式、記憶體順序旗標、同步屏障、ABI、權重和模型計算保持原樣。**
原GPU模組由你選擇的原DLL唯讀取得，候選僅建立於測試行程記憶體；本包只攜帶變更規格和實作，不散布一份偷偷改名的原DLL。

三種比較模式：

| 模式 | 實際作用範圍 |
|---|---|
| original_swin_scheduling | 完整原版，所有核心維持原排程。 |
| alternate_special32_scheduling | 只切換3次special32 Swin呼叫；其他核心原樣。 |
| alternate_all_swin_scheduling | 切換5種Swin核心、合計46次呼叫；其他核心原樣。 |

它們都是待驗證候選，可能加速、不變或更慢。程式沒有直接讀回GPU硬體排程暫存器，因此不能把描述子修改本身當成排程硬體效果已被量測證明。

## 驗證與計時

先保留原始零控制值基準及負向試驗，確認本次與r6.2一致；非零試驗不覆寫這個原始門檻。
再在同一控制值1下，用零值、固定及變動三組合成輸入，比對原版／兩個排程候選的完整配置、Head前後資料及保護區。
每個輸入另以0xA5與0x5A初始填值檢查原版最後觀察區域不依賴先前內容。

在變動輸入下，兩種不同Head干預都必須影響原版非零輸出；原版與候選的干預結果各重複驗證，候選必須和原版一致。不能只靠最後畫面不變，錯把未產生網路貢獻的路徑當成正確。

之後捕捉三個完整HIP graph，確認非零控制值及候選核心已被捕捉，並比對全部配置。
計時為 **36輪前段＋30輪保留樣本**；每6輪包含全部六種模式排列，讓各模式出現在各順序位置的次數相同。
每筆樣本均從相同重設狀態開始，完整計入154次核心呼叫與4次GPU內部複製；重設與上傳不計入graph時間。
原始時間、float位元與HIP回傳碼逐輪保存；零值／異常時間不刪除、不補epsilon，不生成虛假的加速結果。

計時後再次比對完整配置，最後重新執行原始零控制值計畫，核對原始狀態已恢復。
候選需在前段與保留樣本都通過既有至少1%收益的bootstrap篩選，才標示值得後續驗證。
這仍是篩選方法，不是對計時樣本獨立性、時脈控制、最佳化極限或遊戲FPS的證明；沒有候選通過就不採用候選。

## 結果解讀

r63_swin_scheduling_test_complete_not_game_validation：本轮診斷與比較完成。是否改善，另看 timing 與 scheduler_candidate_decisions。
scheduling_gate.approved=true：只允許本次指定合成非零控制值基準測試，**不是遊戲發布許可**。
release_gate.approved、game_deployment_approved、full_model_quality_verified 與 game_tested 仍為 false。
failed：保存JSON/TXT；不要把測試包放入遊戲，也不用因為單次試驗失敗就重裝HIP。

## 建置證據

BUILD_STATUS.json、reports/、SHA256SUMS.txt 記錄實際編譯與CPU測試結果。
本輪CI可解析固定原GPU模組，但 **没有你的實體9070XT，也不曾讀取你本機的完整checkpoint或執行此版GPU試驗**。
Linux執行排程位元組隔離、錯誤拒絕、平衡順序、ASAN/UBSAN與繼承的CPU測試；Windows執行實際EXE的CPU及ZIP自測。
source/包含實際編譯來源、產生器、描述子變更規格、所需標頭及授權。
開發分支 dlssnr-rdna4-r63-swin-scheduling；main與既有版本未合併或覆蓋。
