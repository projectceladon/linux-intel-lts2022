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
#include <linux/ipu-isys.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>

#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include <media/max9671x.h>


#define to_max96724(_sd) container_of(_sd, struct max96724_priv, sd)

#define MAX96724_LINK_STATUS \
	(max96724_get_locked_status(priv, MAX_PORT_SIOA) << MAX_PORT_SIOA \
	| max96724_get_locked_status(priv, MAX_PORT_SIOB) << MAX_PORT_SIOB \
	| max96724_get_locked_status(priv, MAX_PORT_SIOC) << MAX_PORT_SIOC \
	| max96724_get_locked_status(priv, MAX_PORT_SIOD) << MAX_PORT_SIOD)


#define MAX96724_N_SINKS 4
#define MAX96724_N_PADS 5
#define MAX96724_SRC_PAD 4

#define DELAY_MS 100

struct max96724_reg {
	u16 address;
	u8 val;
};

struct max96724_reg_list {
	u32 num_of_regs;
	const struct max96724_reg *regs;
};

static struct regmap_config config16 = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
};

/*
 * 1280x960 UYVY
 */
#if 0
static s64 max96724_query_sub_stream[] = {
	0x001e050003c0200f, 0x011e050003c0200f,
	0x021e050003c0200f, 0x031e050003c0200f,
};
#endif

static s64 max96724_query_sub_stream[] = {
	0x001e06400514200f, 0x011e06400514200f,
	0x001e06400514200f, 0x011e06400514200f,
};

#define MIPI_CSI2_TYPE_YUV422_8 0x1e
static unsigned int mbus_code_to_mipi(u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_UYVY8_1X16:
		return MIPI_CSI2_TYPE_YUV422_8;
	default:
		WARN_ON(1);
		return -EINVAL;
	}
}

static const struct max96724_reg desay_init[] = {
	{0x0003,0xAA},
	{0x00F0,0x62},
	{0x00F1,0xC8},
	{0x00F4,0x0F},
	{0x041A,0xC0},
	{0x040B,0x00},
	{0x040F,0x70},
	{0x0410,0x7A},
	{0x0411,0x40},
	{0x0412,0x20},
	{0x090B,0x07},
	{0x092D,0x15},
	{0x090D,0x2C},
	{0x090E,0x2C},
	{0x090F,0x00},
	{0x0910,0x00},
	{0x0911,0x01},
	{0x0912,0x01},
	{0x094B,0x07},
	{0x096D,0x15},
	{0x094D,0x2C},
	{0x094E,0x6C},
	{0x094F,0x00},
	{0x0950,0x40},
	{0x0951,0x01},
	{0x0952,0x41},
	{0x098B,0x07},
	{0x09AD,0x15},
	{0x098D,0x1E},
	{0x098E,0x9E},
	{0x098F,0x00},
	{0x0990,0x80},
	{0x0991,0x01},
	{0x0992,0x81},
	{0x09CB,0x07},
	{0x09ED,0x15},
	{0x09CD,0x1E},
	{0x09CE,0xDE},
	{0x09CF,0x00},
	{0x09D0,0xC0},
	{0x09D1,0x01},
	{0x09D2,0xC1},
	{0x08A0,0x04},
	{0x08A3,0xE4},
	{0x08A4,0xE4},
	{0x090A,0xC0},
	{0x094A,0xC0},
	{0x098A,0xC0},
	{0x09CA,0xC0},
	{0x08A2,0x04},
	{0x1C00,0xF4},
	{0x1D00,0xF4},
	{0x1E00,0xF4},
	{0x1F00,0xF4},
	{0x0415,0x39},
	{0x0418,0xF9},
	{0x041B,0x39},
	{0x041E,0x39},
	{0x1C00,0xF5},
	{0x1D00,0xF5},
	{0x1E00,0xF5},
	{0x1F00,0xF5},
};
static const struct max96724_reg_list desay_init_setting = {
	.num_of_regs = ARRAY_SIZE(desay_init),
	.regs = desay_init,
};

static const struct max96724_reg desay_init_dms[] = {
	{0x0017,0x14},
	{0x0019,0x94},
	{0x06c2,0x10},
	{0x1401,0x03},
	{0x1501,0x03},
	{0x1601,0x03},
	{0x1701,0x03},
	{0x1445,0x00},
	{0x1545,0x00},
	{0x1645,0x00},
	{0x1745,0x00},
	{0x0010,0x11},
	{0x0011,0x11},
	{0x0006,0x11},
	{0x0018,0x0F},
	{0x0003,0xFE},
	{0x0003,0xAA},
	{0x00F0,0x60},
	{0x00F1,0xEA},
	{0x00F4,0x0F},
	{0x090B,0x07},
	{0x092D,0x15},
	{0x090D,0x1E},
	{0x090E,0x1E},
	{0x090F,0x00},
	{0x0910,0x00},
	{0x0911,0x01},
	{0x0912,0x01},
	{0x094B,0x07},
	{0x096D,0x2A},
	{0x094D,0x1E},
	{0x094E,0x1E},
	{0x094F,0x00},
	{0x0950,0x00},
	{0x0951,0x01},
	{0x0952,0x01},
	{0x08A0,0x04},
	{0x08A3,0xE4},
	{0x08A4,0xE4},
	{0x094A,0xC0},
	{0x090A,0xC0},
	{0x098A,0xC0},
	{0x09CA,0xC0},
	{0x08A2,0xF0},
	{0x1C00,0xF4},
	{0x1D00,0xF4},
	{0x1E00,0xF4},
	{0x1F00,0xF4},
	{0x0415,0x2C},
	{0x0418,0x2C},
	{0x041B,0x2C},
	{0x041E,0x2C},
	{0x1c00,0xF5},
	{0x1D00,0xF5},
	{0x1E00,0xF5},
	{0x1F00,0xF5},
	{0x040B,0x00},
};

