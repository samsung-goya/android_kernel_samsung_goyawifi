// SPDX-License-Identifier: GPL-2.0-only
/*
 * 88pm805-codec.c -- Marvell 88PM805 ALSA SoC Audio Codec driver
 *
 * Copyright (C) 2013 Marvell International Ltd.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/mfd/88pm80x.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "88pm805-codec.h"

#define PM822_HEADSET_DET_EN		0x38
#define PM822_HEADSET_DET_STS		0x39
#define PM822_CLASS_D_CTRL		0x48
#define PM822_CLASS_D_MISC1		0x61
#define PM822_CLASS_D_MISC2		0x62

enum pm805_mic_source {
	PM805_MIC_BUILTIN = 0,
	PM805_MIC_SECONDARY = 1,
	PM805_MIC_HEADSET = 2,
};

struct pm805_priv {
	struct device *dev;
	struct snd_soc_component *component;
	struct pm80x_chip *chip;
	struct regmap *regmap;
	struct regmap *pmic_regmap;
	struct regulator *micbias;
	struct mutex lock;

	bool powered;
	bool playback_prepared;
	bool capture_prepared;
	bool playback_active;
	bool hp_on;
	bool bias_on;
	bool spk_switch;
	bool hp_switch;
	bool jack_plugged;

	unsigned int spk_vol;
	unsigned int hp_vol;
	unsigned int mic_gain;
	enum pm805_mic_source mic_source;
};

static struct pm805_priv *pm805_comp_priv(struct snd_soc_component *comp)
{
	struct pm80x_chip *chip = snd_soc_component_get_drvdata(comp);

	return chip->audio_priv;
}

static struct regmap *pm805_get_companion_regmap(struct pm80x_chip *chip)
{
	struct pm80x_chip *comp;

	if (!chip || !chip->companion)
		return NULL;

	comp = i2c_get_clientdata(chip->companion);
	return comp ? comp->regmap : NULL;
}

static int pm805_update_micbias(struct pm805_priv *pm805, bool preparing)
{
	bool want_on = pm805->mic_source == PM805_MIC_HEADSET &&
		       (pm805->jack_plugged || preparing ||
			pm805->capture_prepared);
	int ret = 0;

	if (!pm805->micbias || pm805->bias_on == want_on)
		return 0;

	if (want_on)
		ret = regulator_enable(pm805->micbias);
	else
		ret = regulator_disable(pm805->micbias);

	if (!ret)
		pm805->bias_on = want_on;

	return ret;
}

static int pm805_hp_gain(struct pm805_priv *pm805, unsigned int val)
{
	int ret;

	ret = regmap_write(pm805->regmap, PM805_CODEC_VOL_SEL_CHANNEL_1, val);
	if (ret)
		return ret;

	return regmap_write(pm805->regmap, PM805_CODEC_VOL_SEL_CHANNEL_2, val);
}

static int pm805_hp_sequence(struct pm805_priv *pm805, bool enable)
{
	unsigned int status = 0;
	int i, ret;

	if (pm805->hp_on == enable)
		return 0;

	/* Attenuate to minimum (-91 dB) and let the ramp settle. */
	ret = pm805_hp_gain(pm805, 1);
	if (ret)
		return ret;
	msleep(20);

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_HEADPHONE_SETTING,
				 0xff, enable ? 0x1d : 0);
	if (ret)
		return ret;

	pm805->hp_on = enable;
	if (enable)
		msleep(100);

	for (i = 0; i < 20; i++) {
		ret = regmap_read(pm805->regmap,
				  PM805_CODEC_AUTO_SEQUENCE_STS_1, &status);
		if (ret)
			return ret;
		if ((status & (enable ? 0x50 : 0xa0)) == (enable ? 0x50 : 0xa0))
			return 0;
		usleep_range(10000, 11000);
	}

	dev_err(pm805->dev, "headphone sequence %s timeout, status=%02x\n",
		enable ? "up" : "down", status);
	return -ETIMEDOUT;
}

