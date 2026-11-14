# Android Common Kernel 4.19 kernel for Samsung Galaxy Tab 3 Lite 7.0 Wi-Fi (`SM-T110` / `goyawifi`)

## Support status

| Subsystem | Hardware | Driver / Details | Status |
|---|---|---|---|
| **SoC / SMP** | Marvell PXA986 (2x Cortex-A9) | `mach-mmp`, SCU, L2C-310, TWD, PMU | Working |
| **Clocks / Power** | APBC / APMU / MPMU / CIU | `clk-of-pxa988`, `pxa988-pm-domain` | Working |
| **DVFS** | CPU (`312`–`1205` MHz), DDR (`312`/`533` MHz), GPU (`156`–`624` MHz) | `cpufreq-dt`, `pxa988-ddr`, `etnaviv` devfreq, `BUCK1` scaling | Working |
| **Idle / Suspend** | C1/C2 cpuidle, `D2_UDR` suspend, `s2idle` | [docs/SUSPEND.md](docs/SUSPEND.md) | Experimental |
| **Storage** | 8-bit eMMC 4.5 + microSD | `sdhci-pxav3` | Working |
| **USB** | USB 2.0 OTG/UDC + MUIC | ChipIdea UDC + `mv-usb2-phy`, `rt8973a` extcon (ConfigFS ACM/RNDIS) | Working |
| **Display** | `1024x600` LVDS panel + QuickLogic VX5B3D bridge | [docs/DISPLAY.md](docs/DISPLAY.md) | Working |
| **3D GPU** | Vivante GC1000 (`rev 5037`) | [docs/GPU.md](docs/GPU.md) | Working |
| **VPU** | Chips&Media CODA7542 (H.264/MPEG4/JPEG) | [docs/VPU.md](docs/VPU.md) | Experimental |
| **Camera** | 2 MP Rear (`SR200PC20M`, MIPI CSI-2) | `marvell-ccic` + `sr200pc20m` (`UYVY` up to `1600x1200`) | Working |
| **Touchscreen & Keys** | Zinitix BT532 + Volume/Home/Power | `zinitix` (5-finger MT + Menu/Back), `gpio-keys`, `88pm80x_on` | Working |
| **PMIC / Regulators / RTC** | Marvell 88PM822 | `88pm800`/`88pm822` MFD, regulators, RTC, GPADC | Working |
| **Battery / Charger** | SMB328 + 88PM822 SW fuel gauge | `smb328-charger` (`extcon` MUIC), `pm822-battery` | Working |
| **Audio** | Marvell 88PM805 codec + SSP1/TDMA | `88pm805`, `pxa-ssp`, `mmp-pcm`, `pxa-88pm805` (speaker & built-in mic) | Partial (3.5mm jack/headset mic untested) |
| **Wi-Fi / Bluetooth** | Marvell Avastar 88W8777 (SDIO) | [docs/WIFI_BT.md](docs/WIFI_BT.md) | Experimental |
| **Sensors** | Bosch BMA255 Accelerometer | `bmc150-accel` (only sensor populated on SM-T110) | Working |
| **Thermal / Watchdog** | PXA988 DROTS + HW WDT | `pxa988_thermal`, `pxa988_wdt` | Working |
| **GPS** | Marvell 88L2000 (UART2) | Requires proprietary userspace | Untested / Not supported |

## Building

```bash
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnu-   # or arm-linux-gnueabihf-

make O=out goyawifi_defconfig
make O=out -j$(nproc) zImage pxa986-samsung-goyawifi.dtb modules
```

## Notes

- **Bootloader & `boot.img` packing (`CONFIG_ARM_SAMSUNG_PXA_BOOTFIXUP=y`):**
  - Samsung's bootloader (`S-Boot`) ignores the Android `boot.img` header command line and passes an obsolete low `initrd` address.
  - Boot images must inject `/chosen/bootargs` and `samsung,goyawifi-initrd-size` into the appended DTB and place the `initramfs` in trailing FDT space (handled by `arch/arm/kernel/samsung_pxa_bootfixup.c`).
  - The `KERNEL` partition hard limit is **16 MiB** (`16,777,216` bytes).
  - *Only `SM-T110` (`goyawifi`) is tested; `SM-T111`/`T113`/`T116` variants are untested.*
- **Memory layout (`VMSPLIT_3G_OPT` + CMA):**
  - Uses `CONFIG_VMSPLIT_3G_OPT=y` (`PAGE_OFFSET=0xB0000000`, `HIGHMEM=n`) so all 1 GiB of RAM is directly mapped in lowmem without `kmap` overhead.
  - `linux,cma` is a **96 MiB** reusable pool in `0x20000000..0x2fffffff` (`< 0x80000000` for `etnaviv` MMUv1).
  - `cp@6000000` (`0x06000000`, 33 MiB) must remain `no-map` in the device tree even on the Wi-Fi-only model because the bootloader/OBM hardware-protects that range at boot.
  - `ramoops@8100000` (512 KiB, `console-size = 0x40000`, `ecc-size = 16`) matches TWRP's `/proc/last_kmsg` reader.
