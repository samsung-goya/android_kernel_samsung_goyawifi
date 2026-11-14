# Video Processing Unit (Chips&Media CODA7542)

## Stack & firmware
- **Kernel driver:** `drivers/media/platform/coda/` (`CONFIG_VIDEO_CODA=m`, `coda.ko`), bound to `marvell,pxa988-vpu` (`0xd420d000`, `GIC_SPI 25`)
- **Power domain & SRAM:** `marvell,pxa988-vpu-power`, `vsram` (`0xd100ea00`, `0x11600` bytes)
- **Clocks:** `vpu_clk` and `vpu_axi` (`156 MHz`)
- **Firmware:** `/lib/firmware/goya-coda7542-vpu-fw.bin` (`163840` bytes / `0x28000`, `CODA_7542` `0.9.86`)
  - Extracted from the stock `libcoda7542hal.so` userspace library (`bit_code` at offset `0x2e004`, SHA-256 `a94eccf44bfb6dff489648077b73b13e5caa2787ce96855d608f463893ebe06d`).

## Capabilities
Registers `/dev/video[1-4]`:
- **H.264 Encode:** up to `1280x720`, Constrained Baseline, Level 3.1 (`NV12`, `YU12`, `YV12`)
- **H.264 Decode:** up to `1920x1088`, Constrained Baseline / Main / High, Level 4.0 (`NV12`, `YU12`, `YV12`)
- **Other codecs:** JPEG encode/decode, MPEG-4 encode, MPEG-2/MPEG-4 decode

## Hardware quirks & notes
- **Headerless firmware & decoder prescan (`0x1d4`):** `CODA_7542` `0.9.86` is a raw headerless BIT image (`size / 4` words instead of `(size - 16) / 4`), and its `PIC_RUN` leaves `CODA_RET_DEC_PIC_OPTION` (`0x1d4`) at `0`, so the `prescan failed` check is skipped for `CODA_7542`.
- **Decoder stride on non-16-aligned widths:** When decoding widths that are not 16-aligned (e.g. `1048x720`), `coda` sets `bytesperline = 1056` (`round_up(width, 16) != width`). `ffmpeg`'s `h264_v4l2m2m` decoder handles this cleanly, but raw/encode consumers that ignore `bytesperline` will shear diagonally.
- **Decoder DPB buffer count:** Lowering decoder capture buffer counts below the default (e.g., to `6` buffers) stalls H.264 DPB reordering.
- **Chroma stride on `YU12` encode:** `coda` aligns `bytesperline` to `16`. Widths that are multiples of `16` but not `32` (e.g., `1040x720`) produce a `YU12` chroma stride that is not 16-byte aligned (`520`), causing diagonal chroma artifacts. Use widths that are multiples of `32` (e.g., `640`, `1024`, `1280`) for `YU12` encode.
- **CBR frame skipping:** At low bitrates with short GOPs (e.g. `ffmpeg` default `-g 12` at `1M`), CODA's frame rate control can exhaust the bit budget and emit 12-byte skip frames. Raising bitrate or GOP (e.g. `-b:v 4M -g 60`) avoids skip frames.