static int pm805_update_output_route(struct pm805_priv *pm805)
{
	bool active = pm805->playback_prepared && pm805->playback_active;
	bool use_hp = pm805->jack_plugged && pm805->hp_switch;
	bool use_spk = active && !pm805->jack_plugged && pm805->spk_switch;
	int ret;

	if (!use_spk) {
		ret = regmap_write(pm805->regmap,
				   PM805_CODEC_VOL_SEL_CHANNEL_3, 1);
		if (ret)
			return ret;
	}

	if (pm805->pmic_regmap) {
		ret = regmap_update_bits(pm805->pmic_regmap,
					 PM822_CLASS_D_CTRL, BIT(1),
					 use_spk ? BIT(1) : 0);
		if (ret)
			return ret;
	}

	ret = pm805_hp_sequence(pm805, pm805->playback_prepared && use_hp);
	if (ret)
		return ret;

	if (pm805->hp_on)
		return pm805_hp_gain(pm805,
				     (active && use_hp) ? pm805->hp_vol + 92 : 1);

	if (use_spk)
		return regmap_write(pm805->regmap,
				    PM805_CODEC_VOL_SEL_CHANNEL_3,
				    pm805->spk_vol + 92);

	return 0;
}

static int pm805_sync_blocks(struct pm805_priv *pm805)
{
	bool p = pm805->playback_prepared;
	bool c = pm805->capture_prepared;
	unsigned int digital = (p || c) ? 0x0c : 0;
	unsigned int sai = (p || c) ? 0x01 : 0;
	int ret;

	if (p) {
		digital |= 0x01;
		sai |= 0x04;
	}
	if (c) {
		digital |= 0x02;
		sai |= 0x10;
	}

	ret = regmap_update_bits(pm805->regmap,
				 PM805_CODEC_DIGITAL_BLOCK_EN_1, 0x1f, digital);
	if (ret)
		return ret;

	return regmap_update_bits(pm805->regmap,
				  PM805_CODEC_DIGITAL_BLOCK_EN_2, 0x15, sai);
}

static int pm805_power_up(struct pm805_priv *pm805)
{
	unsigned int ready = 0;
	int i, ret;

	if (pm805->powered)
		return 0;

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_MAIN_POWERUP,
				 BIT(0), BIT(0));
	if (ret)
		return ret;
	usleep_range(2000, 3000);

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_DIGITAL_BLOCK_EN_1,
				 0x1d, 0x0d);
	if (ret)
		return ret;

	for (i = 0; i < 100; i++) {
		ret = regmap_read(pm805->regmap,
				  PM805_CODEC_AUTO_SEQUENCE_STS_1, &ready);
		if (ret)
			return ret;
		if (ready & BIT(0))
			break;
		usleep_range(10000, 11000);
	}
	if (!(ready & BIT(0))) {
		dev_err(pm805->dev, "codec power-up timeout (sts=%02x)\n",
			ready);
		return -ETIMEDOUT;
	}

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_REF_GROUP,
				 0x7f, 0x7e);
	if (ret)
		return ret;

	pm805->powered = true;
	return 0;
}

static void pm805_power_down(struct pm805_priv *pm805)
{
	pm805->playback_prepared = false;
	pm805->capture_prepared = false;
	pm805->playback_active = false;

	pm805_hp_sequence(pm805, false);
	pm805->powered = false;

	if (pm805->pmic_regmap)
		regmap_update_bits(pm805->pmic_regmap,
				   PM822_CLASS_D_CTRL, BIT(1), 0);

	regmap_update_bits(pm805->regmap, PM805_CODEC_HEADPHONE_SETTING,
			   0xff, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_REF_GROUP, 0x7f, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_DWS_SETTING, 0x7f, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_MIC_BIAS, 0x07, 0x02);
	pm805_update_micbias(pm805, false);
	regmap_update_bits(pm805->regmap, PM805_CODEC_PDM_SETTING_1, 0xc7, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_PDM_SETTING_2, 0x7f, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_DIGITAL_BLOCK_EN_2,
			   0x15, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_DIGITAL_BLOCK_EN_1,
			   0x1f, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_SAI1_SETTING_1, 0x01, 0);
	regmap_update_bits(pm805->regmap, PM805_CODEC_MAIN_POWERUP, 0x01, 0);
}

