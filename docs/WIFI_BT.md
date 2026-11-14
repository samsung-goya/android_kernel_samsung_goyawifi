# Wi-Fi & Bluetooth (Marvell Avastar 88W8777)

> **Status: Unstable.** Basic association, data transfer, and Bluetooth scanning work, but the `88W8777` SDIO stack is still considered unstable despite the workarounds below (SDIO/firmware edge cases can still occur under load or power-state transitions).

## Stack
- **Interface:** SDIO (`sdh2`, with `wifi-pwrseq` and `ldo10` 3.3V supply)
- **Wi-Fi driver:** `mwifiex` / `mwifiex_sdio` (SDIO ID `8777`)
- **Bluetooth driver:** `btmrvl` / `btmrvl_sdio`

## Hardware notes
- `mwifiex` uses descriptor-driven 16-byte TxPD/RxPD, single-port mode, legacy scan, and HT20 fallback for `88W8777`.
- `sdhci-pxav3` includes an SDIO IRQ runtime-suspend fix to prevent `CMD53` timeouts under sustained traffic.
- `btmrvl_sdio` handles `-EPROBE_DEFER` and uses a PM `device_link` to coordinate power sequencing with `mwifiex_sdio`.
- `mwifiex` IEEE power-save and `cleanup_sdio` deadlock fixes are included for clean suspend/resume with Wi-Fi/BT coexistence.
