# Idle, suspend & recovery (`pxa988` PM)

## Stack
- **cpuidle:** `cpuidle-pxa988` (`C1` / `C2` states)
- **Suspend:** `pm-pxa988` (`D2_UDR` low-power mode + `s2idle`), `headsmp-pxa988.S` warm-reset entry via `CIU_WARM_RESET_VECTOR`
- **Wakeups:** `gpio-pxa` wakeup sources (e.g., `gpio-keys` Volume/Home), PMIC (`88pm80x_on`)
- **Watchdog & crash logs:** `pxa988_wdt` (20s HW watchdog) + `ramoops` at `0x08100000` (TWRP `/proc/last_kmsg` compatible)

## Notes
- `pxa988-ddr` handles DDR frequency scaling (`312 / 533 MHz` `simple_ondemand`) and suspend/resume hooks.
- `i2c-pxa` includes restart quiesce and `twsi1` (`GPIO87`/`GPIO88`) bus recovery for reliable suspend/resume of I2C peripherals (touchscreen & DSI bridge).
- **Why `D1P` / `D1` are not used in runtime `cpuidle`:**
  - `D1P` asserts `PMUM_AXISD` (AXI shutdown), and `D1` additionally asserts `PMUM_APBSD | PMUM_SLPEN | PMUM_DDRCORSD` (APB shutdown, sleep enable, and DDR core shutdown) in `MPMU_APCR`.
  - Enabling `D1P`/`D1` during runtime idle would gate clocks/buses and DDR while active peripherals and DMA engines (display scanout, USB, SDIO/Wi-Fi, audio TDMA) are still running without full device-suspend quiescing, breaking active I/O and causing hangs/timeouts.
  - Because of this, runtime `cpuidle` stops at `C1`/`C2` (per-core WFI/power-down), and SoC-wide low-power entry (`D2_UDR`) is only used during full system suspend after drivers are suspended.
- **Deferred:** DDR self-refresh / deeper `D2` states beyond `D2_UDR` are not enabled.
