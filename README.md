# Nomad Drive v1.0.1 — EOS 局域網／跨網段聯機補丁

> **適用範圍**：本專案與發行補丁僅針對 **Nomad Drive v1.0.1** 驗證打包（不保證適用於其他遊戲）。
> 下載即用補丁包（`NomadDrive-EOS-LAN-Patch-v1.0.1.zip`）：**[Releases v1.0.1](https://github.com/Salamancatuko/NomadDrive-EOS-LAN-Patch/releases/tag/v1.0.1)**

---

## 簡明原理

*Nomad Drive* 聯機底層透過 `EOSSDK-Win64-Shipping.dll`（Epic Online Services）搜尋大廳並建立 P2P 連線。本補丁替換該 DLL，將大廳與 P2P 改走 **本機 UDP 探索＋TCP 直連**（免 Steam、免外網），並修復客機閃退無法重連、Windows 非 `/24` 子網遮罩、動態網卡刷新及中文路徑等問題；所有相依庫已靜態連結，免安裝額外運行庫。

## 致謝原作者

- **[Nemirtingas](https://gitlab.com/Nemirtingas)** — 原版 *Nemirtingas's Epic Emulator* 作者（LGPL-3.0）。
- **[psfree](https://github.com/psfree/eos_sdk_emu)** — 上游分支 `psfree/eos_sdk_emu` 維護與貢獻者。

---

## 如何使用與注意事項

完整圖文與各網路環境範例請直接下載解壓壓縮包或檢視 **[使用說明.md](./使用說明.md)**：

1. **安裝位置**：關閉遊戲並備份原版後，將 `EOSSDK-Win64-Shipping.dll` 覆蓋至 `Nomad Drive_Data\Plugins\x86_64\`，並把範本 `NemirtingasEpicEmu.json` 放於 `Nomad Drive.exe` 旁（房主建議改用 `presets/NomadDrive-Host-NemirtingasEpicEmu.json` 並更名為 `NemirtingasEpicEmu.json` 以支援客機閃退重連）。
2. **切勿複製已生成的設定檔**：每台電腦首次啟動會寫入隨機 `epicid` 與 `productuserid`，直接複製給朋友會造成 ID 衝突無法連線。
3. **跨網段／VPN／埠轉發**：在 `custom_broadcast` 填入目標 IP、CIDR（如 `192.168.20.0/24`）、IP 區間（`192.168.20.10-50`）或主機名，支援單邊探索；跨公網可固定 `discovery_port` 與 `tcp_port` 進行埠轉發。
4. **防火牆**：首次執行請放行遊戲網路權限，或開放 UDP `55789–55798` 入站規則。

## 自行編譯與授權

本倉庫原始碼延續上游 **GNU LGPL v3.0** 授權；編譯環境為 Windows + MSYS2 MinGW64（GCC C++17 + `protobuf-lite` 靜態連結）。詳細設定檔範本位於 [`release_templates/`](./release_templates/)。