static const struct max96724_reg_list desay_init_dms_setting = {
	.num_of_regs = ARRAY_SIZE(desay_init_dms),
	.regs = desay_init_dms,
};

/*
 * FSYNC_MODE 2b01 FSYNC_METH 2b00
 * 25Mhz XTAL, 30fps, TX_ID 8
 * OVLP window 0
 * enable fsync on pipe 1
 */
static const struct max96724_reg fsync_30fps[] = {
	{0x04a0, 0x04},
	{0x04a2, 0x00},
	{0x04aa, 0x00},
	{0x04ab, 0x00},
	{0x04af, 0xcf},
	{0x04a7, 0x0c},
	{0x04a6, 0xb7},
	{0x04a5, 0x35},
	{0x04b1, 0x40},
};
static const struct max96724_reg_list fsync_setting = {
	.num_of_regs = ARRAY_SIZE(fsync_30fps),
	.regs = fsync_30fps,
};

/*
 * disable CSI out
 * 2x4
 * 800MBps
 * 4lanes
 * DPHY auto initial deskew on
 * lanes swapped matches pin
 * enable PHY 0/1/2/3
 * write to 0x40b to enable csi out
 */
static const struct max96724_reg csi_phy[] = {
	{0x040b, 0x00},
	{0x08a0, 0x04},
	{0x08a3, 0xe4},
	{0x094a, 0xc0},
	{0x1d00, 0xf4},
//	{0x0943, 0x80},
	{0x0418, 0x28},
	{0x1d00, 0xf5},
	{0x08a2, 0xf0},
};
static const struct max96724_reg_list mipi_phy_setting = {
	.num_of_regs = ARRAY_SIZE(csi_phy),
	.regs = csi_phy,
};

/*
 * link a pipe z -> pipe 0
 * link b pipe z -> pipe 1
 * link c pipe z -> pipe 2
 * link d pipe z -> pipe 3
 * enable pipe 0/1/2/3
 */
static const struct max96724_reg video_pipe_sel[] = {
	{0x00f0, 0x62},
	{0x00f1, 0xea},
	{0x00f4, 0x0f},
};
static const struct max96724_reg_list video_pipe_setting = {
	.num_of_regs = ARRAY_SIZE(video_pipe_sel),
	.regs = video_pipe_sel,
};

/*
 * pipe 0 vc0
 * FS/DATA/FE identity mapping
 * to csi ctrl 1
 * pipe 1 vc1
 * FS/DATA/FE identity mapping
 * to csi ctrl 1
 * pipe 2 vc2
 * FS/DATA/FE identity mapping
 * to csi ctrl 1
 * pipe 3 vc3
 * FS/DATA/FE identity mapping
 * to csi ctrl 1
 */
static const struct max96724_reg video_pipe_to_csi_ctrl_mapping[] = {
	{0x090b, 0x07},
	{0x090d, 0x00},
	{0x090e, 0x00},
	{0x090f, 0x1e},
	{0x0910, 0x1e},
	{0x0911, 0x01},
	{0x0912, 0x01},
	{0x092d, 0x15},
	/* pipe 1 */
	{0x094b, 0x07},
	{0x094d, 0x00},
	{0x094e, 0x40},
	{0x094f, 0x1e},
	{0x0950, 0x5e},
	{0x0951, 0x01},
	{0x0952, 0x41},
	{0x096d, 0x15},
	/* pipe 2 */
	{0x098b, 0x07},
	{0x098d, 0x00},
	{0x098e, 0x80},
	{0x098f, 0x1e},
	{0x0990, 0x9e},
	{0x0991, 0x01},
	{0x0992, 0x81},
	{0x09ad, 0x15},
	/* pipe 3 */
	{0x09cb, 0x07},
	{0x09cd, 0x00},
	{0x09ce, 0xc0},
	{0x09cf, 0x1e},
	{0x09d0, 0xde},
	{0x09d1, 0x01},
	{0x09d2, 0xc1},
	{0x09ed, 0x15},
};

static const struct max96724_reg_list mipi_ctrl_setting = {
	.num_of_regs = ARRAY_SIZE(video_pipe_to_csi_ctrl_mapping),
	.regs = video_pipe_to_csi_ctrl_mapping,
};

/*
 * 2lanes
 * lane swapping
 * GPIO 8 fsync
 * Route YUV422 8bit to pipe
 * RLMS
 */
