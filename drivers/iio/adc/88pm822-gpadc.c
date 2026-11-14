// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell 88PM822 GPADC IIO driver
 *
 * Exposes VBAT (instantaneous and averaged) and GPADC2/GPADC3 channels
 * from the 88PM822 GPADC I2C page via the standard IIO subsystem.
 */

#include <linux/iio/iio.h>
#include <linux/mfd/88pm80x.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define PM822_VBAT_MEAS1	0x42
#define PM822_VBAT_AVG1		0xa0
#define PM822_GPADC2_MEAS1	0x58
#define PM822_GPADC3_MEAS1	0x5a

struct pm822_gpadc {
	struct regmap *map;
};

#define PM822_GPADC_CHAN(_idx, _addr, _name) {			\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = _idx,					\
	.address = _addr,					\
	.datasheet_name = _name,				\
	.extend_name = _name,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
			      BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec pm822_gpadc_channels[] = {
	PM822_GPADC_CHAN(0, PM822_VBAT_MEAS1, "vbat"),
	PM822_GPADC_CHAN(1, PM822_VBAT_AVG1, "vbat_avg"),
	PM822_GPADC_CHAN(2, PM822_GPADC2_MEAS1, "gpadc2"),
	PM822_GPADC_CHAN(3, PM822_GPADC3_MEAS1, "gpadc3"),
};

static int pm822_gpadc_read_raw(struct iio_dev *indio_dev,
				struct iio_chan_spec const *chan,
				int *val, int *val2, long mask)
{
	struct pm822_gpadc *adc = iio_priv(indio_dev);
	u8 buf[2];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_bulk_read(adc->map, chan->address, buf, 2);
		if (ret)
			return ret;
		*val = (buf[0] << 4) | (buf[1] & 0x0f);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* VBAT full-scale is 5.6V (12-bit); GPADCx full-scale is 1.4V */
		*val = (chan->channel <= 1) ? 5600 : 1400;
		*val2 = 12;
		return IIO_VAL_FRACTIONAL_LOG2;

	default:
		return -EINVAL;
	}
}

static const struct iio_info pm822_gpadc_info = {
	.read_raw = pm822_gpadc_read_raw,
};

static int pm822_gpadc_probe(struct platform_device *pdev)
{
	struct pm80x_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct pm822_gpadc *adc;
	struct iio_dev *indio_dev;

	if (!chip || !chip->subchip || !chip->subchip->regmap_gpadc)
		return -ENODEV;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->map = chip->subchip->regmap_gpadc;

	indio_dev->dev.parent = &pdev->dev;
	indio_dev->dev.of_node = pdev->dev.of_node;
	indio_dev->name = "88pm822-gpadc";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &pm822_gpadc_info;
	indio_dev->channels = pm822_gpadc_channels;
	indio_dev->num_channels = ARRAY_SIZE(pm822_gpadc_channels);

	return devm_iio_device_register(&pdev->dev, indio_dev);
}

static const struct of_device_id pm822_gpadc_of_match[] = {
	{ .compatible = "marvell,88pm822-gpadc" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm822_gpadc_of_match);

static struct platform_driver pm822_gpadc_driver = {
	.driver = {
		.name = "88pm822-gpadc",
		.of_match_table = pm822_gpadc_of_match,
	},
	.probe = pm822_gpadc_probe,
};
module_platform_driver(pm822_gpadc_driver);

MODULE_DESCRIPTION("Marvell 88PM822 GPADC IIO driver");
MODULE_LICENSE("GPL");