static void pm805_drop_stream(struct pm805_priv *pm805, int stream)
{
	if (stream == SNDRV_PCM_STREAM_PLAYBACK) {
		pm805->playback_prepared = false;
		pm805->playback_active = false;
		pm805_update_output_route(pm805);
		regmap_update_bits(pm805->regmap,
				   PM805_CODEC_PDM_SETTING_1, 0xc7, 0);
		regmap_update_bits(pm805->regmap,
				   PM805_CODEC_PDM_SETTING_2, 0x7f, 0);
	} else {
		pm805->capture_prepared = false;
		regmap_update_bits(pm805->regmap,
				   PM805_CODEC_DWS_SETTING, 0x7f, 0);
		regmap_update_bits(pm805->regmap,
				   PM805_CODEC_MIC_BIAS, 0x07, 0x02);
		pm805_update_micbias(pm805, false);
	}

	if (!pm805->playback_prepared && !pm805->capture_prepared)
		pm805_power_down(pm805);
	else
		pm805_sync_blocks(pm805);
}

static int pm805_prepare_playback(struct pm805_priv *pm805)
{
	static const u8 c1[] = { 4, 0x80, 0 };
	static const u8 c2[] = { 13, 0x80, 0 };
	static const u8 c3[] = {
		16, 0x80, 0, 0x80, 0, 0, 0, 0, 0, 0x40, 0, 0x40, 0, 0x40, 0,
		0x40, 0, 0, 0, 0, 0, 0x80, 0, 0x80, 0, 0x80, 0, 0x80, 0, 0x80,
		0, 0x80, 0
	};
	int ret;

	ret = regmap_update_bits(pm805->regmap,
				 PM805_CODEC_VOL_CHANNEL_1_2_SEL, 0xff, 0xee);
	if (ret)
		return ret;

	ret = pm805_hp_gain(pm805, 1);
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap,
				 PM805_CODEC_VOL_CHANNEL_3_4_SEL, 0xf0, 0xf0);
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap,
				 PM805_CODEC_MIX_EQ_COEFFICIENT_2, 0x1f, 0x01);
	if (ret)
		return ret;

	ret = regmap_raw_write(pm805->regmap,
			       PM805_CODEC_MIX_EQ_COEFFICIENT_3, c1, sizeof(c1));
	if (ret)
		return ret;
	ret = regmap_raw_write(pm805->regmap,
			       PM805_CODEC_MIX_EQ_COEFFICIENT_3, c2, sizeof(c2));
	if (ret)
		return ret;
	ret = regmap_raw_write(pm805->regmap,
			       PM805_CODEC_MIX_EQ_COEFFICIENT_3, c3, sizeof(c3));
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap,
				 PM805_CODEC_VOL_CTRL_PARAM_SEL, 0xf0, 0x20);
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap,
				 PM805_CODEC_PDM_SETTING_2, 0x7f, 0x41);
	if (ret)
		return ret;
	ret = regmap_update_bits(pm805->regmap,
				 PM805_CODEC_PDM_SETTING_1, 0xc7, 0xc1);
	if (ret)
		return ret;

	ret = regmap_write(pm805->regmap, PM805_CODEC_VOL_SEL_CHANNEL_3, 1);
	if (ret)
		return ret;

	if (pm805->pmic_regmap) {
		ret = regmap_update_bits(pm805->pmic_regmap,
					 PM822_CLASS_D_MISC1, 0x0c, 0x0c);
		if (ret)
			return ret;
		ret = regmap_update_bits(pm805->pmic_regmap,
					 PM822_CLASS_D_MISC2, 0x60, 0x20);
		if (ret)
			return ret;
		ret = regmap_update_bits(pm805->pmic_regmap,
					 PM822_CLASS_D_CTRL, 0xf6, 0xf0);
		if (ret)
			return ret;
		msleep(20);
	}

	pm805->playback_prepared = true;
	ret = pm805_sync_blocks(pm805);
	if (ret)
		return ret;

	return pm805_update_output_route(pm805);
}