static const struct max96724_reg link_abcd_default[] = {
	/* disable local CC */
//	{0x0001, 0xe4},
	{0x0330, 0x00},
	{0x0331, 0x11},
	{0x0332, 0xE0},
	{0x0333, 0x04},
	{0x0318, 0x5E},
	{0x02D6, 0x84},
	{0x1417, 0x00},
	{0x1432, 0x7F},
};

static const struct max96724_reg link_abcd_default_9295[] = {
	{0x0100,0xF2},
	{0x0101,0x4A},
	{0x0002,0x13},
	{0x0007,0x07},
	{0x03F0,0x51},
	{0x03F1,0x05},
	{0x02D6,0x00},
	{0x02C1,0x10},
};

static const struct max96724_reg link_abcd_default_9295_dms[] = {
	{0x02df,0x00},//sleep 1
	{0x0330,0x0c},//sleep 1
	{0x0330,0x84},//sleep 1
	{0x0309,0x01},
	{0x030a,0x00},
	{0x0314,0x5e},
	{0x0315,0x80},
	{0x02df,0x10},
};

static const struct max96724_reg_list link_setting = {
	.num_of_regs = ARRAY_SIZE(link_abcd_default_9295_dms),
	.regs = link_abcd_default_9295_dms,
};

static const s64 max96724_link_freq[] = {
	400000000,
	800000000,
};

static void set_sub_stream_fmt(int index, u32 code)
{
	max96724_query_sub_stream[index] &= 0xFFFFFFFFFFFF0000;
	max96724_query_sub_stream[index] |= code;
}

static void set_sub_stream_h(int index, u32 height)
{
	s64 val = height & 0xFFFF;

	max96724_query_sub_stream[index] &= 0xFFFFFFFF0000FFFF;
	max96724_query_sub_stream[index] |= val << 16;
}

static void set_sub_stream_w(int index, u32 width)
{
	s64 val = width & 0xFFFF;

	max96724_query_sub_stream[index] &= 0xFFFF0000FFFFFFFF;
	max96724_query_sub_stream[index] |= val << 32;
}

static void set_sub_stream_dt(int index, u32 dt)
{
	s64 val = dt & 0xFF;

	max96724_query_sub_stream[index] &= 0xFF00FFFFFFFFFFFF;
	max96724_query_sub_stream[index] |= val << 48;
}

static void set_sub_stream_vc_id(int index, u32 vc_id)
{
	s64 val = vc_id & 0xFF;

	max96724_query_sub_stream[index] &= 0x00FFFFFFFFFFFFFF;
	max96724_query_sub_stream[index] |= val << 56;
}

static u8 max96724_set_sub_stream[] = {
	0, 0, 0, 0
};

struct max96724_source {
	struct v4l2_subdev *sd;

	struct max9671x_subdev_info *subdev_info;
};

/*
 * fixed mapping
 * SIOA - vc 0
 * SIOB - vc 1
 * SIOC - vc 2
 * SIOD - vc 3
 *
 * link/vc to subdev mapping is flexible
 */
struct max96724_priv {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pads[MAX96724_N_PADS];

	struct regmap *regmap16;

	struct v4l2_ctrl_handler ctrls;

	struct v4l2_mbus_framefmt fmt[MAX96724_N_SINKS];

	struct mutex mutex;

	int errb_int;
	int lock_int;

	unsigned int source_mask;
	unsigned int stream_count;
	struct max96724_source sources[MAX96724_N_SINKS];

	struct max9671x_platform_data *platform_data;

};

static int max96724_read(struct max96724_priv *priv, u32 reg, u32 *val)
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

static int max96724_write(struct max96724_priv *priv, u32 reg, u32 val)
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

static int max96724_read_rem(struct max96724_priv *priv, u16 addr, u32 reg, u32 *val)
{
	int ret;
	unsigned short addr_backup;

	addr_backup = priv->client->addr;
	priv->client->addr = addr;
	ret = regmap_read(priv->regmap16, reg, val);
	priv->client->addr = addr_backup;

	if (ret < 0) {
		dev_err(&priv->client->dev,
				"%s : addr 0x%x register 0x%02x read failed (%d)\n",
				__func__, addr, reg, ret);
	}

	return ret;
}

static int max96724_write_rem(struct max96724_priv *priv, u16 addr, u32 reg, u32 val)
{
	int ret;
	unsigned short addr_backup;

	addr_backup = priv->client->addr;
	priv->client->addr = addr;
	ret = regmap_write(priv->regmap16, reg, val);
	priv->client->addr = addr_backup;

	if (ret) {
		dev_err(&priv->client->dev,
				"%s : addr 0x%x register 0x%02x write failed (%d)\n",
				__func__, addr, reg, ret);
	}

	return ret;
}

static int max96724_write_reg_list(struct max96724_priv *priv,
		const struct max96724_reg_list *r_list)
{
	int ret, i;

	for (i = 0; i < r_list->num_of_regs; i++) {
		if (r_list->regs[i].address == 0xffff) {
			msleep(r_list->regs[i].val);
			continue;
		}

		ret = max96724_write(priv, r_list->regs[i].address,
				r_list->regs[i].val);

		if (ret) {
			dev_err(&priv->client->dev,
					"%s : register list write failed @ (%d)\n",
					__func__, i);
			return ret;
		}
	}

	return 0;
}

