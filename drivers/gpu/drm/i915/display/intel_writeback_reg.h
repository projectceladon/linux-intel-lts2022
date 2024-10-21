/* SPDX-License-Identifier: MIT*/
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _INTEL_WRITEBACK_REG_H_
#define _INTEL_WRITEBACK_REG_H_

#include "i915_reg.h"
/* Gen12 WD */
#define _MMIO_WD(tc, wd0, wd1)		_MMIO_TRANS((tc) - TRANSCODER_WD_0, \
							wd0, wd1)

#define WD_TRANS_ENABLE			(1 << 31)
#define WD_TRANS_DISABLE		0
#define WD_TRANS_ACTIVE			(1 << 30)

/* WD transcoder control */
#define _WD_TRANS_FUNC_CTL_0		0x6e400
#define _WD_TRANS_FUNC_CTL_1		0x6ec00
#define WD_TRANS_FUNC_CTL(tc)		_MMIO_WD(tc,\
					_WD_TRANS_FUNC_CTL_0,\
					_WD_TRANS_FUNC_CTL_1)

#define TRANS_WD_FUNC_ENABLE		(1 << 31)
#define WD_TRIGGERED_CAP_MODE_ENABLE	(1 << 30)
#define START_TRIGGER_FRAME		(1 << 29)
#define STOP_TRIGGER_FRAME		(1 << 28)
#define WD_CTL_POINTER_ETEH		(0 << 18)
#define WD_CTL_POINTER_ETDH		(1 << 18)
#define WD_CTL_POINTER_DTDH		(2 << 18)
#define WD_INPUT_SELECT_MASK		(7 << 12)
#define WD_INPUT_PIPE_A			(0 << 12)
#define WD_INPUT_PIPE_B			(5 << 12)
#define WD_INPUT_PIPE_C			(6 << 12)
#define WD_INPUT_PIPE_D			(7 << 12)

#define WD_PIX_FMT_MASK			(0x3 << 20)
#define WD_PIX_FMT_YUYV			(0x1 << 20)
#define WD_PIX_FMT_XYUV8888		(0x2 << 20)
#define WD_PIX_FMT_XBGR8888		(0x3 << 20)
#define WD_PIX_FMT_Y410			(0x4 << 20)
#define WD_PIX_FMT_YUV422		(0x5 << 20)
#define WD_PIX_FMT_XBGR2101010		(0x6 << 20)
#define WD_PIX_FMT_RGB565		(0x7 << 20)

#define WD_FRAME_NUMBER_MASK		15

#define _WD_STRIDE_0			0x6e510
#define _WD_STRIDE_1			0x6ed10
#define WD_STRIDE(tc)			_MMIO_WD(tc,\
					_WD_STRIDE_0,\
					_WD_STRIDE_1)
#define WD_STRIDE_SHIFT			6
#define WD_STRIDE_MASK			(0x3ff << WD_STRIDE_SHIFT)

#define _WD_STREAMCAP_CTL0		0x6e590
#define _WD_STREAMCAP_CTL1		0x6ed90
#define WD_STREAMCAP_CTL(tc)		_MMIO_WD(tc,\
					_WD_STREAMCAP_CTL0,\
					_WD_STREAMCAP_CTL1)

#define WD_STREAM_CAP_MODE_EN		(1 << 31)
#define WD_STRAT_MASK			(3 << 24)
#define WD_SLICING_STRAT_1_1		(0 << 24)
#define WD_SLICING_STRAT_2_1		(1 << 24)
#define WD_SLICING_STRAT_4_1		(2 << 24)
#define WD_SLICING_STRAT_8_1		(3 << 24)
#define WD_STREAM_OVERRUN_STATUS	1

#define _WD_SURF_0			0x6e514
#define _WD_SURF_1			0x6ed14
#define WD_SURF(tc)			_MMIO_WD(tc,\
					_WD_SURF_0,\
					_WD_SURF_1)

#define _WD_IMR_0			0x6e560
#define _WD_IMR_1			0x6ed60
#define WD_IMR(tc)			_MMIO_WD(tc,\
					_WD_IMR_0,\
					_WD_IMR_1)
#define WD_FRAME_COMPLETE_INT		(1 << 7)
#define WD_GTT_FAULT_INT		(1 << 6)
#define WD_VBLANK_INT			(1 << 5)
#define WD_OVERRUN_INT			(1 << 4)
#define WD_CAPTURING_INT		(1 << 3)
#define WD_WRITE_COMPLETE_INT		(1 << 2)

#define _WD_IIR_0			0x6e564
#define _WD_IIR_1			0x6ed64
#define WD_IIR(tc)			_MMIO_WD(tc,\
					_WD_IIR_0,\
					_WD_IIR_1)

#define _WD_FRAME_STATUS_0		0x6e56b
#define _WD_FRAME_STATUS_1		0x6ed6b
#define WD_FRAME_STATUS(tc)		_MMIO_WD(tc,\
					_WD_FRAME_STATUS_0,\
					_WD_FRAME_STATUS_1)

#define WD_FRAME_COMPLETE		(1 << 31)
#define WD_STATE_IDLE			(0 << 24)
#define WD_STATE_CAPSTART		(1 << 24)
#define WD_STATE_FRAME_START		(2 << 24)
#define WD_STATE_CAPACITIVE		(3 << 24)
#define WD_STATE_TG_DONE		(4 << 24)
#define WD_STATE_WDX_DONE		(5 << 24)
#define WD_STATE_QUICK_CAP		(6 << 24)

#define _WD_27_M_0			0x6e524
#define _WD_27_M_1			0x6ed24
#define WD_27_M(tc)			_MMIO_WD(tc,\
					_WD_27_M_0,\
					_WD_27_M_1)

#define _WD_27_N_0			0x6e528
#define _WD_27_N_1			0x6ec28
#define WD_27_N(tc)			_MMIO_WD(tc,\
					_WD_27_N_0,\
					_WD_27_N_1)

#define _WD_TAIL_CFG_0			0x6e520
#define _WD_TAIL_CFG_1			0x6ed20

#define WD_TAIL_CFG(tc)			_MMIO_WD(tc,\
					_WD_TAIL_CFG_0,\
					_WD_TAIL_CFG_1)
#endif /* _INTEL_WRITEBACK_REG_H_ */
