# 3D GPU (`etnaviv` / Vivante GC1000)

## Stack
- **Kernel driver:** Mainline `etnaviv` (`CONFIG_DRM_ETNAVIV=y`), exposing `/dev/dri/renderD128`
- **Power domain:** `marvell,pxa988-gc-power` (`arch/arm/mach-mmp/pxa988-pm-domain.c`)
- **Userspace:** Mesa (`OpenGL ES 2.0`, `mmp-drm_dri.so` / KMSRO)

## Clocks & DVFS
- Uses `pxa988` GC mix/gate clocks (`156 MHz` base core / `208 MHz` AXI).
- GPU DVFS is wired via `simple_ondemand` devfreq across `156 / 312 / 416 / 624 MHz`.

## Memory / CMA notes
- `etnaviv` allocates from the shared `reusable` `linux,cma` pool (`96 MiB` in `0x20000000..0x2fffffff`).
- Keeping `linux,cma` below `0x80000000` is required for `etnaviv` MMUv1 linear/command-buffer virtual addressing (`etnaviv_cmdbuf_get_va < 0x80000000`).

## Benchmark & shader notes (`glmark2 2023.01`)
Tested at `800x600` windowed (`OpenGL ES 2.0 Mesa 26.2.6`, `Vivante GC1000 rev 5037`):
- **Overall score:** `177` (`build use-vbo=true`: 524 FPS, `texture`: ~309–324 FPS, `shading`: 131–314 FPS, `jellyfish`: 90 FPS, `shadow`: 59 FPS).
- **Known limitation:** The `terrain` scene fails shader compilation on GC1000 (`MESA: error: etna_draw_vbo:405: compiled shaders are not okay`, 9 FPS).