static int max96724_write_rem_reg_list(struct max96724_priv *priv, u16 addr,
		const struct max96724_reg_list *r_list)
{
	int ret, i;

	for (i = 0; i < r_list->num_of_regs; i++) {
		if (r_list->regs[i].address == 0xffff) {
			msleep(r_list->regs[i].val);
			continue;
		}

		ret = max96724_write_rem(priv, addr, r_list->regs[i].address,
				r_list->regs[i].val);

		if (ret) {
			dev_err(&priv->client->dev,
					"%s : register list write failed @ (%d)\n",
					__func__, i);
			return ret;
		}
	}

	return 0;
}

static int max96724_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct max96724_priv *priv = to_max96724(sd);
	if(enable) {
		max96724_write(priv, 0x0002, 0x53);
		max96724_write(priv, 0x040b, 0x42);
	} else {
		max96724_write(priv, 0x0002, 0x03);
		max96724_write(priv, 0x040b, 0x00);
	}

	return 0;
}

/*
 * new interface, enable GMSL link
 */
static int max96724_s_stream_vc(struct max96724_priv *priv, u8 vc_id, u8 state)
{
	if (!(priv->source_mask & BIT(vc_id))) {
		dev_err(&priv->client->dev, "No device on link %d\n", vc_id);
		return -EIO;
	}

	if (state) {
		if (priv->stream_count) {
			priv->stream_count++;
			return 0;
		}

		/* force mipi clocks running */
		dev_dbg(&priv->client->dev, "power on MIPI\n");
		max96724_write(priv, 0x8a0, 0x04);
		max96724_write(priv, 0x8a0, 0x84);
		priv->stream_count++;
	} else {
		priv->stream_count--;
		if (priv->stream_count)
			return 0;

		dev_dbg(&priv->client->dev, "power off MIPI\n");
		max96724_write(priv, 0x8a0, 0x04);
	}

	return 0;
}

static struct v4l2_mbus_framefmt *
max96724_get_pad_format(struct max96724_priv *priv,
		struct v4l2_subdev_state *sd_state,
		unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&priv->sd, sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &priv->fmt[pad];
	default:
		return NULL;
	}
}

static int max96724_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{
	struct max96724_priv *priv = to_max96724(sd);
	unsigned int pad = format->pad;
	struct v4l2_mbus_framefmt *cfg_fmt;

	mutex_lock(&priv->mutex);

	if (pad == MAX96724_SRC_PAD)
		pad = __ffs(priv->source_mask);

	cfg_fmt = max96724_get_pad_format(priv, sd_state, pad, format->which);
	if (!cfg_fmt) {
		dev_err(sd->dev, "Failed to find format info for pad %d\n", pad);
		mutex_unlock(&priv->mutex);
		return -EINVAL;
	}

	format->format = *cfg_fmt;

	mutex_unlock(&priv->mutex);

	return 0;
}

static int max96724_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{
	struct max96724_priv *priv = to_max96724(sd);
	unsigned int pad = format->pad;
	struct v4l2_mbus_framefmt *cfg_fmt;
	if (pad == MAX96724_SRC_PAD)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	cfg_fmt = max96724_get_pad_format(priv, sd_state, pad, format->which);
	if (!cfg_fmt) {
		dev_err(sd->dev, "Failed to find format info for pad %d\n", pad);
		mutex_unlock(&priv->mutex);
		return -EINVAL;
	}

	*cfg_fmt = format->format;

	set_sub_stream_fmt(pad, cfg_fmt->code);
	set_sub_stream_h(pad, cfg_fmt->height);
	set_sub_stream_w(pad, cfg_fmt->width);
	set_sub_stream_dt(pad, mbus_code_to_mipi(cfg_fmt->code));
	set_sub_stream_vc_id(pad, pad);

	mutex_unlock(&priv->mutex);
	return 0;
}

static void max96724_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = 1600;
	fmt->height = 1300;
	fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int max96724_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format;
	int i;

	for (i = 0; i < MAX96724_N_SINKS; i++) {
		format = v4l2_subdev_get_try_format(sd, fh->state, i);
		max96724_init_format(format);
	}
	return 0;
}

static int max96724_registered(struct v4l2_subdev *sd)
{
	int ret;
	struct max96724_priv *priv = to_max96724(sd);
	int i;
	int src_pad;
	unsigned int rx_port;

	for (i = 0; i < priv->platform_data->subdev_num && i < MAX96724_N_SINKS; i++) {
		struct max9671x_subdev_info *info = &priv->platform_data->subdev_info[i];

		rx_port = info->rx_port;

		priv->sources[rx_port].sd = v4l2_i2c_new_subdev_board(
				priv->sd.v4l2_dev, priv->client->adapter,
				&info->board_info, 0);

		if (!priv->sources[rx_port].sd) {
			dev_err(sd->dev, "Failed to init remote dev %d\n", i);
			continue;
		}

		src_pad = media_get_pad_index(&priv->sources[rx_port].sd->entity,
				false, PAD_SIGNAL_DEFAULT);
		if (src_pad < 0) {
			dev_err(sd->dev, "Failed to find source pad on %s\n",
					priv->sources[rx_port].sd->name);
			return ret;
		}

		ret = media_create_pad_link(&priv->sources[rx_port].sd->entity, src_pad,
				&priv->sd.entity, info->rx_port, MEDIA_LNK_FL_DYNAMIC);
		if (ret) {
			dev_err(sd->dev, "Failed to creaet link %s:%d -> %s:%d\n",
					priv->sources[i].sd->name, src_pad,
					priv->sd.name, rx_port);
			return ret;
		}
	}
	return 0;
}