static int pm805_prepare_capture(struct pm805_priv *pm805)
{
	unsigned int dws, pga_reg;
	int ret;

	if (pm805->mic_source == PM805_MIC_HEADSET) {
		ret = pm805_update_micbias(pm805, true);
		if (ret)
			return ret;
		msleep(100);
	}

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_ADCS_SETTING_2,
				 0x0f,
				 pm805->mic_source == PM805_MIC_HEADSET ? 1 : 0);
	if (ret)
		return ret;

	if (pm805->mic_source == PM805_MIC_SECONDARY)
		dws = 0x10;
	else if (pm805->mic_source == PM805_MIC_HEADSET)
		dws = 0x09;
	else
		dws = 0x01;

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_DWS_SETTING,
				 0x7f, dws);
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_ADCS_SETTING_1,
				 0x1f, 0x14);
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_ADC_BIAS,
				 0xff, 0x56);
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_MIC_BIAS, 0x0f,
				 pm805->mic_source == PM805_MIC_HEADSET ? 1 : 3);
	if (ret)
		return ret;

	pga_reg = pm805->mic_source == PM805_MIC_SECONDARY ?
		  PM805_CODEC_ADC_GAIN_1 : PM805_CODEC_ADCS_SETTING_3;
	ret = regmap_update_bits(pm805->regmap, pga_reg, 0x1f,
				 31 - pm805->mic_gain);
	if (ret)
		return ret;

	ret = regmap_update_bits(pm805->regmap, PM805_CODEC_SRC_SETTING_2, 0xff,
				 pm805->mic_source == PM805_MIC_SECONDARY ?
				 0x11 : 0x00);
	if (ret)
		return ret;

	pm805->capture_prepared = true;
	return pm805_sync_blocks(pm805);
}

static int pm805_dai_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	struct pm805_priv *pm805 = pm805_comp_priv(dai->component);
	int ret;

	if (params_rate(params) != 44100 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;

	mutex_lock(&pm805->lock);
	ret = pm805_power_up(pm805);
	if (!ret)
		ret = regmap_update_bits(pm805->regmap,
					 PM805_CODEC_SAI1_SETTING_1,
					 0x3f, 0x0e);
	if (!ret)
		ret = regmap_update_bits(pm805->regmap,
					 PM805_CODEC_SAI1_SETTING_2,
					 0xff, 0x49);
	if (!ret)
		ret = regmap_update_bits(pm805->regmap,
					 PM805_CODEC_SAI1_SETTING_3,
					 0x2f, 0x04);
	mutex_unlock(&pm805->lock);

	return ret;
}

static int pm805_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct pm805_priv *pm805 = pm805_comp_priv(dai->component);
	unsigned int val = 0;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		val |= PM805_SAI_MASTER;
		break;
	default:
		return -EINVAL;
	}

	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) == SND_SOC_DAIFMT_I2S)
		val |= PM805_SAI_I2S_MODE;

	return regmap_update_bits(pm805->regmap, PM805_CODEC_SAI1_SETTING_1,
				  PM805_SAI_MASTER | PM805_SAI_I2S_MODE, val);
}

static int pm805_dai_prepare(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct pm805_priv *pm805 = pm805_comp_priv(dai->component);
	int ret;

	mutex_lock(&pm805->lock);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK &&
	    pm805->playback_prepared)
		pm805_drop_stream(pm805, SNDRV_PCM_STREAM_PLAYBACK);
	else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
		 pm805->capture_prepared)
		pm805_drop_stream(pm805, SNDRV_PCM_STREAM_CAPTURE);

	ret = pm805_power_up(pm805);
	if (ret)
		goto out;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = pm805_prepare_playback(pm805);
	else
		ret = pm805_prepare_capture(pm805);

