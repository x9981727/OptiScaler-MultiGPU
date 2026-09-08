XeFG v26.2 Controller-only Hotfix

用途
- 針對 v26.1 已能成功建立 Present 關聯，但控制器可能因啟動視窗搶走前景而一直停在 baseline 的情況。
- 此包只更新 AutoPacing 控制器，不替換遊戲 dxgi.dll，也不修改 OptiScaler.ini。

v26.2 變更
- 啟動時只做一次 best-effort 前景交接到 wwm.exe 的可見主視窗。
- 如果 Windows 拒絕切換，不會繞過安全門檻；控制器會明確顯示 GAME FOCUS REQUIRED。
- association、SDK 2-output、來源完成率 -3%、native-to-display +8 ms 等原有 fail-closed 保護維持不變。
- 額外等待只會在 association 已鎖定、stage=active、最新 native 資料新鮮且 DLL 自身 eligible=true 時啟用。

測試方式
1. 保留目前已安裝的 v26 遊戲 DLL。
2. 解壓本包到遊戲資料夾以外的位置。
3. 啟動遊戲並開啟原本的 XeFG 雙卡設定。
4. 以系統管理員身分執行 1-AutoPacing-v26.2.cmd。
5. 正常情況下控制器會嘗試把焦點交回 wwm.exe。若畫面沒有切回遊戲，請手動點一次遊戲視窗。
6. 保持相同場景至少 10 秒。觀察控制器是否出現 association=True、mode=active，以及 matched 持續增加。
7. 測試完成後回傳最新 capture-v262-* 資料夾與 OptiScaler.log。

重要限制
- 這仍是實驗性時序校正，不是完整畫質或輸入到光子延遲認證。
- 若來源完成率下降超過 3%、native-to-display 中位數增加超過 8 ms、關聯變得不安全或遊戲離開前景，額外等待會停止。
- 不要為了讓控制器保持 active 而停用這些保護。
