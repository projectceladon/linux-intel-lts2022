// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2022 Intel Corporation.

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/regmap.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/max9671x.h>

#define to_max9295e_dummy(_sd) container_of(_sd, struct max9295e_dummy_priv, sd)

#define MAX9295e_DUMMY_N_PADS 1

struct max9295e_dummy_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 fps;

	/* MIPI CSI-2 */
	u8 vc;
	u8 dt;
	u8 bpp;

	/* x/y/z/u */
	u8 pipe;
};

static const struct max9295e_dummy_mode supported_modes[] = {
	{
		.width = 1600,
		.height = 1300,
		.fps = 30,
		.code = MEDIA_BUS_FMT_UYVY8_1X16,

		.vc = 0,
		.dt = 0x1e,
		.bpp = 10,

		.pipe = 0,
	},
};

/*
 * 16 bit addr 8 bit val
 */
static struct regmap_config config16 = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
};

struct max9295e_dummy_priv {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct regmap *regmap16;

	struct v4l2_ctrl_handler ctrls;

	const struct max9295e_dummy_mode *mode;

	struct mutex mutex;

	bool streaming;

	struct max9671x_subdev_platform_data *platform_data;
};

#if 0
static int max9295e_dummy_read(struct max9295e_dummy_priv *priv, u32 reg, u32 *val)
{
	int ret;

	ret = regmap_read(priv->regmap16, reg, val);

	if (ret) {
		dev_err(&priv->client->dev,
				"%s : register 0x%02x read failed (%d)\n",
				__func__, reg, ret);
	}

	return ret;
}

static int max9295e_dummy_write(struct max9295e_dummy_priv *priv, u32 reg, u32 val)
{
	int ret;

	ret = regmap_write(priv->regmap16, reg, val);

	if (ret) {
		dev_err(&priv->client->dev,
				"%s : register 0x%02x write failed (%d)\n",
				__func__, reg, ret);
	}

	return ret;
}
#endif
static int max9295e_dummy_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max9295e_dummy_priv *priv = to_max9295e_dummy(sd);
	int ret;

	if (priv->streaming == enable)
		return 0;

	mutex_lock(&priv->mutex);

	/* set GPIO8 as GPO and in reverse direction to control streaming*/
	//if (enable)
	//	ret = max9295e_dummy_write(priv, 0x2d6, 0x84);
	//else
	//	ret = max9295e_dummy_write(priv, 0x2d6, 0x80);

	priv->streaming = enable;

	mutex_unlock(&priv->mutex);

	if (ret)
		dev_err(&priv->client->dev, "%s : Failed to s_stream %d\n", __func__, ret);

	return ret;
}

static void max9295e_dummy_init_format(const struct max9295e_dummy_mode *mode,
		struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = mode->code;
	fmt->field = V4L2_FIELD_ANY;
}

/* fixed format */
static int max9295e_dummy_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{
	struct max9295e_dummy_priv *priv = to_max9295e_dummy(sd);

	mutex_lock(&priv->mutex);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, sd_state, format->pad);
		format->format = *framefmt;
	} else {
		max9295e_dummy_init_format(priv->mode, &format->format);
	}

	mutex_unlock(&priv->mutex);

	return 0;
}

static int max9295e_dummy_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{

	struct max9295e_dummy_priv *priv = to_max9295e_dummy(sd);

	mutex_lock(&priv->mutex);

	max9295e_dummy_init_format(priv->mode, &format->format);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, sd_state, format->pad);

		*framefmt = format->format;
	}

	mutex_unlock(&priv->mutex);

	return 0;
}

static const struct v4l2_subdev_video_ops max9295e_dummy_video_ops = {
	.s_stream = max9295e_dummy_s_stream,
};

static int max9295e_dummy_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct max9295e_dummy_priv *priv = to_max9295e_dummy(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_get_try_format(sd, fh->state, 0);
	max9295e_dummy_init_format(priv->mode, fmt);

	return 0;
}

static const struct v4l2_subdev_pad_ops max9295e_dummy_pad_ops = {
	.get_fmt = max9295e_dummy_get_fmt,
	.set_fmt = max9295e_dummy_set_fmt,
};

static const struct v4l2_subdev_ops max9295e_dummy_subdev_ops = {
	.video = &max9295e_dummy_video_ops,
	.pad = &max9295e_dummy_pad_ops,
};

static const struct v4l2_subdev_internal_ops max9295e_dummy_internal_ops = {
	.open = max9295e_dummy_open,
};

static int max9295e_dummy_init(struct max9295e_dummy_priv *priv)
{
	//int ret;
	u32 val = 0;

	//ret = max9295e_dummy_read(priv, 0x0d, &val);
	//if (ret) {
	//	dev_err(&priv->client->dev, "Failed to get DEV_ID, slave addr 0x%x\n", priv->client->addr);
	//	return ret;
	//}

	dev_info(&priv->client->dev, "Got DEV_ID 0x%0x\n", val);

	return 0;
}

static int max9295e_dummy_probe(struct i2c_client *client)
{
	struct max9295e_dummy_priv *priv;
	int ret;
	printk("max9295e_dummy_probe================+\n");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;

	priv->regmap16 = devm_regmap_init_i2c(client, &config16);
	if (IS_ERR(priv->regmap16)) {
		dev_err(&client->dev, "Failed to init regmap\n");
		return -EIO;
	}

	mutex_init(&priv->mutex);
	priv->platform_data = client->dev.platform_data;

	v4l2_i2c_subdev_init(&priv->sd, priv->client, &max9295e_dummy_subdev_ops);

	snprintf(priv->sd.name, sizeof(priv->sd.name), "max9295e %c", priv->platform_data->suffix);
	priv->sd.internal_ops = &max9295e_dummy_internal_ops;
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;

	v4l2_ctrl_handler_init(&priv->ctrls, 1);

	priv->sd.ctrl_handler = &priv->ctrls;

	priv->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	priv->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&priv->sd.entity, 1, &priv->pad);
	if (ret < 0) {
		dev_err(&client->dev,
				"%s : media entity init failed %d\n", __func__, ret);
		return ret;
	}

	priv->mode = &supported_modes[0];

	ret = max9295e_dummy_init(priv);
	if (ret) {
		dev_err(&client->dev, "Failed to init device\n");
		goto probe_err;
	}
	printk("max9295e_dummy_probe================-\n");

	return 0;

probe_err:
	media_entity_cleanup(&priv->sd.entity);
	mutex_destroy(&priv->mutex);

	return ret;
}

static void max9295e_dummy_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max9295e_dummy_priv *priv = to_max9295e_dummy(sd);

	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
	mutex_destroy(&priv->mutex);

	return;
}

static const struct i2c_device_id max9295e_dummy_id_table[] = {
	{"max9295e_dummy", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, max9295e_dummy_id_table);


static struct i2c_driver max9295e_dummy_i2c_driver = {
	.driver = {
		.name = "max9295e_dummy",
	},
	.probe_new = max9295e_dummy_probe,
	.remove = max9295e_dummy_remove,
	.id_table = max9295e_dummy_id_table,
};
module_i2c_driver(max9295e_dummy_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX9295E GMSL Serializer With SOC Sensor Driver");
MODULE_AUTHOR("Intel");
MODULE_LICENSE("GPL");