static const struct v4l2_subdev_video_ops max96724_video_ops = {
	.s_stream = max96724_s_stream,
};

static const struct v4l2_subdev_pad_ops max96724_pad_ops = {
	.get_fmt = max96724_get_fmt,
	.set_fmt = max96724_set_fmt,
};

static const struct v4l2_subdev_ops max96724_subdev_ops = {
	.video = &max96724_video_ops,
	.pad = &max96724_pad_ops,
};

static const struct v4l2_subdev_internal_ops max96724_internal_ops = {
	.open = max96724_open,
	.registered = max96724_registered,
};

static const struct media_entity_operations max96724_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int max96724_get_locked_status(struct max96724_priv *priv, int link)
{
	u16 reg;
	int ret;
	int val;

	switch (link) {
	case MAX_PORT_SIOA:
		reg = 0x1A;
		break;
	case MAX_PORT_SIOB:
		reg = 0x0A;
		break;
	case MAX_PORT_SIOC:
		reg = 0x0B;
		break;
	case MAX_PORT_SIOD:
		reg = 0x0C;
		break;
	default:
		dev_err(&priv->client->dev, "invalid link %d\n", link);
		return 0;
	}

	ret = max96724_read(priv, reg, &val);
	if (ret) {
		dev_err(&priv->client->dev, "failed to get link status %d\n", link);
		return 0;
	}

	return ((val & 0x08) >> 3);
}

static int max96724_remote_init(struct max96724_priv *priv, int rx_port,
		const struct max96724_reg_list *init_setting)
{
	u32 val;
	int ret;
	unsigned short tmp_addr;
	struct max9671x_subdev_info *info = &priv->platform_data->subdev_info[0];

	ret = max96724_get_locked_status(priv, rx_port);

	if (!ret) {
		dev_info(&priv->client->dev, "link %d not locked\n", rx_port);
		return -EIO;
	}

	max96724_write(priv, 0x03, ~(1 << rx_port * 2));

	/* get current addr in use */
	if (max96724_read_rem(priv, info->phy_i2c_addr, 0x10, &val))
		tmp_addr = info->alias_addr;
	else
		tmp_addr = info->phy_i2c_addr;
	/* reset */
//	max96724_read_rem(priv, tmp_addr, 0x10, &val);
//	max96724_write_rem(priv, tmp_addr, 0x10, val | 0x20);
//	msleep(DELAY_MS);

	/* assign new addr */
//	max96724_write_rem(priv, info->phy_i2c_addr, 0x00,
	max96724_write_rem(priv, tmp_addr, 0x00,
			info->alias_addr << 1);

	/* initialize remote */
	ret = max96724_write_rem_reg_list(priv, info->alias_addr, init_setting);

	max96724_write(priv, 0x03, 0xff);

	return ret;
}

/* V4L2 control IDs */
#define V4L2_CID_LINKA_STATUS (V4L2_CID_IPU_BASE + 6)
#define V4L2_CID_RESET_LINKA (V4L2_CID_IPU_BASE + 7)
#define V4L2_CID_LINKB_STATUS (V4L2_CID_IPU_BASE + 8)
#define V4L2_CID_RESET_LINKB (V4L2_CID_IPU_BASE + 9)
#define V4L2_CID_LINKC_STATUS (V4L2_CID_IPU_BASE + 10)
#define V4L2_CID_RESET_LINKC (V4L2_CID_IPU_BASE + 11)
#define V4L2_CID_LINKD_STATUS (V4L2_CID_IPU_BASE + 12)
#define V4L2_CID_RESET_LINKD (V4L2_CID_IPU_BASE + 13)

static const char * const max96724_link_status[] = {
	"not locked",
	"locked",
};

static int max96724_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max96724_priv *priv = container_of(ctrl->handler,
			struct max96724_priv, ctrls);
	struct i2c_client *client = v4l2_get_subdevdata(&priv->sd);
	u32 val;
	u8 vc_id;
	u8 state;
	int ret = 0;

	switch (ctrl->id) {
	case  V4L2_CID_IPU_SET_SUB_STREAM:
		val = (*ctrl->p_new.p_s64 & 0xffff);
		vc_id = (val >> 8) & 0xff;
		state = val & 0xff;

		max96724_set_sub_stream[vc_id] = state;
		ret = max96724_s_stream_vc(priv, vc_id, state);
		break;
	case V4L2_CID_RESET_LINKA:
		ret = max96724_remote_init(priv, MAX_PORT_SIOA, &link_setting);
		break;
	case V4L2_CID_RESET_LINKB:
		ret = max96724_remote_init(priv, MAX_PORT_SIOB, &link_setting);
		break;
	case V4L2_CID_RESET_LINKC:
		ret = max96724_remote_init(priv, MAX_PORT_SIOC, &link_setting);
		break;
	case V4L2_CID_RESET_LINKD:
		ret = max96724_remote_init(priv, MAX_PORT_SIOD, &link_setting);
		break;
	default:
		dev_info(&client->dev, "%s : v4l2 control id 0x%x\n", __func__, ctrl->id);
	}

	return ret;
}

