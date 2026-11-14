# Display (`mmp-drm` & QuickLogic VX5B3D)

## Stack
- **DRM/KMS driver:** `drivers/gpu/drm/mmp/` (`mmp_drv.c`, `mmp_crtc.c`, `mmp_plane.c`, `mmp_dsi.c`)
- **DSI-to-LVDS bridge:** `quicklogic-vx5b3d` (with `vx5b3d-backlight`)
- **Panel:** `boe,ba070ws1-400` (`panel-lvds`, `1024x600`, `vesa-24`, `154x90` mm)
- **Buffers:** Standard DRM CMA GEM helpers (`drm_gem_cma_*`, `drm_fbdev_generic_setup`)

## Notes
- The Samsung bootloader leaves the LCD controller (`0xd420b000`) and MIPI-DSI host (`0xd420b800`) active scanning out a `1024x600` (`a8r8g8b8`) bootloader splash at `0x17000000`.
- `mmp-drm` exposes primary (`GRA`) and overlay (`DMA`) planes on `/dev/dri/card0` and removes any conflicting early framebuffer via `drm_fb_helper_remove_conflicting_framebuffers()`. This would theoretically allow for multi-plane composition?
- Display has some visible tearing on some regions, however isn't a major issue