out:
	if (ret)
		pm805_drop_stream(pm805, substream->stream);
	mutex_unlock(&pm805->lock);
	return ret;
}

static void pm805_dai_shutdown(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct pm805_priv *pm805 = pm805_comp_priv(dai->component);

	mutex_lock(&pm805->lock);
	pm805_drop_stream(pm805, substream->stream);
	mutex_unlock(&pm805->lock);
}

static int pm805_dai_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct pm805_priv *pm805 = pm805_comp_priv(dai->component);
	int ret;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	mutex_lock(&pm805->lock);
	pm805->playback_active = !mute;
	ret = pm805_update_output_route(pm805);
	mutex_unlock(&pm805->lock);

	return ret;
}

static const struct snd_soc_dai_ops pm805_dai_ops = {
	.hw_params = pm805_dai_hw_params,
	.set_fmt = pm805_dai_set_fmt,
	.prepare = pm805_dai_prepare,
	.shutdown = pm805_dai_shutdown,
	.mute_stream = pm805_dai_mute,
};

static struct snd_soc_dai_driver pm805_dai[] = {
	{
		.name = "88pm805-i2s",
		.id = 1,
		.playback = {
			.stream_name = "I2S Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_44100,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.capture = {
			.stream_name = "I2S Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_44100,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &pm805_dai_ops,
	},
};

static const DECLARE_TLV_DB_SCALE(pm805_dac_tlv, -4550, 50, 0);

static int pm805_spk_vol_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);

	mutex_lock(&pm805->lock);
	ucontrol->value.integer.value[0] = pm805->spk_vol;
	mutex_unlock(&pm805->lock);
	return 0;
}

static int pm805_spk_vol_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);
	long val = ucontrol->value.integer.value[0];
	int ret = 0, changed;

	if (val < 0 || val > 91)
		return -EINVAL;

	mutex_lock(&pm805->lock);
	changed = pm805->spk_vol != val;
	pm805->spk_vol = val;
	if (pm805->playback_prepared && pm805->playback_active &&
	    pm805->spk_switch && !pm805->jack_plugged)
		ret = regmap_write(pm805->regmap,
				   PM805_CODEC_VOL_SEL_CHANNEL_3, val + 92);
	mutex_unlock(&pm805->lock);

	return ret ? ret : changed;
}

static int pm805_spk_switch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);

	mutex_lock(&pm805->lock);
	ucontrol->value.integer.value[0] = pm805->spk_switch;
	mutex_unlock(&pm805->lock);
	return 0;
}

static int pm805_spk_switch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);
	bool val = !!ucontrol->value.integer.value[0];
	int ret = 0, changed;

	mutex_lock(&pm805->lock);
	changed = pm805->spk_switch != val;
	pm805->spk_switch = val;
	if (pm805->playback_prepared)
		ret = pm805_update_output_route(pm805);
	mutex_unlock(&pm805->lock);

	return ret ? ret : changed;
}

static int pm805_hp_vol_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);

	mutex_lock(&pm805->lock);
	ucontrol->value.integer.value[0] = pm805->hp_vol;
	mutex_unlock(&pm805->lock);
	return 0;
}

static int pm805_hp_vol_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);
	long val = ucontrol->value.integer.value[0];
	int ret = 0, changed;

	if (val < 0 || val > 91)
		return -EINVAL;

	mutex_lock(&pm805->lock);
	changed = pm805->hp_vol != val;
	pm805->hp_vol = val;
	if (pm805->playback_prepared && pm805->playback_active &&
	    pm805->hp_switch && pm805->jack_plugged)
		ret = pm805_hp_gain(pm805, val + 92);
	mutex_unlock(&pm805->lock);

	return ret ? ret : changed;
}