static int max96724_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max96724_priv *priv = container_of(ctrl->handler,
			struct max96724_priv, ctrls);
	struct i2c_client *client = priv->client;

	switch (ctrl->id) {
	case V4L2_CID_LINKA_STATUS:
		ctrl->val = max96724_get_locked_status(priv, MAX_PORT_SIOA);
		break;
	case V4L2_CID_LINKB_STATUS:
		ctrl->val = max96724_get_locked_status(priv, MAX_PORT_SIOB);
		break;
	case V4L2_CID_LINKC_STATUS:
		ctrl->val = max96724_get_locked_status(priv, MAX_PORT_SIOC);
		break;
	case V4L2_CID_LINKD_STATUS:
		ctrl->val = max96724_get_locked_status(priv, MAX_PORT_SIOD);
		break;
	default:
		dev_info(&client->dev, "%s : v4l2 control id 0x%x\n", __func__, ctrl->id);
	}

	return 0;
}

static const struct v4l2_ctrl_ops max96724_ctrl_ops = {
	.g_volatile_ctrl = max96724_g_volatile_ctrl,
	.s_ctrl = max96724_s_ctrl,
};

static const struct v4l2_ctrl_config max96724_controls[] = {
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_LINK_FREQ,
		.name = "V4L2_CID_LINK_FREQ",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(max96724_link_freq) - 1,
		.def = 0,
		.menu_skip_mask = 0,
		.qmenu_int = max96724_link_freq,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_IPU_QUERY_SUB_STREAM,
		.name = "query virtual channel",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.max = ARRAY_SIZE(max96724_query_sub_stream) - 1,
		.min = 0,
		.def = 0,
		.menu_skip_mask = 0,
		.qmenu_int = max96724_query_sub_stream,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_IPU_SET_SUB_STREAM,
		.name = "set virtual channel",
		.type = V4L2_CTRL_TYPE_INTEGER64,
		.max = 0xffff,
		.min = 0,
		.def = 0,
		.step = 1,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_LINKA_STATUS,
		.name = "query SIOA link status",
		.type = V4L2_CTRL_TYPE_MENU,
		.max = ARRAY_SIZE(max96724_link_status) - 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
		.qmenu = max96724_link_status,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_RESET_LINKA,
		.name = "reset SIOA",
		.type = V4L2_CTRL_TYPE_BUTTON,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_LINKB_STATUS,
		.name = "query SIOB link status",
		.type = V4L2_CTRL_TYPE_MENU,
		.max = ARRAY_SIZE(max96724_link_status) - 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
		.qmenu = max96724_link_status,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_RESET_LINKB,
		.name = "reset SIOB",
		.type = V4L2_CTRL_TYPE_BUTTON,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_LINKC_STATUS,
		.name = "query SIOC link status",
		.type = V4L2_CTRL_TYPE_MENU,
		.max = ARRAY_SIZE(max96724_link_status) - 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
		.qmenu = max96724_link_status,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_RESET_LINKC,
		.name = "reset SIOC",
		.type = V4L2_CTRL_TYPE_BUTTON,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_LINKD_STATUS,
		.name = "query SIOD link status",
		.type = V4L2_CTRL_TYPE_MENU,
		.max = ARRAY_SIZE(max96724_link_status) - 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
		.qmenu = max96724_link_status,
	},
	{
		.ops = &max96724_ctrl_ops,
		.id = V4L2_CID_RESET_LINKD,
		.name = "reset SIOD",
		.type = V4L2_CTRL_TYPE_BUTTON,
	},
};

static int max96724_register_subdev(struct max96724_priv *priv)
{
	int ret;
	int i;

	v4l2_i2c_subdev_init(&priv->sd, priv->client, &max96724_subdev_ops);
	snprintf(priv->sd.name, sizeof(priv->sd.name), "max96724");
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.internal_ops = &max96724_internal_ops;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->sd.entity.ops = &max96724_subdev_entity_ops;

	v4l2_ctrl_handler_init(&priv->ctrls, 1);
	priv->sd.ctrl_handler = &priv->ctrls;

	for (i = 0; i < ARRAY_SIZE(max96724_controls); i++) {
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_new_custom(&priv->ctrls, &max96724_controls[i], NULL);
		if (priv->ctrls.error) {
			dev_err(&priv->client->dev, "Failed to create ctrl %s %d\n",
					max96724_controls[i].name, priv->ctrls.error);
			ret = priv->ctrls.error;
			goto failed_out;
		}
	}

	for (i = 0; i < MAX96724_N_SINKS; i++)
		priv->pads[i].flags = MEDIA_PAD_FL_SINK;
	priv->pads[MAX96724_SRC_PAD].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&priv->sd.entity, MAX96724_N_PADS, priv->pads);
	if (ret) {
		dev_err(&priv->client->dev,
				"%s : Failed to init media entity\n", __func__);
		goto failed_out;
	}

	return 0;

