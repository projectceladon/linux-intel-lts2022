/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2020 Intel Corporation */

#ifndef MAX9671X_H
#define MAX9671X_H

#include <linux/i2c.h>

#define MAX_SER_9295A 0x1
#define MAX_SER_96717F 0x2

#define ID_9295A 0x91
#define ID_96717F 0xc8

#define MAX_PORT_SIOA 0x0
#define MAX_PORT_SIOB 0x1
#define MAX_PORT_SIOC 0x2
#define MAX_PORT_SIOD 0x3

struct max9671x_subdev_platform_data {
	unsigned int fsync_gpio;
	unsigned int mode;
	char suffix;
};

struct max9671x_subdev_info {
	struct i2c_board_info board_info;
	unsigned int rx_port;
	unsigned int power_gpio;
	unsigned int phy_i2c_addr;
	unsigned int alias_addr;
	unsigned int ser_type;
	unsigned int pipe;
	unsigned int str_id;
	unsigned int soft_vc;
	unsigned int soft_dt;
	unsigned int soft_bpp;
	unsigned int mux_mode;
	unsigned int fsync_gpio;
	char suffix;
};

struct max9671x_platform_data {
	unsigned int subdev_num;
	struct max9671x_subdev_info *subdev_info;
	unsigned int lock_gpio;
	unsigned int errb_gpio;
	unsigned int pwdnb_gpio;
	char errb_gpio_name[16];
	unsigned int errb_gpio_flags;
	unsigned int phy_id;
	unsigned int phy_lanes;
	unsigned int phy_freq;
	unsigned int fsync_tx_id;
	unsigned int fsync_period;
	char suffix;
};

#endif
