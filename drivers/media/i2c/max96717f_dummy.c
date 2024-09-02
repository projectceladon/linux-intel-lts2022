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

#define to_max96717f_dummy(_sd) container_of(_sd, struct max96717f_dummy_priv, sd)

#define MAX96717F_DUMMY_N_PADS 1

struct max96717f_dummy_mode {
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

static const struct max96717f_dummy_mode supported_modes[] = {
	{
		.width = 1920,
		.height = 1080,
		.fps = 30,
		.code = MEDIA_BUS_FMT_UYVY8_1X16,

		.vc = 0,
		.dt = 0x1e,
		.bpp = 10,

		.pipe = 2,
	},
	{
		.width = 1280,
		.height = 960,
		.fps = 30,
		.code = MEDIA_BUS_FMT_UYVY8_1X16,

		.vc = 0,
		.dt = 0x1e,
		.bpp = 10,

		.pipe = 2,
	},
};

struct max96717f_dummy_priv {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrls;

	const struct max96717f_dummy_mode *mode;

	struct mutex mutex;

	bool streaming;

	struct max9671x_subdev_platform_data *platform_data;
};

static int max96717f_dummy_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96717f_dummy_priv *priv = to_max96717f_dummy(sd);

	if (priv->streaming == enable)
		return 0;

	mutex_lock(&priv->mutex);

	priv->streaming = enable;

	mutex_unlock(&priv->mutex);

	return 0;
}

static void max96717f_dummy_init_format(const struct max96717f_dummy_mode *mode,
		struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = mode->code;
	fmt->field = V4L2_FIELD_ANY;
}

static int max96717f_dummy_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{
	struct max96717f_dummy_priv *priv = to_max96717f_dummy(sd);

	mutex_lock(&priv->mutex);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, sd_state, format->pad);
		format->format = *framefmt;
	} else {
		max96717f_dummy_init_format(priv->mode, &format->format);
	}

	mutex_unlock(&priv->mutex);

	return 0;
}

static int max96717f_dummy_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{

	struct max96717f_dummy_priv *priv = to_max96717f_dummy(sd);

	mutex_lock(&priv->mutex);

	max96717f_dummy_init_format(priv->mode, &format->format);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, sd_state, format->pad);

		*framefmt = format->format;
	}

	mutex_unlock(&priv->mutex);

	return 0;
}

static const struct v4l2_subdev_video_ops max96717f_dummy_video_ops = {
	.s_stream = max96717f_dummy_s_stream,
};

static int max96717f_dummy_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct max96717f_dummy_priv *priv = to_max96717f_dummy(sd);
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_get_try_format(sd, fh->state, 0);
	max96717f_dummy_init_format(priv->mode, fmt);

	return 0;
}

static const struct v4l2_subdev_pad_ops max96717f_dummy_pad_ops = {
	.get_fmt = max96717f_dummy_get_fmt,
	.set_fmt = max96717f_dummy_set_fmt,
};

static const struct v4l2_subdev_ops max96717f_dummy_subdev_ops = {
	.video = &max96717f_dummy_video_ops,
	.pad = &max96717f_dummy_pad_ops,
};

static const struct v4l2_subdev_internal_ops max96717f_dummy_internal_ops = {
	.open = max96717f_dummy_open,
};

static int max96717f_dummy_init(struct max96717f_dummy_priv *priv)
{

	return 0;
}

static int max96717f_dummy_probe(struct i2c_client *client)
{
	struct max96717f_dummy_priv *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;

	priv->platform_data = client->dev.platform_data;

	mutex_init(&priv->mutex);

	v4l2_i2c_subdev_init(&priv->sd, client, &max96717f_dummy_subdev_ops);
	snprintf(priv->sd.name, sizeof(priv->sd.name), "max96717 %c", priv->platform_data->suffix);
	priv->sd.internal_ops = &max96717f_dummy_internal_ops;
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;

	v4l2_ctrl_handler_init(&priv->ctrls, 1);

	priv->sd.ctrl_handler = &priv->ctrls;

	priv->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	priv->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&priv->sd.entity, 1, &priv->pad);

	if (ret) {
		dev_err(&client->dev, "media entity init failed %d\n", ret);
		return ret;
	}

	priv->mode = &supported_modes[priv->platform_data->mode];

	ret = max96717f_dummy_init(priv);
	if (ret) {
		dev_err(&client->dev, "Failed to init device %d\n", ret);
		goto probe_err;
	}

	return 0;

probe_err:
	media_entity_cleanup(&priv->sd.entity);
	mutex_destroy(&priv->mutex);

	return ret;
}

static void max96717f_dummy_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max96717f_dummy_priv *priv = to_max96717f_dummy(sd);

	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
	mutex_destroy(&priv->mutex);

	return;
}


static const struct i2c_device_id max96717f_dummy_id_table[] = {
	{"max96717f_dummy", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, max96717f_dummy_id_table);


static struct i2c_driver max96717f_dummy_i2c_driver = {
	.driver = {
		.name = "max96717f_dummy",
	},
	.probe_new = max96717f_dummy_probe,
	.remove = max96717f_dummy_remove,
	.id_table = max96717f_dummy_id_table,
};
module_i2c_driver(max96717f_dummy_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX96717F GMSL Serializer With SOC Sensor Driver");
MODULE_AUTHOR("Intel");
MODULE_LICENSE("GPL");