failed_out:
	media_entity_cleanup(&priv->sd.entity);
	v4l2_ctrl_handler_free(&priv->ctrls);

	return ret;
}

static int max96724_init(struct max96724_priv *priv)
{
	int ret;
	unsigned int val;
	unsigned short tmp_addr;
	struct max9671x_platform_data *pdata = priv->platform_data;
	struct max9671x_subdev_info *info = &pdata->subdev_info[0];

	/* FIXME: need external method to resolve conflict address */
	max96724_write(priv, 0x03, 0xff);
	max96724_write_rem(priv, 0x28, 0x01, 0x11);
	max96724_write_rem(priv, 0x28, 0x03, 0x57);
	max96724_write_rem(priv, 0x2a, 0x01, 0x11);
	max96724_write_rem(priv, 0x2a, 0x03, 0x57);

	/* chip identify */
	ret = max96724_read(priv, 0x0D, &val);

	if (ret) {
		dev_err(&priv->client->dev, "Failed to read reg %x %d\n",
				0xD, ret);
		return ret;
	}
	dev_info(&priv->client->dev, "des dev id is 0x%x, slave addr is 0x%d\n",val, priv->client->addr);

	if (val != 0xA0) {
		dev_err(&priv->client->dev, "Failed to detect max96724 %x\n", val);
		return -ENXIO;
	}

	/* internal regulator */
	ret = max96724_read(priv, 0x17, &val);
	max96724_write(priv, 0x17, val | 0x04);
	ret = max96724_read(priv, 0x19, &val);
	max96724_write(priv, 0x19, val | 0x10);

	/* RESET_ONESHOT A/B/C/D */
	ret = max96724_write(priv, 0x18, 0x0F);
	msleep(DELAY_MS);

	/* control channel */
	ret = max96724_read(priv, 0x01, &val);
	dev_info(&priv->client->dev, "CC settings %x\n", val);

	/* GMSL2 */
	ret = max96724_read(priv, 0x06, &val);
	dev_info(&priv->client->dev, "Link settings %x\n", val);

	/* Coax */
	ret = max96724_read(priv, 0x22, &val);
	dev_info(&priv->client->dev, "Cable settings %x\n", val);

	/* Link lock */
	priv->source_mask = MAX96724_LINK_STATUS;
	dev_info(&priv->client->dev, "Link status %x\n", priv->source_mask);

	if (!priv->source_mask) {
		dev_err(&priv->client->dev, "No remote devices connected\n");
		return -ENXIO;
	}

	/* enable rem cc */
	max96724_write(priv, 0x03, 0xaa);

	/* get current addr in use */
	if (max96724_read_rem(priv, info->phy_i2c_addr, 0x10, &val))
		tmp_addr = info->alias_addr;
	else
		tmp_addr = info->phy_i2c_addr;

	max96724_read_rem(priv, tmp_addr, 0xd, &val);
	dev_info(&priv->client->dev, "ser dev id is 0x%x, slave addr is 0x%x\n",val, tmp_addr);

	/* broadcast to all connected remote */
	ret = max96724_write_rem(priv, tmp_addr,
			0x00, info->alias_addr << 1);
	if (ret)
		return ret;

	ret = max96724_write_rem_reg_list(priv, info->alias_addr,
			&link_setting);
	if (ret)
		return ret;

	/* disnable all CC and reset */
	max96724_write(priv, 0x03, 0xff);
	max96724_write(priv, 0x18, 0x0F);
	msleep(DELAY_MS);

	/* Link lock */
	priv->source_mask = MAX96724_LINK_STATUS;
	dev_info(&priv->client->dev, "Link status %x\n", priv->source_mask);

	if (!priv->source_mask) {
		dev_err(&priv->client->dev, "No remote devices connected\n");
		return -ENXIO;
	}

#if 0
	/* FSYNC */
	ret = max96724_write_reg_list(priv, &fsync_setting);

	/* CFGH {A/B/C/D} VIDEO {X/Y/Z/U} */
	/* VIDEO PIPE SEL */
	ret = max96724_write_reg_list(priv, &video_pipe_setting);

	/* MIPI TX */
	ret = max96724_write_reg_list(priv, &mipi_ctrl_setting);

	/* MIPI PHY */
	ret = max96724_write_reg_list(priv, &mipi_phy_setting);
#endif
	ret = max96724_write_reg_list(priv, &desay_init_dms_setting);
	return 0;
}

static irqreturn_t max96724_threaded_irq_fn(int irq, void *devid)
{
	struct max96724_priv *priv = devid;

	dev_dbg(&priv->client->dev, "IRQ triggered  %x\n", irq);

	return IRQ_HANDLED;
}