static int pm805_hp_switch_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);

	mutex_lock(&pm805->lock);
	ucontrol->value.integer.value[0] = pm805->hp_switch;
	mutex_unlock(&pm805->lock);
	return 0;
}

static int pm805_hp_switch_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);
	bool val = !!ucontrol->value.integer.value[0];
	int ret = 0, changed;

	mutex_lock(&pm805->lock);
	changed = pm805->hp_switch != val;
	pm805->hp_switch = val;
	if (pm805->playback_prepared)
		ret = pm805_update_output_route(pm805);
	mutex_unlock(&pm805->lock);

	return ret ? ret : changed;
}

static int pm805_mic_vol_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);

	mutex_lock(&pm805->lock);
	ucontrol->value.integer.value[0] = pm805->mic_gain;
	mutex_unlock(&pm805->lock);
	return 0;
}

static int pm805_mic_vol_put(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);
	long val = ucontrol->value.integer.value[0];
	unsigned int pga_reg;
	int ret = 0, changed;

	if (val < 0 || val > 31)
		return -EINVAL;

	mutex_lock(&pm805->lock);
	changed = pm805->mic_gain != val;
	pm805->mic_gain = val;
	if (pm805->capture_prepared) {
		pga_reg = pm805->mic_source == PM805_MIC_SECONDARY ?
			  PM805_CODEC_ADC_GAIN_1 : PM805_CODEC_ADCS_SETTING_3;
		ret = regmap_update_bits(pm805->regmap, pga_reg, 0x1f,
					 31 - val);
	}
	mutex_unlock(&pm805->lock);

	return ret ? ret : changed;
}

static const char * const pm805_mic_source_texts[] = {
	"Built-in Mic",
	"Mic 2",
	"Headset Mic",
};

static SOC_ENUM_SINGLE_EXT_DECL(pm805_mic_source_enum, pm805_mic_source_texts);

static int pm805_mic_source_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);

	mutex_lock(&pm805->lock);
	ucontrol->value.enumerated.item[0] = pm805->mic_source;
	mutex_unlock(&pm805->lock);
	return 0;
}

static int pm805_mic_source_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *comp = snd_soc_kcontrol_component(kcontrol);
	struct pm805_priv *pm805 = pm805_comp_priv(comp);
	unsigned int val = ucontrol->value.enumerated.item[0];
	int ret = 0, changed;

	if (val >= ARRAY_SIZE(pm805_mic_source_texts))
		return -EINVAL;

	mutex_lock(&pm805->lock);
	if (pm805->mic_source == val) {
		mutex_unlock(&pm805->lock);
		return 0;
	}
	if (pm805->capture_prepared) {
		mutex_unlock(&pm805->lock);
		return -EBUSY;
	}
	changed = pm805->mic_source != val;
	pm805->mic_source = val;
	ret = pm805_update_micbias(pm805, false);
	mutex_unlock(&pm805->lock);

	return ret ? ret : changed;
}

static const struct snd_kcontrol_new pm805_controls[] = {
	SOC_SINGLE_EXT_TLV("Speaker Playback Volume", 0, 0, 91, 0,
			   pm805_spk_vol_get, pm805_spk_vol_put,
			   pm805_dac_tlv),
	SOC_SINGLE_BOOL_EXT("Speaker Playback Switch", 0,
			    pm805_spk_switch_get, pm805_spk_switch_put),
	SOC_SINGLE_EXT_TLV("Headphone Playback Volume", 0, 0, 91, 0,
			   pm805_hp_vol_get, pm805_hp_vol_put,
			   pm805_dac_tlv),
	SOC_SINGLE_BOOL_EXT("Headphone Playback Switch", 0,
			    pm805_hp_switch_get, pm805_hp_switch_put),
	SOC_SINGLE_EXT("Mic Capture Volume", 0, 0, 31, 0,
		       pm805_mic_vol_get, pm805_mic_vol_put),
	SOC_ENUM_EXT("Capture Source", pm805_mic_source_enum,
		     pm805_mic_source_get, pm805_mic_source_put),
};

