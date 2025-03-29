/* SPDX-License-Identifier: MIT*/
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _INTEL_WB_H
#define _INTEL_WB_H

#include <drm/drm_crtc.h>

#include "intel_display_types.h"

#define I915_MAX_WD_TANSCODERS 2
#define WB_DUMP_DEBUG          0

struct intel_wb {
	struct intel_encoder base;
	struct drm_writeback_connector wb_conn;
	struct intel_crtc *wb_crtc;
	intel_wakeref_t io_wakeref[I915_MAX_WD_TANSCODERS];
	enum transcoder trans;
	struct i915_vma *vma;
	unsigned long flags;
	struct drm_writeback_job *job;
	int triggered_cap_mode;
	int frame_num;
	bool stream_cap;
	bool start_capture;
	int slicing_strategy;
};

static inline struct intel_wb *enc_to_intel_wb(struct intel_encoder *encoder)
{
	return container_of(&encoder->base, struct intel_wb, base.base);
}

static inline struct intel_wb *wb_conn_to_intel_wb(struct drm_writeback_connector *wb_conn)
{
	return container_of(wb_conn, struct intel_wb, wb_conn);
}

void intel_wb_init(struct drm_i915_private *dev_priv, enum transcoder trans);
void intel_wb_enable_capture(struct intel_crtc_state *pipe_config,
			     struct drm_connector_state *conn_state);
void intel_wb_handle_isr(struct drm_i915_private *dev_priv);
void intel_wb_set_vblank_event(struct intel_atomic_state *state,
			       struct intel_crtc *crtc,
			       struct intel_crtc_state *crtc_state);
struct drm_writeback_job *intel_get_writeback_job_from_queue(struct intel_wb *intel_wb);
#endif/* _INTEL_WB_H */