static int max96724_probe(struct i2c_client *client)
{
	struct max96724_priv *priv;
	int ret;
	printk("max96724_probe================+\n");

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;

	priv->regmap16 = devm_regmap_init_i2c(client, &config16);
	if (IS_ERR(priv->regmap16)) {
		dev_err(&client->dev, "%s : Failed to init regmap\n", __func__);
		return -EIO;
	}

	priv->platform_data = client->dev.platform_data;

	if (priv->platform_data->errb_gpio != -1) {
		ret = devm_gpio_request_one(&client->dev,
				priv->platform_data->errb_gpio,
				0, "ERRB PIN");
		if (ret) {
			dev_err(&client->dev, "request errb gpio failed %d\n", ret);
			return ret;
		}

		ret = gpio_direction_input(priv->platform_data->errb_gpio);
		if (ret) {
			dev_err(&client->dev, "Failed to set ERRB as input %d\n", ret);
			return ret;
		}

		priv->errb_int = gpio_to_irq(priv->platform_data->errb_gpio);

		ret = devm_request_threaded_irq(&client->dev, priv->errb_int,
				NULL, max96724_threaded_irq_fn,
				priv->platform_data->errb_gpio_flags,
				priv->platform_data->errb_gpio_name,
				priv);
		if (ret) {
			dev_err(&client->dev, "Failed to request ERRB IRQ %d\n", ret);
			return ret;
		}
	}

	if (priv->platform_data->lock_gpio != -1) {
		ret = devm_gpio_request_one(&client->dev,
				priv->platform_data->lock_gpio,
				0, "LOCK PIN");
		if (ret) {
			dev_err(&client->dev, "request lock gpio failed %d\n", ret);
			return ret;
		}

		ret = gpio_direction_input(priv->platform_data->lock_gpio);
		if (ret) {
			dev_err(&client->dev, "Failed to set LOCK as input %d\n", ret);
			return ret;
		}

		priv->lock_int = gpio_to_irq(priv->platform_data->lock_gpio);

		ret = devm_request_threaded_irq(&client->dev, priv->lock_int,
				NULL, max96724_threaded_irq_fn,
				priv->platform_data->errb_gpio_flags,
				priv->platform_data->errb_gpio_name,
				priv);
		if (ret) {
			dev_err(&client->dev, "Failed to request LOCK IRQ %d\n", ret);
			return ret;
		}
	}

	dev_info(&client->dev, "errb irq %x, lock irq %x\n",
			priv->errb_int, priv->lock_int);

	if (priv->platform_data->pwdnb_gpio != -1) {
		ret = devm_gpio_request_one(&client->dev,
				priv->platform_data->pwdnb_gpio,
				GPIOF_OUT_INIT_HIGH, "PWDNB PIN");
		if (ret) {
			dev_err(&client->dev, "request pwdnb pin failed %d\n", ret);
			return ret;
		}
		gpio_set_value(priv->platform_data->pwdnb_gpio, 1);
		msleep(DELAY_MS);
	}

	mutex_init(&priv->mutex);

	ret = max96724_init(priv);
	if (ret) {
		dev_err(&client->dev, "failed to init max96724 %d\n", ret);
		goto probe_err;
	}

	ret = max96724_register_subdev(priv);
	if (ret) {
		dev_err(&priv->client->dev,
				"%s : failed to register subdev\n", __func__);
		goto probe_err;
	}
	printk("max96724_probe================-\n");

	return 0;

probe_err:

	mutex_destroy(&priv->mutex);

	return ret;
}

static void max96724_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max96724_priv *priv = to_max96724(sd);

	mutex_destroy(&priv->mutex);

	v4l2_ctrl_handler_free(&priv->ctrls);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_device_unregister_subdev(sd);

	return;
}

/* no power or clk control */
static int max96724_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct max96724_priv *priv = to_max96724(subdev);
	int ret;
	int i;

	for (i = 0; i < MAX96724_N_SINKS; i++)
		if (max96724_set_sub_stream[i]) {
			ret = max96724_s_stream_vc(priv, i, 0);
			if (ret)
				dev_err(&client->dev, "failed to stop link %d ret %d\n", i, ret);
		}

	return 0;
}

/* re-initialize the link and resume streaming if needed */
static int max96724_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct max96724_priv *priv = to_max96724(subdev);
	int ret;
	int i;

	ret = max96724_init(priv);
	if (ret) {
		dev_err(&client->dev, "%s : resume fail %d\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < MAX96724_N_SINKS; i++)
		if (max96724_set_sub_stream[i]) {
			ret = max96724_s_stream_vc(priv, i, 1);
			if (ret)
				dev_err(&client->dev, "failed to start	link %d ret %d\n", i, ret);
		}

	return 0;
}

static const struct i2c_device_id max96724_id_table[] = {
	{"max96724", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, max96724_id_table);

static const struct dev_pm_ops max96724_pm_ops = {
	.suspend = max96724_suspend,
	.resume = max96724_resume,
};

static struct i2c_driver max96724_i2c_driver = {
	.driver = {
		.name = "max96724",
		.pm = &max96724_pm_ops,
	},
	.probe_new = max96724_probe,
	.remove = max96724_remove,
	.id_table = max96724_id_table,
};

module_i2c_driver(max96724_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX96724 GMSL Deserializer Driver");
MODULE_AUTHOR("Intel");
MODULE_LICENSE("GPL");
