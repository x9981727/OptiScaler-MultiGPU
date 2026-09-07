# RDNA4 r6 AutoLab — 自動驗證環境與可執行套件

**這是驗證環境的交付版，不是「最完美 DLSS-NR」或遊戲最終 DLL。**
本版未重寫完整 Swin，也沒有新增已測得的遊戲效能提升。兩個 Head 候選仍使用 r4 已實測的相同 GPU binary。

## 已實作的自動化

### Linux / Windows 虛擬環境

`.github/workflows/rdna4-autolab.yml` 在獨立開發分支有程式變更時自動執行。
Linux VM 以 Debug、最佳化、AddressSanitizer/UndefinedBehaviorSanitizer 三組編譯設定，執行 Head 數值契約、完整索引/FP16 捨入、舊解析錯誤回歸及隨機錯誤指標測試。Windows VM 編譯真正的 x64 EXE，執行 CPU 與 ZIP/拒絕案例。

這是 **CPU 上的數值契約與軟體測試環境**，不是 gfx1201 指令集或時脈精確模擬器，不會捏造 9070 XT FPS、WMMA 利用率或完整模型影像。
原版核心得以交叉反組譯、候選核心得以交叉編譯，不代表它們已在此 VM 執行。`reports/` 與 `BUILD_STATUS.json` 明確區分每一種驗證。

### 一次啟動的實體 GPU 套件

`rdna4-autolab.exe` 重用已跑通的 r5.2 計畫播放程式，新增：

1. 三種合成初始化下的原版與兩個 Head 替換版對照。
2. 原版逐命令指紋重跑，確認診斷本身可重現。
3. 確認 Head 輸出真的被暫時覆寫後，逐命令追蹤全部相關資料配置的 SHA256；不把「沒有真的改到輸出」當有效反向測試。
4. 分開記錄逐命令 GPU event 計時及完整 GPU graph 計時。前者有同步/插樁開銷，不可直接除以後者作精確百分比。
5. 若輸出敏感性失敗，自動保存完整追蹤與逐命令耗時，停止後續候選升版；不是把失敗稱作收斂。
6. 只有敏感性通過才進入 45 輪訓練及 21 輪保留樣本，交錯比較原版與候選。保存所有樣本並以成對差異 bootstrap 篩選，要求正的估计區間下界及至少 1% 平均改善。這是篩選門檻，不是統計獨立性或全域最優的證明。

每一輪有限制，遇到錯誤立即保存並中止。沒有無上限迴圈、系統時脈修改、GPU reset、安全軟體停用、登錄變更或遊戲注入。
程式不會自行修改 AI 模型/原始碼，也不會因單個測試較快就自動覆蓋遊戲 DLL。

## 使用與自動呼叫

解壓到可寫入的獨立資料夾。GPU 測試仍需要正確 AMD HIP 7 及 RX9070/gfx1201；自動裝置選擇不使用 RX6600XT。
`RUN_AUTOLAB.cmd` 是一般入口，只需在開始時選取原本三個檔案。也可讓已授權的遠端工具直接呼叫，不必每輪人工操作：

```bat
rdna4-autolab.exe --plan "D:\Tests\DLSSNR-rewrite-source-checkpoint.zip" --runtime "D:\Game\dlssnr_amd_pass1.dll" --weights "D:\Game\dlssnr_on_amd_weights.bin" --output "D:\Tests\rdna4-r6-result.json"
```

以上路徑是範例，不是已知的你的電腦路徑。檔案只讀，程式不搜尋整顆硬碟、不上傳權重或影像，也不新增遠端存取服務。
若只驗證 CPU：

```bat
rdna4-autolab.exe --cpu-self-test --output cpu-lab.json
```

若只檢查原 checkpoint：

```bat
rdna4-autolab.exe --validate-plan-only --plan "D:\Tests\DLSSNR-rewrite-source-checkpoint.zip" --output plan-check.json
```

不需要 Python 或 Visual Studio。完整 GPU 流程仍需要你已經有的 checkpoint、原版 DLL、權重與 HIP，這些不包含在新 ZIP。
原 checkpoint、原版 GPU 核心、r4 候選 binary 仍由雜湊保護，不能用任意測試 JSON 或其他版本代替。

## 真正做到無人工來回，需要什麼

虛擬環境可以自動編譯與重跑 CPU 測試；若要由助理直接量測你的 9070 XT，必須有你授權且已連線的實體 GPU 終端。僅有 GitHub 原始碼連線及舊 JSON 不能遠端控制你的電腦。
本套件沒有擅自建立此連線，也沒有租用收費 GPU。

## 如何解讀狀態

- `cpu_tests_passed_original_plan_and_gpu_not_tested`：CPU 測試通過，不是 GPU 通過。
- `autolab_blocked_output_sensitivity`：數值比較可通過，但被確認的 Head 改動沒有反映在觀察輸出。檢查 `causal_trace`，禁止採用效能結論作遊戲發布依據。
- `failed`：讀檔、數值、指標、GPU 或其他檢查失敗，保存第一個錯誤。
- 固定計畫測試通過：只表示特定合成案例，仍不足以證明遊戲、歷史狀態、HDR 或完整模型品質。

診斷成功但被發布門檻擋下時，行程可能正常結束；自動化必須檢查 JSON 的狀態與 `release_gate.approved`，不能只看 exit code。
`release_gate.approved` 在這個開發階段保持 false。最終遊戲 DLL 需要完整 Swin 實作、有效的真實輸入/時序驗證、實機長時間穩定性、遊戲接合及可重現整體效能改善；目前這些尚未完成。

## 可重現內容

套件含 EXE、固定 r4 HSACO、CPU VM 執行報告、原版邊界 ISA 靜態調查、第三方授權及實際編譯原始碼。
CI 不包含原 checkpoint。CPU 錯誤回歸 fixture 僅包含先前報告的三筆特定呼叫；不得將它誤認為 154 次完整呼叫已在 CPU 執行。
`source/` 和 `.github/workflows/rdna4-autolab.yml` 保存建置流程；`SHA256SUMS.txt` 可驗證套件。
開發分支 `dlssnr-rdna4-r6-autolab` 與 main 分離，沒有覆蓋原遊戲套件。
