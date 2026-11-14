/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ETNAVIV_GOYA_H__
#define __ETNAVIV_GOYA_H__

struct etnaviv_gpu;

#ifdef CONFIG_DRM_ETNAVIV_GOYA
int etnaviv_goya_init(struct etnaviv_gpu *gpu);
void etnaviv_goya_fini(struct etnaviv_gpu *gpu);
void etnaviv_goya_busy(struct etnaviv_gpu *gpu, int delta);
void etnaviv_goya_reset(struct etnaviv_gpu *gpu);
int etnaviv_goya_cooling(struct etnaviv_gpu *gpu, unsigned long state);
#else
static inline int etnaviv_goya_init(struct etnaviv_gpu *gpu) { return 0; }
static inline void etnaviv_goya_fini(struct etnaviv_gpu *gpu) { }
static inline void etnaviv_goya_busy(struct etnaviv_gpu *gpu, int delta) { }
static inline void etnaviv_goya_reset(struct etnaviv_gpu *gpu) { }
static inline int etnaviv_goya_cooling(struct etnaviv_gpu *gpu, unsigned long state)
{
	return 0;
}
#endif

#endif /* __ETNAVIV_GOYA_H__ */
