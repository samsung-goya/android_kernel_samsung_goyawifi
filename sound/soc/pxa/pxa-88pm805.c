// SPDX-License-Identifier: GPL-2.0-only
/*
 * pxa-88pm805.c -- ASoC machine driver for Marvell PXA988/PXA986 + 88PM805
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/88pm805-codec.h"
#include "pxa-ssp.h"

#define PXA988_ISCCR1_44100	0xea948be2

struct pxa_88pm805_priv {
	struct snd_soc_card card;
	struct snd_soc_dai_link dai_link;
	struct snd_soc_jack hp_jack;
	struct snd_soc_component *codec_comp;
	struct delayed_work jack_work;
	void __iomem *isccr;
	int last_jack_state;
	bool stopping;
};

static struct snd_soc_jack_pin pxa_88pm805_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static void pxa_88pm805_jack_poll(struct work_struct *work)
{
	struct pxa_88pm805_priv *priv =
		container_of(to_delayed_work(work), struct pxa_88pm805_priv,
			     jack_work);
	int state;

	if (priv->codec_comp) {
		state = pm805_jack_get_state(priv->codec_comp);
		if (state >= 0 && state != priv->last_jack_state) {
			priv->last_jack_state = state;
			snd_soc_jack_report(&priv->hp_jack, state,
					    SND_JACK_HEADPHONE);
		}
	}

	if (!READ_ONCE(priv->stopping))
		schedule_delayed_work(&priv->jack_work,
				      msecs_to_jiffies(250));
}

static int pxa_88pm805_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct pxa_88pm805_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	if (priv->isccr)
		writel(PXA988_ISCCR1_44100, priv->isccr);

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S |
				  SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, PXA_SSP_CLK_AUDIO, 0,
				     SND_SOC_CLOCK_OUT);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_tdm_slot(cpu_dai, 3, 3, 2, 16);
	if (ret < 0)
		return ret;

	return snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S |
				   SND_SOC_DAIFMT_NB_NF |
				   SND_SOC_DAIFMT_CBS_CFS);
}

static void pxa_88pm805_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct pxa_88pm805_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	if (priv->isccr && !rtd->cpu_dai->active)
		writel(readl(priv->isccr) & ~(BIT(31) | BIT(29)), priv->isccr);
}

static const struct snd_soc_ops pxa_88pm805_ops = {
	.hw_params = pxa_88pm805_hw_params,
	.shutdown = pxa_88pm805_shutdown,
};

static int pxa_88pm805_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct pxa_88pm805_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	priv->codec_comp = rtd->codec_dai->component;

	ret = snd_soc_card_jack_new(rtd->card, "Headphone Jack",
				    SND_JACK_HEADPHONE, &priv->hp_jack,
				    pxa_88pm805_jack_pins,
				    ARRAY_SIZE(pxa_88pm805_jack_pins));
	if (ret)
		return ret;

	pm805_jack_enable(priv->codec_comp, true);
	schedule_delayed_work(&priv->jack_work, 0);

	return 0;
}

static const struct snd_soc_dapm_widget pxa_88pm805_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Built-in Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static int pxa_88pm805_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct pxa_88pm805_priv *priv;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res) {
		priv->isccr = devm_ioremap_resource(dev, res);
		if (IS_ERR(priv->isccr))
			return PTR_ERR(priv->isccr);
	}

	INIT_DELAYED_WORK(&priv->jack_work, pxa_88pm805_jack_poll);
	priv->last_jack_state = -1;

	priv->dai_link.name = "88pm805 i2s";
	priv->dai_link.stream_name = "88PM805 HiFi";
	priv->dai_link.codec_dai_name = "88pm805-i2s";
	priv->dai_link.ops = &pxa_88pm805_ops;
	priv->dai_link.init = pxa_88pm805_dai_init;

	priv->dai_link.cpu_of_node = of_parse_phandle(np, "cpu-dai", 0);
	priv->dai_link.codec_of_node = of_parse_phandle(np, "audio-codec", 0);
	priv->dai_link.platform_of_node = of_parse_phandle(np, "platform", 0);
	if (!priv->dai_link.cpu_of_node || !priv->dai_link.codec_of_node ||
	    !priv->dai_link.platform_of_node) {
		dev_err(dev, "missing cpu-dai, audio-codec, or platform phandle\n");
		ret = -EINVAL;
		goto err_put;
	}

	priv->card.name = "PXA-88PM805";
	priv->card.driver_name = "pxa-88pm805";
	priv->card.dev = dev;
	priv->card.owner = THIS_MODULE;
	priv->card.dai_link = &priv->dai_link;
	priv->card.num_links = 1;
	priv->card.dapm_widgets = pxa_88pm805_widgets;
	priv->card.num_dapm_widgets = ARRAY_SIZE(pxa_88pm805_widgets);

	snd_soc_card_set_drvdata(&priv->card, priv);

	ret = devm_snd_soc_register_card(dev, &priv->card);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to register sound card: %d\n", ret);
		goto err_put;
	}

	return 0;

err_put:
	of_node_put(priv->dai_link.cpu_of_node);
	of_node_put(priv->dai_link.codec_of_node);
	of_node_put(priv->dai_link.platform_of_node);
	return ret;
}

static int pxa_88pm805_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct pxa_88pm805_priv *priv = snd_soc_card_get_drvdata(card);

	WRITE_ONCE(priv->stopping, true);
	cancel_delayed_work_sync(&priv->jack_work);
	of_node_put(priv->dai_link.cpu_of_node);
	of_node_put(priv->dai_link.codec_of_node);
	of_node_put(priv->dai_link.platform_of_node);
	return 0;
}

static const struct of_device_id pxa_88pm805_of_match[] = {
	{ .compatible = "marvell,pxa-88pm805-snd-card" },
	{ }
};
MODULE_DEVICE_TABLE(of, pxa_88pm805_of_match);

static struct platform_driver pxa_88pm805_driver = {
	.driver = {
		.name = "pxa-88pm805",
		.of_match_table = pxa_88pm805_of_match,
	},
	.probe = pxa_88pm805_probe,
	.remove = pxa_88pm805_remove,
};
module_platform_driver(pxa_88pm805_driver);

MODULE_DESCRIPTION("ALSA SoC PXA988/88PM805 machine driver");
MODULE_LICENSE("GPL");
