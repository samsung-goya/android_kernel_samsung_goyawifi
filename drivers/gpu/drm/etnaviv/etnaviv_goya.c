// SPDX-License-Identifier: GPL-2.0
/*
 * Board-scoped Samsung goya board GC1000 devfreq.
 * Busy time is the union of queued hardware-job intervals through fence IRQ
 * completion, not CPU load.
 */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "etnaviv_gpu.h"
#include "etnaviv_goya.h"

static const unsigned long goya_gpu_rates[] = {
	156000000, 312000000, 416000000, 624000000
};

struct etnaviv_goya {
	struct etnaviv_gpu *gpu;
	struct devfreq *df;
	struct devfreq_dev_profile profile;
	struct devfreq_simple_ondemand_data governor;
	spinlock_t lock;
	u64 last_ns;
	u64 sample_ns;
	u64 busy_ns;
	u64 submitted;
	u64 completed;
	unsigned int jobs;
	unsigned int util;
	unsigned int transitions;
};

static void account_busy(struct etnaviv_goya *d, u64 now)
{
	if (d->jobs)
		d->busy_ns += now - d->last_ns;
	d->last_ns = now;
}

void etnaviv_goya_busy(struct etnaviv_gpu *gpu, int delta)
{
	struct etnaviv_goya *d = READ_ONCE(gpu->goya);
	unsigned long flags;

	if (!d)
		return;

	spin_lock_irqsave(&d->lock, flags);
	account_busy(d, ktime_get_ns());
	if (delta > 0) {
		d->jobs++;
		d->submitted++;
	} else if (d->jobs) {
		d->jobs--;
		d->completed++;
	}
	spin_unlock_irqrestore(&d->lock, flags);
}

void etnaviv_goya_reset(struct etnaviv_gpu *gpu)
{
	struct etnaviv_goya *d = READ_ONCE(gpu->goya);
	unsigned long flags;

	if (!d)
		return;

	spin_lock_irqsave(&d->lock, flags);
	account_busy(d, ktime_get_ns());
	d->jobs = 0;
	spin_unlock_irqrestore(&d->lock, flags);
}

static int etnaviv_goya_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct etnaviv_gpu *gpu = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	unsigned long target_freq, before;
	int ret, active;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	target_freq = dev_pm_opp_get_freq(opp);
	dev_pm_opp_put(opp);

	active = pm_runtime_get_if_in_use(dev);
	if (active < 0)
		return active;

	mutex_lock(&gpu->lock);
	before = clk_get_rate(gpu->clk_core);
	ret = clk_set_rate(gpu->clk_core, target_freq);
	*freq = clk_get_rate(gpu->clk_core);
	if (!ret && before != *freq)
		gpu->goya->transitions++;
	mutex_unlock(&gpu->lock);

	if (active)
		pm_runtime_put_autosuspend(dev);

	return ret;
}

static int etnaviv_goya_status(struct device *dev,
			       struct devfreq_dev_status *stat)
{
	struct etnaviv_gpu *gpu = dev_get_drvdata(dev);
	struct etnaviv_goya *d = gpu->goya;
	u64 now = ktime_get_ns();
	u64 total, busy;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	account_busy(d, now);
	total = now - d->sample_ns;
	busy = min(d->busy_ns, total);
	d->sample_ns = now;
	d->busy_ns = 0;
	d->util = total ? (unsigned int)div64_u64(busy * 100, total) : 0;
	spin_unlock_irqrestore(&d->lock, flags);

	stat->current_frequency = clk_get_rate(gpu->clk_core);
	stat->total_time = min_t(u64, div_u64(total, 1000), ULONG_MAX);
	stat->busy_time = min_t(u64, div_u64(busy, 1000), ULONG_MAX);
	return 0;
}

static int etnaviv_goya_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct etnaviv_gpu *gpu = dev_get_drvdata(dev);

	*freq = clk_get_rate(gpu->clk_core);
	return 0;
}

