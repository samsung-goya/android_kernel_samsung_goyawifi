// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Siliconfile SR200PC20M 2MP YUV camera sensor driver.
 * Register tables derived from Samsung SM-T110 GPL driver.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "sr200pc20m-regs.h"

struct sr200pc20m {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_ctrl_handler ctrls;
	struct mutex lock;
	struct clk *clk;
	struct gpio_desc *io;
	struct gpio_desc *analog;
	struct gpio_desc *standby;
	struct gpio_desc *reset;
	unsigned int width;
	unsigned int height;
	bool powered;
	bool initialized;
};

static inline struct sr200pc20m *to_sr200pc20m(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sr200pc20m, sd);
}

static int sr200pc20m_write(struct sr200pc20m *s, u16 reg)
{
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);

	return i2c_smbus_write_byte_data(client, reg >> 8, reg & 0xff);
}

static int sr200pc20m_write_table(struct sr200pc20m *s, const u16 *regs, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		if ((regs[i] >> 8) == 0xff) {
			if (regs[i] & 0xff)
				msleep((regs[i] & 0xff) * 10);
			continue;
		}
		ret = sr200pc20m_write(s, regs[i]);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static void sr200pc20m_power_off(struct sr200pc20m *s)
{
	if (!s->powered)
		return;

	gpiod_set_value_cansleep(s->reset, 1);
	msleep(1);
	clk_disable_unprepare(s->clk);
	msleep(5);
	gpiod_set_value_cansleep(s->standby, 0);
	gpiod_set_value_cansleep(s->analog, 0);
	gpiod_set_value_cansleep(s->io, 0);
	s->powered = false;
	s->initialized = false;
}

static int sr200pc20m_power_on(struct sr200pc20m *s)
{
	int ret;

	if (s->powered)
		return 0;

	gpiod_set_value_cansleep(s->io, 1);
	gpiod_set_value_cansleep(s->analog, 1);
	msleep(5);
	gpiod_set_value_cansleep(s->standby, 1);
	msleep(1);

	ret = clk_prepare_enable(s->clk);
	if (ret) {
		gpiod_set_value_cansleep(s->standby, 0);
		gpiod_set_value_cansleep(s->analog, 0);
		gpiod_set_value_cansleep(s->io, 0);
		return ret;
	}

	s->powered = true;
	msleep(40);
	gpiod_set_value_cansleep(s->reset, 1);
	msleep(2);
	gpiod_set_value_cansleep(s->reset, 0);
	msleep(5);

	return 0;
}

static int sr200pc20m_s_power(struct v4l2_subdev *sd, int on)
{
	struct sr200pc20m *s = to_sr200pc20m(sd);
	int ret = 0;

	mutex_lock(&s->lock);
	if (on)
		ret = sr200pc20m_power_on(s);
	else
		sr200pc20m_power_off(s);
	mutex_unlock(&s->lock);

	return ret;
}

static int sr200pc20m_reset(struct v4l2_subdev *sd, u32 val)
{
	return 0;
}

static int sr200pc20m_init(struct v4l2_subdev *sd, u32 val)
{
	struct sr200pc20m *s = to_sr200pc20m(sd);
	int ret;

	mutex_lock(&s->lock);
	ret = s->powered ?
		sr200pc20m_write_table(s, sr_init_regs, ARRAY_SIZE(sr_init_regs)) :
		-EPIPE;
	s->initialized = !ret;
	mutex_unlock(&s->lock);

	return ret;
}

static const unsigned int sr200pc20m_widths[] = { 640, 800, 1600 };
static const unsigned int sr200pc20m_heights[] = { 480, 600, 1200 };

static void sr200pc20m_fill_fmt(struct v4l2_mbus_framefmt *fmt,
				unsigned int width, unsigned int height)
{
	*fmt = (struct v4l2_mbus_framefmt) {
		.width = width,
		.height = height,
		.code = MEDIA_BUS_FMT_UYVY8_2X8,
		.field = V4L2_FIELD_NONE,
		.colorspace = V4L2_COLORSPACE_JPEG,
		.ycbcr_enc = V4L2_YCBCR_ENC_601,
		.quantization = V4L2_QUANTIZATION_FULL_RANGE,
	};
}

static int sr200pc20m_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_format *format)
{
	struct sr200pc20m *s = to_sr200pc20m(sd);
	unsigned int mode;
	int ret = 0;

	if (format->format.width >= 1200)
		mode = 2;
	else if (format->format.width >= 720)
		mode = 1;
	else
		mode = 0;

	sr200pc20m_fill_fmt(&format->format,
			    sr200pc20m_widths[mode],
			    sr200pc20m_heights[mode]);

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		mutex_lock(&s->lock);
		s->width = sr200pc20m_widths[mode];
		s->height = sr200pc20m_heights[mode];
		if (s->powered && s->initialized) {
			if (mode == 2) {
				ret = sr200pc20m_write_table(s, sr_uxga_regs,
						ARRAY_SIZE(sr_uxga_regs));
				/* Retain AWB for live full-resolution capture. */
				if (!ret)
					ret = sr200pc20m_write(s, 0x0322);
				if (!ret)
					ret = sr200pc20m_write(s, 0x10e9);
			} else if (mode == 1) {
				ret = sr200pc20m_write_table(s, sr_svga_regs,
						ARRAY_SIZE(sr_svga_regs));
			} else {
				ret = sr200pc20m_write_table(s, sr_vga_regs,
						ARRAY_SIZE(sr_vga_regs));
			}
		}
		mutex_unlock(&s->lock);
	}

	return ret;
}

