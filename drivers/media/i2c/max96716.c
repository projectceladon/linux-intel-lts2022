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


#define to_max96716(_sd) container_of(_sd, struct max96716_priv, sd)

#define MAX96716_LINK_STATUS \
	(max96716_get_locked_status(priv, MAX_PORT_SIOA) << MAX_PORT_SIOA \
	| max96716_get_locked_status(priv, MAX_PORT_SIOB) << MAX_PORT_SIOB)


#define MAX96716_N_SINKS 2
#define MAX96716_N_PADS 3
#define MAX96716_SRC_PAD 2

#define DELAY_MS 100

struct max96716_reg {
	u16 address;
	u8 val;
};

struct max96716_reg_list {
	u32 num_of_regs;
	const struct max96716_reg *regs;
};

static struct regmap_config config16 = {
	.reg_bits = 16,
	.val_bits = 8,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
};

/*
 * 1920x1080 UYVY
 */
static s64 max96716_query_sub_stream[] = {
	0x001e07800438200f, 0x011e07800438200f,
	0, 0,
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

/*
 * disable CSI out
 * 2x4
 * 800MBps
 * 2lanes
 * enable PHY 0/1/2/3
 */
static const struct max96716_reg csi_phy[] = {
	{0x0313, 0x00},
	{0x0330, 0x04},
	{0x044a, 0x50},
	{0x0320, 0x28},
};
static const struct max96716_reg_list mipi_phy_setting = {
	.num_of_regs = ARRAY_SIZE(csi_phy),
	.regs = csi_phy,
};

/*
 * link a pipe z -> pipe y
 * link b pipe z -> pipe z
 * enable pipe y/z
 */
static const struct max96716_reg video_pipe_sel[] = {
	{0x0161, 0x32},
	{0x0160, 0x03},
};
static const struct max96716_reg_list video_pipe_setting = {
	.num_of_regs = ARRAY_SIZE(video_pipe_sel),
	.regs = video_pipe_sel,
};

/*
 * pipe y vc0
 * FS/DATA/FE identity mapping
 * to csi ctrl 1
 * pipe z vc1
 * FS/DATA/FE identity mapping
 * to csi ctrl 1
 */
static const struct max96716_reg video_pipe_to_csi_ctrl_mapping[] = {
	/* pipe y */
	{0x044b, 0x07},
	{0x044d, 0x00},
	{0x044e, 0x00},
	{0x044f, 0x1e},
	{0x0450, 0x1e},
	{0x0451, 0x01},
	{0x0452, 0x01},
	{0x046d, 0x15},
	/* pipe z */
	{0x048b, 0x07},
	{0x048d, 0x00},
	{0x048e, 0x40},
	{0x048f, 0x1e},
	{0x0490, 0x5e},
	{0x0491, 0x01},
	{0x0492, 0x41},
	{0x04ad, 0x15},
};

static const struct max96716_reg_list mipi_ctrl_setting = {
	.num_of_regs = ARRAY_SIZE(video_pipe_to_csi_ctrl_mapping),
	.regs = video_pipe_to_csi_ctrl_mapping,
};

static const struct max96716_reg rlms[] ={
	/* RLMS A */
	{0x143f, 0x3d},
	{0x143e, 0xfd},
	{0x14ad, 0x68},
	{0x14ac, 0xa8},
	{0x148c, 0x10},
	{0x1498, 0xc0},
	{0x1446, 0x01},
	{0x1445, 0x81},
	{0x140b, 0x44},
	{0x140a, 0x08},
	{0x1431, 0x18},
	{0x1421, 0x08},
	{0x14a5, 0x70},
	/* RLMS B */
	{0x153f, 0x3d},
	{0x153e, 0xfd},
	{0x15ad, 0x68},
	{0x15ac, 0xa8},
	{0x158c, 0x10},
	{0x1598, 0xc0},
	{0x1546, 0x01},
	{0x1545, 0x81},
	{0x150b, 0x44},
	{0x150a, 0x08},
	{0x1531, 0x18},
	{0x1521, 0x08},
	{0x15a5, 0x70},
};

static const struct max96716_reg_list rlms_conf = {
	.num_of_regs = ARRAY_SIZE(rlms),
	.regs = rlms,
};

/*
 * Route YUV422 8bit to pipe
 * CMU
 * RLMS
 */
static const struct max96716_reg link_ab_default[] = {
	/* disable local CC */
	{0x0001, 0xe4},
	{0x0318, 0x5E},
	{0x0302, 0x10},
	{0x1417, 0x00},
	{0x1432, 0x7F},
	{0x0010, 0x31},
	{0xffff, 0x64},
};

static const struct max96716_reg_list link_setting = {
	.num_of_regs = ARRAY_SIZE(link_ab_default),
	.regs = link_ab_default,
};

static const s64 max96716_link_freq[] = {
	400000000,
	800000000,
};

static void set_sub_stream_fmt(int index, u32 code)
{
	max96716_query_sub_stream[index] &= 0xFFFFFFFFFFFF0000;
	max96716_query_sub_stream[index] |= code;
}

static void set_sub_stream_h(int index, u32 height)
{
	s64 val = height & 0xFFFF;

	max96716_query_sub_stream[index] &= 0xFFFFFFFF0000FFFF;
	max96716_query_sub_stream[index] |= val << 16;
}

static void set_sub_stream_w(int index, u32 width)
{
	s64 val = width & 0xFFFF;

	max96716_query_sub_stream[index] &= 0xFFFF0000FFFFFFFF;
	max96716_query_sub_stream[index] |= val << 32;
}

static void set_sub_stream_dt(int index, u32 dt)
{
	s64 val = dt & 0xFF;

	max96716_query_sub_stream[index] &= 0xFF00FFFFFFFFFFFF;
	max96716_query_sub_stream[index] |= val << 48;
}

static void set_sub_stream_vc_id(int index, u32 vc_id)
{
	s64 val = vc_id & 0xFF;

	max96716_query_sub_stream[index] &= 0x00FFFFFFFFFFFFFF;
	max96716_query_sub_stream[index] |= val << 56;
}

static u8 max96716_set_sub_stream[] = {
	0, 0, 0, 0
};

struct max96716_source {
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
struct max96716_priv {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pads[MAX96716_N_PADS];

	struct regmap *regmap16;

	struct v4l2_ctrl_handler ctrls;

	struct v4l2_mbus_framefmt fmt[MAX96716_N_SINKS];

	struct mutex mutex;

	int errb_int;
	int lock_int;

	unsigned int source_mask;
	unsigned int stream_count;
	struct max96716_source sources[MAX96716_N_SINKS];

	struct max9671x_platform_data *platform_data;

};

static int max96716_read(struct max96716_priv *priv, u32 reg, u32 *val)
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

static int max96716_write(struct max96716_priv *priv, u32 reg, u32 val)
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

static int max96716_read_rem(struct max96716_priv *priv, u16 addr, u32 reg, u32 *val)
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

static int max96716_write_rem(struct max96716_priv *priv, u16 addr, u32 reg, u32 val)
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

static int max96716_write_reg_list(struct max96716_priv *priv,
		const struct max96716_reg_list *r_list)
{
	int ret, i;

	for (i = 0; i < r_list->num_of_regs; i++) {
		if (r_list->regs[i].address == 0xffff) {
			msleep(r_list->regs[i].val);
			continue;
		}

		ret = max96716_write(priv, r_list->regs[i].address,
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

static int max96716_write_rem_reg_list(struct max96716_priv *priv, u16 addr,
		const struct max96716_reg_list *r_list)
{
	int ret, i;

	for (i = 0; i < r_list->num_of_regs; i++) {
		if (r_list->regs[i].address == 0xffff) {
			msleep(r_list->regs[i].val);
			continue;
		}

		ret = max96716_write_rem(priv, addr, r_list->regs[i].address,
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

static int max96716_s_stream(struct v4l2_subdev *sd, int enable)
{
	return 0;
}

/*
 * new interface, enable GMSL link
 */
static int max96716_s_stream_vc(struct max96716_priv *priv, u8 vc_id, u8 state)
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
		max96716_write(priv, 0x330, 0x04);
		max96716_write(priv, 0x330, 0x84);
		priv->stream_count++;
	} else {
		priv->stream_count--;
		if (priv->stream_count)
			return 0;

		dev_dbg(&priv->client->dev, "power off MIPI\n");
		max96716_write(priv, 0x330, 0x04);
	}

	return 0;
}

static struct v4l2_mbus_framefmt *
max96716_get_pad_format(struct max96716_priv *priv,
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

static int max96716_get_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{
	struct max96716_priv *priv = to_max96716(sd);
	unsigned int pad = format->pad;
	struct v4l2_mbus_framefmt *cfg_fmt;

	mutex_lock(&priv->mutex);

	if (pad == MAX96716_SRC_PAD)
		pad = __ffs(priv->source_mask);

	cfg_fmt = max96716_get_pad_format(priv, sd_state, pad, format->which);
	if (!cfg_fmt) {
		dev_err(sd->dev, "Failed to find format info for pad %d\n", pad);
		mutex_unlock(&priv->mutex);
		return -EINVAL;
	}

	format->format = *cfg_fmt;

	mutex_unlock(&priv->mutex);

	return 0;
}

static int max96716_set_fmt(struct v4l2_subdev *sd,
		struct v4l2_subdev_state *sd_state,
		struct v4l2_subdev_format *format)
{
	struct max96716_priv *priv = to_max96716(sd);
	unsigned int pad = format->pad;
	struct v4l2_mbus_framefmt *cfg_fmt;

	if (pad == MAX96716_SRC_PAD)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	cfg_fmt = max96716_get_pad_format(priv, sd_state, pad, format->which);
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

static void max96716_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = 1920;
	fmt->height = 1080;
	fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static int max96716_registered(struct v4l2_subdev *sd)
{
	int ret;
	struct max96716_priv *priv = to_max96716(sd);
	int i;
	int src_pad;
	unsigned int rx_port;

	for (i = 0; i < priv->platform_data->subdev_num && i < MAX96716_N_SINKS; i++) {
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

static int max96716_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_mbus_framefmt *format;

	format = v4l2_subdev_get_try_format(sd, fh->state, MAX96716_SRC_PAD);
	max96716_init_format(format);

	return 0;
}

static const struct v4l2_subdev_video_ops max96716_video_ops = {
	.s_stream = max96716_s_stream,
};

static const struct v4l2_subdev_pad_ops max96716_pad_ops = {
	.get_fmt = max96716_get_fmt,
	.set_fmt = max96716_set_fmt,
};

static const struct v4l2_subdev_ops max96716_subdev_ops = {
	.video = &max96716_video_ops,
	.pad = &max96716_pad_ops,
};

static const struct v4l2_subdev_internal_ops max96716_internal_ops = {
	.open = max96716_open,
	.registered = max96716_registered,
};

static const struct media_entity_operations max96716_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int max96716_get_locked_status(struct max96716_priv *priv, int link)
{
	u16 reg;
	int ret;
	int val;

	switch (link) {
	case MAX_PORT_SIOA:
		reg = 0x13;
		break;
	case MAX_PORT_SIOB:
		reg = 0x5009;
		break;
	default:
		dev_err(&priv->client->dev, "invalid link %d\n", link);
		return 0;
	}

	ret = max96716_read(priv, reg, &val);
	if (ret) {
		dev_err(&priv->client->dev, "failed to get link status %d\n", link);
		return 0;
	}

	return ((val & 0x08) >> 3);
}

static int max96716_remote_init(struct max96716_priv *priv, int rx_port,
		const struct max96716_reg_list *init_setting)
{
	u32 val;
	int ret;
	unsigned short tmp_addr;
	struct max9671x_subdev_info *info = &priv->platform_data->subdev_info[0];

	ret = max96716_get_locked_status(priv, rx_port);

	if (!ret) {
		dev_info(&priv->client->dev, "link %d not locked\n", rx_port);
		return -EIO;
	}

	if (rx_port) {
		max96716_read(priv, 0x01, &val);
		max96716_write(priv, 0x01,  val & ~(1 << 4));
	} else {
		max96716_read(priv, 0x03, &val);
		max96716_write(priv, 0x03,  val & ~(1 << 2));
	}

	/* get current addr in use */
	if (max96716_read_rem(priv, info->phy_i2c_addr, 0x10, &val))
		tmp_addr = info->alias_addr;
	else
		tmp_addr = info->phy_i2c_addr;
	/* reset */
//	max96712_read_rem(priv, tmp_addr, 0x10, &val);
//	max96712_write_rem(priv, tmp_addr, 0x10, val | 0x20);
//	msleep(DELAY_MS);

	/* assign new addr */
//	max96712_write_rem(priv, info->phy_i2c_addr, 0x00,
	max96716_write_rem(priv, tmp_addr, 0x00,
			info->alias_addr << 1);

	/* initialize remote */
	ret = max96716_write_rem_reg_list(priv, info->alias_addr, init_setting);

	if (rx_port) {
		max96716_read(priv, 0x01, &val);
		max96716_write(priv, 0x01,  val | (1 << 4));
	} else {
		max96716_read(priv, 0x03, &val);
		max96716_write(priv, 0x03,  val | (1 << 2));
	}

	return ret;
}

/* V4L2 control IDs */
#define V4L2_CID_LINKA_STATUS (V4L2_CID_IPU_BASE + 6)
#define V4L2_CID_RESET_LINKA (V4L2_CID_IPU_BASE + 7)
#define V4L2_CID_LINKB_STATUS (V4L2_CID_IPU_BASE + 8)
#define V4L2_CID_RESET_LINKB (V4L2_CID_IPU_BASE + 9)

static const char * const max96716_link_status[] = {
	"not locked",
	"locked",
};

static int max96716_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max96716_priv *priv = container_of(ctrl->handler,
			struct max96716_priv, ctrls);
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

		max96716_set_sub_stream[vc_id] = state;

		ret = max96716_s_stream_vc(priv, vc_id, state);
		break;
	case V4L2_CID_RESET_LINKA:
		ret = max96716_remote_init(priv, MAX_PORT_SIOA, &link_setting);
		break;
	case V4L2_CID_RESET_LINKB:
		ret = max96716_remote_init(priv, MAX_PORT_SIOB, &link_setting);
		break;
	default:
		dev_info(&client->dev, "%s : v4l2 control id 0x%x\n", __func__, ctrl->id);
	}

	return ret;
}

static int max96716_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct max96716_priv *priv = container_of(ctrl->handler,
			struct max96716_priv, ctrls);
	struct i2c_client *client = priv->client;

	switch (ctrl->id) {
	case V4L2_CID_LINKA_STATUS:
		ctrl->val = max96716_get_locked_status(priv, MAX_PORT_SIOA);
		break;
	case V4L2_CID_LINKB_STATUS:
		ctrl->val = max96716_get_locked_status(priv, MAX_PORT_SIOB);
		break;
	default:
		dev_info(&client->dev, "%s : v4l2 control id 0x%x\n", __func__, ctrl->id);
	}

	return 0;
}

static const struct v4l2_ctrl_ops max96716_ctrl_ops = {
	.g_volatile_ctrl = max96716_g_volatile_ctrl,
	.s_ctrl = max96716_s_ctrl,
};

static const struct v4l2_ctrl_config max96716_controls[] = {
	{
		.ops = &max96716_ctrl_ops,
		.id = V4L2_CID_LINK_FREQ,
		.name = "V4L2_CID_LINK_FREQ",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.min = 0,
		.max = ARRAY_SIZE(max96716_link_freq) - 1,
		.def = 0,
		.menu_skip_mask = 0,
		.qmenu_int = max96716_link_freq,
	},
	{
		.ops = &max96716_ctrl_ops,
		.id = V4L2_CID_IPU_QUERY_SUB_STREAM,
		.name = "query virtual channel",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.max = ARRAY_SIZE(max96716_query_sub_stream) - 1,
		.min = 0,
		.def = 0,
		.menu_skip_mask = 0,
		.qmenu_int = max96716_query_sub_stream,
	},
	{
		.ops = &max96716_ctrl_ops,
		.id = V4L2_CID_IPU_SET_SUB_STREAM,
		.name = "set virtual channel",
		.type = V4L2_CTRL_TYPE_INTEGER64,
		.max = 0xffff,
		.min = 0,
		.def = 0,
		.step = 1,
	},
	{
		.ops = &max96716_ctrl_ops,
		.id = V4L2_CID_LINKA_STATUS,
		.name = "query SIOA link status",
		.type = V4L2_CTRL_TYPE_MENU,
		.max = ARRAY_SIZE(max96716_link_status) - 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
		.qmenu = max96716_link_status,
	},
	{
		.ops = &max96716_ctrl_ops,
		.id = V4L2_CID_RESET_LINKA,
		.name = "reset SIOA",
		.type = V4L2_CTRL_TYPE_BUTTON,
	},
	{
		.ops = &max96716_ctrl_ops,
		.id = V4L2_CID_LINKB_STATUS,
		.name = "query SIOB link status",
		.type = V4L2_CTRL_TYPE_MENU,
		.max = ARRAY_SIZE(max96716_link_status) - 1,
		.def = 0,
		.flags = V4L2_CTRL_FLAG_VOLATILE | V4L2_CTRL_FLAG_READ_ONLY,
		.qmenu = max96716_link_status,
	},
	{
		.ops = &max96716_ctrl_ops,
		.id = V4L2_CID_RESET_LINKB,
		.name = "reset SIOB",
		.type = V4L2_CTRL_TYPE_BUTTON,
	},
};

static int max96716_register_subdev(struct max96716_priv *priv)
{
	int ret;
	int i;

	v4l2_i2c_subdev_init(&priv->sd, priv->client, &max96716_subdev_ops);
	snprintf(priv->sd.name, sizeof(priv->sd.name), "max96716 %c",
			priv->platform_data->suffix);
	priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	priv->sd.internal_ops = &max96716_internal_ops;
	priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	priv->sd.entity.ops = &max96716_subdev_entity_ops;

	v4l2_ctrl_handler_init(&priv->ctrls, 1);
	priv->sd.ctrl_handler = &priv->ctrls;

	for (i = 0; i < ARRAY_SIZE(max96716_controls); i++) {
		struct v4l2_ctrl *ctrl;

		ctrl = v4l2_ctrl_new_custom(&priv->ctrls, &max96716_controls[i], NULL);
		if (priv->ctrls.error) {
			dev_err(&priv->client->dev, "Failed to create ctrl %s %d\n",
					max96716_controls[i].name, priv->ctrls.error);
			ret = priv->ctrls.error;
			goto failed_out;
		}
	}

	for (i = 0; i < MAX96716_N_SINKS; i++)
		priv->pads[i].flags = MEDIA_PAD_FL_SINK;
	priv->pads[MAX96716_SRC_PAD].flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&priv->sd.entity, MAX96716_N_PADS, priv->pads);
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

int max9671x_powerup(struct i2c_client *client)
{
#define PULLUP_PIN(client, pin) \
	devm_gpio_request_one(&client->dev, pin, GPIOF_OUT_INIT_HIGH, NULL);\
	devm_gpio_free(&client->dev, pin)

	PULLUP_PIN(client, 658);
	PULLUP_PIN(client, 648);
//	gpio_set_value(658, 1);
//	gpio_set_value(648, 1);
	msleep(20);
	PULLUP_PIN(client, 649);
	PULLUP_PIN(client, 650);
	PULLUP_PIN(client, 651);
//	gpio_set_value(650, 1);
//	gpio_set_value(651, 1);
	msleep(10);
	PULLUP_PIN(client, 657);
	PULLUP_PIN(client, 657);
	PULLUP_PIN(client, 659);
	PULLUP_PIN(client, 659);
	PULLUP_PIN(client, 660);
	PULLUP_PIN(client, 660);
//	gpio_set_value(659, 1);
//	gpio_set_value(659, 1);
//	gpio_set_value(660, 1);
//	gpio_set_value(660, 1);
	msleep(20);
	PULLUP_PIN(client, 833);
	PULLUP_PIN(client, 985);
	PULLUP_PIN(client, 838);
//	gpio_set_value(833, 1);
//	gpio_set_value(985, 1);
	msleep(50);

#undef PULLUP_PIN

	return 0;
}
EXPORT_SYMBOL_GPL(max9671x_powerup);

static int max96716_init(struct max96716_priv *priv)
{
	int ret;
	unsigned int val;
	unsigned short tmp_addr;
	struct max9671x_platform_data *pdata = priv->platform_data;
	struct max9671x_subdev_info *info = &pdata->subdev_info[0];

	/* chip identify */
	ret = max96716_read(priv, 0x0D, &val);
	if (ret) {
		dev_err(&priv->client->dev, "Failed to read reg %x %d\n",
				0xD, ret);
		return ret;
	}

	if (val != 0xBE) {
		dev_err(&priv->client->dev, "Failed to detect max96716 %x\n", val);
		return -ENXIO;
	}

	/* internal regulator */
	ret = max96716_read(priv, 0x10, &val);
	max96716_write(priv, 0x10, val | 0x04);
	ret = max96716_read(priv, 0x12, &val);
	max96716_write(priv, 0x12, val | 0x10);

	/* CFGH {A/B/C/D} VIDEO {X/Y/Z/U} */
	/* VIDEO PIPE SEL */
	ret = max96716_write_reg_list(priv, &video_pipe_setting);

	/* MIPI TX */
	ret = max96716_write_reg_list(priv, &mipi_ctrl_setting);

	/* MIPI PHY */
	ret = max96716_write_reg_list(priv, &mipi_phy_setting);

	/* RLMS setting */
	ret = max96716_write_reg_list(priv, &rlms_conf);

	/* RESET_ONESHOT A/B */
	ret = max96716_read(priv, 0x10, &val);
	max96716_write(priv, 0x10, val | 0x20);
	ret = max96716_read(priv, 0x12, &val);
	max96716_write(priv, 0x12, val | 0x20);
	msleep(DELAY_MS);

	/* Link lock */
	priv->source_mask = MAX96716_LINK_STATUS;
	dev_info(&priv->client->dev, "Link status %x\n", priv->source_mask);

	if (!priv->source_mask) {
		dev_err(&priv->client->dev, "No remote devices connected\n");
		return -ENXIO;
	}

	/* enable rem cc */
	max96716_read(priv, 0x01, &val);
	max96716_write(priv, 0x01,  val & ~(1 << 4));
	max96716_read(priv, 0x03, &val);
	max96716_write(priv, 0x03,  val & ~(1 << 2));

	/* get current addr in use */
	if (max96716_read_rem(priv, info->phy_i2c_addr, 0x10, &val))
		tmp_addr = info->alias_addr;
	else
		tmp_addr = info->phy_i2c_addr;

	/* broadcast to all connected remote */
	ret = max96716_write_rem(priv, tmp_addr,
			0x00, info->alias_addr << 1);
	if (ret)
		return ret;

	ret = max96716_write_rem_reg_list(priv, info->alias_addr,
			&link_setting);
	if (ret)
		return ret;

	/* disnable all CC*/
	max96716_read(priv, 0x01, &val);
	max96716_write(priv, 0x01,  val | (1 << 4));
	max96716_read(priv, 0x03, &val);
	max96716_write(priv, 0x03,  val | (1 << 2));

	/* Link lock */
	priv->source_mask = MAX96716_LINK_STATUS;
	dev_info(&priv->client->dev, "Link status %x\n", priv->source_mask);

	if (!priv->source_mask) {
		dev_err(&priv->client->dev, "No remote devices connected\n");
		return -ENXIO;
	}

	return 0;
}

static irqreturn_t max96716_threaded_irq_fn(int irq, void *devid)
{
	struct max96716_priv *priv = devid;

	dev_dbg(&priv->client->dev, "IRQ triggered  %x\n", irq);

	return IRQ_HANDLED;
}

static int max96716_probe(struct i2c_client *client)
{
	struct max96716_priv *priv;
	int ret;

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
				NULL, max96716_threaded_irq_fn,
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
				NULL, max96716_threaded_irq_fn,
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

	ret = max96716_init(priv);
	if (ret) {
		dev_err(&client->dev, "failed to init max96716 %d\n", ret);
		goto probe_err;
	}

	ret = max96716_register_subdev(priv);
	if (ret) {
		dev_err(&priv->client->dev,
				"%s : failed to register subdev\n", __func__);
		goto probe_err;
	}

	return 0;

probe_err:

	mutex_destroy(&priv->mutex);

	return ret;
}

static int max96716_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct max96716_priv *priv = to_max96716(sd);

	mutex_destroy(&priv->mutex);

	v4l2_ctrl_handler_free(&priv->ctrls);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_device_unregister_subdev(sd);

	return 0;
}

/* no power or clk control */
static int max96716_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct max96716_priv *priv = to_max96716(subdev);
	int ret;
	int i;

	for (i = 0; i < MAX96716_N_SINKS; i++)
		if (max96716_set_sub_stream[i]) {
			ret = max96716_s_stream_vc(priv, i, 0);
			if (ret)
				dev_err(&client->dev, "failed to stop link %d ret %d\n", i, ret);
		}

	return 0;
}

/* re-initialize the link and resume streaming if needed */
static int max96716_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *subdev = i2c_get_clientdata(client);
	struct max96716_priv *priv = to_max96716(subdev);
	int ret;
	int i;

	ret = max96716_init(priv);
	if (ret) {
		dev_err(&client->dev, "%s : resume fail %d\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < MAX96716_N_SINKS; i++)
		if (max96716_set_sub_stream[i]) {
			ret = max96716_s_stream_vc(priv, i, 1);
			if (ret)
				dev_err(&client->dev, "failed to start	link %d ret %d\n", i, ret);
		}

	return 0;
}

static const struct i2c_device_id max96716_id_table[] = {
	{"max96716", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, max96716_id_table);

static const struct dev_pm_ops max96716_pm_ops = {
	.suspend = max96716_suspend,
	.resume = max96716_resume,
};

static struct i2c_driver max96716_i2c_driver = {
	.driver = {
		.name = "max96716",
		.pm = &max96716_pm_ops,
	},
	.probe_new = max96716_probe,
	.remove = max96716_remove,
	.id_table = max96716_id_table,
};

module_i2c_driver(max96716_i2c_driver);

MODULE_DESCRIPTION("Maxim MAX96716 GMSL Deserializer Driver");
MODULE_AUTHOR("Intel");
MODULE_LICENSE("GPL");