static ssize_t gpu_load_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct etnaviv_gpu *gpu = dev_get_drvdata(dev);
	struct etnaviv_goya *d = gpu->goya;
	unsigned long flags;
	int n;

	spin_lock_irqsave(&d->lock, flags);
	n = scnprintf(buf, PAGE_SIZE,
		      "utilization_pct=%u active_jobs=%u submitted=%llu completed=%llu transitions=%u thermal_state=%u\n",
		      d->util, d->jobs, d->submitted, d->completed,
		      d->transitions, gpu->freq_scale);
	spin_unlock_irqrestore(&d->lock, flags);
	return n;
}
static DEVICE_ATTR_RO(gpu_load);

int etnaviv_goya_cooling(struct etnaviv_gpu *gpu, unsigned long state)
{
	struct etnaviv_goya *d = READ_ONCE(gpu->goya);
	unsigned long cap;

	if (state >= ARRAY_SIZE(goya_gpu_rates))
		return -EINVAL;

	cap = goya_gpu_rates[ARRAY_SIZE(goya_gpu_rates) - 1 - state];
	if (d && d->df) {
		mutex_lock(&d->df->lock);
		d->df->max_freq = cap;
		mutex_unlock(&d->df->lock);
	}

	gpu->freq_scale = state;
	return 0;
}

int etnaviv_goya_init(struct etnaviv_gpu *gpu)
{
	struct etnaviv_goya *d;
	int i, ret;

	if (!of_machine_is_compatible("samsung,goyawifi"))
		return 0;

	if (gpu->identity.model != 0x1000 || gpu->identity.revision != 0x5037)
		return -ENODEV;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->gpu = gpu;
	spin_lock_init(&d->lock);
	d->last_ns = ktime_get_ns();
	d->sample_ns = d->last_ns;

	for (i = 0; i < ARRAY_SIZE(goya_gpu_rates); i++) {
		ret = dev_pm_opp_add(gpu->dev, goya_gpu_rates[i], 1175000);
		if (ret)
			goto err_opp;
	}

	d->profile.initial_freq = clk_get_rate(gpu->clk_core) ?: 156000000;
	d->profile.polling_ms = 100;
	d->profile.target = etnaviv_goya_target;
	d->profile.get_dev_status = etnaviv_goya_status;
	d->profile.get_cur_freq = etnaviv_goya_get_cur_freq;

	d->governor.upthreshold = 40;
	d->governor.downdifferential = 20;

	WRITE_ONCE(gpu->goya, d);
	d->df = devfreq_add_device(gpu->dev, &d->profile,
				   DEVFREQ_GOV_SIMPLE_ONDEMAND, &d->governor);
	if (IS_ERR(d->df)) {
		ret = PTR_ERR(d->df);
		goto err_clear;
	}

	ret = device_create_file(gpu->dev, &dev_attr_gpu_load);
	if (ret)
		dev_warn(gpu->dev, "failed to create gpu_load sysfs: %d\n", ret);

	dev_info(gpu->dev, "Samsung goya board GC1000 devfreq registered (init %lu MHz)\n",
		 d->profile.initial_freq / 1000000);
	return 0;

err_clear:
	WRITE_ONCE(gpu->goya, NULL);
err_opp:
	while (--i >= 0)
		dev_pm_opp_remove(gpu->dev, goya_gpu_rates[i]);
	kfree(d);
	return ret;
}

void etnaviv_goya_fini(struct etnaviv_gpu *gpu)
{
	struct etnaviv_goya *d = READ_ONCE(gpu->goya);
	int i;

	if (!d)
		return;

	device_remove_file(gpu->dev, &dev_attr_gpu_load);
	devfreq_remove_device(d->df);
	WRITE_ONCE(gpu->goya, NULL);
	for (i = 0; i < ARRAY_SIZE(goya_gpu_rates); i++)
		dev_pm_opp_remove(gpu->dev, goya_gpu_rates[i]);
	kfree(d);
}