static int sr200pc20m_get_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_pad_config *cfg,
			      struct v4l2_subdev_format *format)
{
	struct sr200pc20m *s = to_sr200pc20m(sd);

	sr200pc20m_fill_fmt(&format->format, s->width, s->height);
	return 0;
}

static int sr200pc20m_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_pad_config *cfg,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;

	code->code = MEDIA_BUS_FMT_UYVY8_2X8;
	return 0;
}

static int sr200pc20m_enum_frame_size(struct v4l2_subdev *sd,
				      struct v4l2_subdev_pad_config *cfg,
				      struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(sr200pc20m_widths))
		return -EINVAL;

	fse->min_width = sr200pc20m_widths[fse->index];
	fse->max_width = sr200pc20m_widths[fse->index];
	fse->min_height = sr200pc20m_heights[fse->index];
	fse->max_height = sr200pc20m_heights[fse->index];
	return 0;
}

static int sr200pc20m_enum_frame_interval(struct v4l2_subdev *sd,
					  struct v4l2_subdev_pad_config *cfg,
					  struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->index)
		return -EINVAL;

	fie->interval = (struct v4l2_fract) { 1, 15 };
	return 0;
}

static int sr200pc20m_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sr200pc20m *s = to_sr200pc20m(sd);
	int ret;

	mutex_lock(&s->lock);
	ret = sr200pc20m_write(s, 0x0300);
	if (!ret)
		ret = sr200pc20m_write(s, enable ? 0x0130 : 0x0131);
	mutex_unlock(&s->lock);

	return ret;
}

static int sr200pc20m_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sr200pc20m *s = container_of(ctrl->handler, struct sr200pc20m, ctrls);
	struct i2c_client *client = v4l2_get_subdevdata(&s->sd);
	int val, ret = 0;

	mutex_lock(&s->lock);
	if (s->powered && s->initialized) {
		ret = sr200pc20m_write(s, 0x0300);
		if (!ret) {
			val = i2c_smbus_read_byte_data(client, 0x11);
			if (val < 0)
				ret = val;
			else
				ret = sr200pc20m_write(s,
					0x1100 | ((val & ~2) | (ctrl->val ? 2 : 0)));
		}
	}
	mutex_unlock(&s->lock);

	return ret;
}

static const struct v4l2_ctrl_ops sr200pc20m_ctrl_ops = {
	.s_ctrl = sr200pc20m_s_ctrl,
};

static const struct v4l2_subdev_core_ops sr200pc20m_core_ops = {
	.s_power = sr200pc20m_s_power,
	.reset = sr200pc20m_reset,
	.init = sr200pc20m_init,
};