int pm805_jack_enable(struct snd_soc_component *component, bool enable)
{
	struct pm805_priv *pm805 = pm805_comp_priv(component);

	if (!pm805->pmic_regmap)
		return -ENODEV;

	return regmap_update_bits(pm805->pmic_regmap, PM822_HEADSET_DET_EN,
				  BIT(7), enable ? BIT(7) : 0);
}
EXPORT_SYMBOL_GPL(pm805_jack_enable);

int pm805_jack_get_state(struct snd_soc_component *component)
{
	struct pm805_priv *pm805 = pm805_comp_priv(component);
	unsigned int val = 0;
	bool plugged;
	int ret;

	if (!pm805->pmic_regmap)
		return 0;

	ret = regmap_read(pm805->pmic_regmap, PM822_HEADSET_DET_STS, &val);
	if (ret)
		return ret;

	plugged = !!(val & BIT(7));

	mutex_lock(&pm805->lock);
	if (pm805->jack_plugged != plugged) {
		pm805->jack_plugged = plugged;
		pm805_update_micbias(pm805, false);
		if (pm805->playback_prepared)
			pm805_update_output_route(pm805);
	}
	mutex_unlock(&pm805->lock);

	return plugged ? SND_JACK_HEADPHONE : 0;
}
EXPORT_SYMBOL_GPL(pm805_jack_get_state);

static int pm805_component_probe(struct snd_soc_component *component)
{
	struct pm805_priv *pm805 = pm805_comp_priv(component);

	pm805->component = component;
	return 0;
}

static const struct snd_soc_component_driver soc_component_dev_pm805 = {
	.probe = pm805_component_probe,
	.controls = pm805_controls,
	.num_controls = ARRAY_SIZE(pm805_controls),
};

static int pm805_codec_probe(struct platform_device *pdev)
{
	struct pm80x_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct pm805_priv *pm805;
	struct regulator *micbias;

	if (!chip || !chip->regmap)
		return -EPROBE_DEFER;

	pm805 = devm_kzalloc(&pdev->dev, sizeof(*pm805), GFP_KERNEL);
	if (!pm805)
		return -ENOMEM;

	micbias = devm_regulator_get_optional(pdev->dev.parent, "micbias");
	if (IS_ERR(micbias)) {
		if (PTR_ERR(micbias) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		micbias = NULL;
	}

	pm805->dev = &pdev->dev;
	pm805->chip = chip;
	pm805->regmap = chip->regmap;
	pm805->pmic_regmap = pm805_get_companion_regmap(chip);
	pm805->micbias = micbias;
	pm805->spk_switch = true;
	pm805->hp_switch = true;
	pm805->spk_vol = 55;
	pm805->hp_vol = 31;
	pm805->mic_gain = 13;
	mutex_init(&pm805->lock);

	platform_set_drvdata(pdev, pm805);
	chip->audio_priv = pm805;

	return devm_snd_soc_register_component(pdev->dev.parent,
					       &soc_component_dev_pm805,
					       pm805_dai,
					       ARRAY_SIZE(pm805_dai));
}

static int pm805_codec_remove(struct platform_device *pdev)
{
	struct pm805_priv *pm805 = platform_get_drvdata(pdev);

	mutex_lock(&pm805->lock);
	pm805_power_down(pm805);
	mutex_unlock(&pm805->lock);

	pm805->chip->audio_priv = NULL;
	return 0;
}

static struct platform_driver pm805_codec_driver = {
	.driver = {
		.name = "88pm80x-codec",
	},
	.probe = pm805_codec_probe,
	.remove = pm805_codec_remove,
};
module_platform_driver(pm805_codec_driver);

MODULE_DESCRIPTION("ASoC 88PM805 codec driver");
MODULE_AUTHOR("Xiaofan Tian <tianxf@marvell.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:88pm80x-codec");