static const struct v4l2_subdev_video_ops sr200pc20m_video_ops = {
	.s_stream = sr200pc20m_s_stream,
};

static const struct v4l2_subdev_pad_ops sr200pc20m_pad_ops = {
	.get_fmt = sr200pc20m_get_fmt,
	.set_fmt = sr200pc20m_set_fmt,
	.enum_mbus_code = sr200pc20m_enum_mbus_code,
	.enum_frame_size = sr200pc20m_enum_frame_size,
	.enum_frame_interval = sr200pc20m_enum_frame_interval,
};

static const struct v4l2_subdev_ops sr200pc20m_ops = {
	.core = &sr200pc20m_core_ops,
	.video = &sr200pc20m_video_ops,
	.pad = &sr200pc20m_pad_ops,
};

static int sr200pc20m_probe(struct i2c_client *client)
{
	struct sr200pc20m *s;
	int ret, id;

	s = devm_kzalloc(&client->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	mutex_init(&s->lock);
	s->width = 640;
	s->height = 480;
	v4l2_i2c_subdev_init(&s->sd, client, &sr200pc20m_ops);

	s->clk = devm_clk_get(&client->dev, NULL);
	if (IS_ERR(s->clk)) {
		ret = PTR_ERR(s->clk);
		if (ret != -EPROBE_DEFER)
			dev_err(&client->dev, "failed to get MCLK: %d\n", ret);
		return ret;
	}

	s->io = devm_gpiod_get(&client->dev, "io-enable", GPIOD_OUT_LOW);
	if (IS_ERR(s->io))
		return PTR_ERR(s->io);

	s->analog = devm_gpiod_get(&client->dev, "analog-enable", GPIOD_OUT_LOW);
	if (IS_ERR(s->analog))
		return PTR_ERR(s->analog);

	s->standby = devm_gpiod_get(&client->dev, "standby", GPIOD_OUT_LOW);
	if (IS_ERR(s->standby))
		return PTR_ERR(s->standby);

	s->reset = devm_gpiod_get(&client->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s->reset))
		return PTR_ERR(s->reset);

	ret = sr200pc20m_power_on(s);
	if (ret)
		return ret;

	ret = sr200pc20m_write(s, 0x0300);
	id = ret < 0 ? ret : i2c_smbus_read_byte_data(client, 0x04);
	sr200pc20m_power_off(s);

	if (id != 0xb4) {
		dev_err(&client->dev, "unexpected sensor ID %#02x (expected 0xb4)\n", id);
		return id < 0 ? id : -ENODEV;
	}

	v4l2_ctrl_handler_init(&s->ctrls, 1);
	v4l2_ctrl_new_std(&s->ctrls, &sr200pc20m_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);
	s->sd.ctrl_handler = &s->ctrls;
	if (s->ctrls.error) {
		ret = s->ctrls.error;
		goto err_ctrls;
	}

	s->pad.flags = MEDIA_PAD_FL_SOURCE;
	s->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&s->sd.entity, 1, &s->pad);
	if (ret)
		goto err_ctrls;

	ret = v4l2_async_register_subdev(&s->sd);
	if (ret) {
		media_entity_cleanup(&s->sd.entity);
		goto err_ctrls;
	}

	dev_info(&client->dev, "SR200PC20M sensor (ID 0xb4) registered\n");
	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&s->ctrls);
	return ret;
}

static int sr200pc20m_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sr200pc20m *s = to_sr200pc20m(sd);

	v4l2_async_unregister_subdev(sd);
	sr200pc20m_power_off(s);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&s->ctrls);

	return 0;
}

static const struct of_device_id sr200pc20m_of_match[] = {
	{ .compatible = "siliconfile,sr200pc20m" },
	{ }
};
MODULE_DEVICE_TABLE(of, sr200pc20m_of_match);

static struct i2c_driver sr200pc20m_driver = {
	.probe_new = sr200pc20m_probe,
	.remove = sr200pc20m_remove,
	.driver = {
		.name = "sr200pc20m",
		.of_match_table = sr200pc20m_of_match,
	},
};
module_i2c_driver(sr200pc20m_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Siliconfile SR200PC20M 2MP camera sensor driver");
