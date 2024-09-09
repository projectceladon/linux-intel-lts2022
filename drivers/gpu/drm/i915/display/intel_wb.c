// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_fourcc.h>

#include "intel_atomic.h"
#include "intel_connector.h"
#include "intel_wb.h"
#include "intel_fb_pin.h"
#include "intel_de.h"
#include "intel_writeback_reg.h"

#define CSC_MAX_COEFF_REG_COUNT    6
#define CSC_MAX_OFFSET_COUNT       3

enum {
	WD_CAPTURE_4_PIX,
	WD_CAPTURE_2_PIX,
} wb_capture_format;

struct drm_writeback_job
*intel_get_writeback_job_from_queue(struct intel_wb *intel_wb)
{
	struct drm_writeback_job *job;
	struct drm_i915_private *i915 = to_i915(intel_wb->base.base.dev);
	struct drm_writeback_connector *wb_conn =
		&intel_wb->wb_conn;
	unsigned long flags;

	spin_lock_irqsave(&wb_conn->job_lock, flags);
	job = list_first_entry_or_null(&wb_conn->job_queue,
				       struct drm_writeback_job,
				       list_entry);
	spin_unlock_irqrestore(&wb_conn->job_lock, flags);
	if (!job)
		drm_dbg_kms(&i915->drm, "job queue is empty\n");

	return job;
}

static const u32 wb_fmts[] = {
	DRM_FORMAT_YUV444,
	DRM_FORMAT_XYUV8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_Y410,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_RGB565,
};

static int intel_wb_get_format(int pixel_format)
{
	int wb_format = -EINVAL;

	DRM_INFO("Get format pixel format %x\n",
		  pixel_format);

	switch (pixel_format) {
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_XYUV8888:
	case DRM_FORMAT_YUV444:
		wb_format = WD_CAPTURE_4_PIX;
		break;
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_RGB565:
		wb_format = WD_CAPTURE_2_PIX;
		break;
	default:
		DRM_ERROR("unsupported pixel format %x!\n",
			  pixel_format);
	}
	DRM_INFO("Get format wb format %x\n",
		  wb_format);

	return wb_format;
}

static int intel_wb_verify_pix_format(int format)
{
	const struct drm_format_info *info = drm_format_info(format);
	int pix_format = info->format;
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(wb_fmts); i++)
		if (pix_format == wb_fmts[i])
			return 0;

	return -EINVAL;
}

static u32 intel_wb_get_stride(const struct intel_crtc_state *crtc_state,
			       int format)
{
	const struct drm_format_info *info = drm_format_info(format);
	int wb_format;
	int hactive, pixel_size;

	wb_format = intel_wb_get_format(info->format);

	switch (wb_format) {
	case WD_CAPTURE_4_PIX:
		pixel_size = 4;
		break;
	case WD_CAPTURE_2_PIX:
		pixel_size = 2;
		break;
	default:
		pixel_size = 1;
		break;
	}

	hactive = crtc_state->hw.adjusted_mode.crtc_hdisplay;

	return DIV_ROUND_UP(hactive * pixel_size, 64);
}

static int intel_wb_pin_fb(struct intel_wb *intel_wb,
			   struct drm_framebuffer *fb)
{
	const struct i915_gtt_view view = {
		.type = I915_GTT_VIEW_NORMAL,
	};
	struct i915_vma *vma;

	vma = intel_pin_and_fence_fb_obj(fb, false, &view, true,
					 &intel_wb->flags);

	if (IS_ERR(vma))
		return PTR_ERR(vma);

	intel_wb->vma = vma;
	return 0;
}

static void intel_configure_slicing_strategy(struct drm_i915_private *i915,
					     struct intel_wb *intel_wb,
					     u32 tmp)
{
	drm_dbg_kms(&i915->drm, "WD_STREAMCAP_CTL: 0x%8x\n", tmp);

	tmp &= ~WD_STRAT_MASK;
	drm_dbg_kms(&i915->drm, "WD_STREAMCAP_CTL: 0x%8x\n", tmp);
	if (intel_wb->slicing_strategy == 1)
		tmp |= WD_SLICING_STRAT_1_1;
	else if (intel_wb->slicing_strategy == 2)
		tmp |= WD_SLICING_STRAT_2_1;
	else if (intel_wb->slicing_strategy == 3)
		tmp |= WD_SLICING_STRAT_4_1;
	else if (intel_wb->slicing_strategy == 4)
		tmp |= WD_SLICING_STRAT_8_1;

	drm_dbg_kms(&i915->drm, "Slicing_strategy WD_STREAMCAP_CTL(0x%05x): 0x%08x\n", WD_STREAMCAP_CTL(intel_wb->trans).reg, tmp);

	intel_de_write(i915, WD_STREAMCAP_CTL(intel_wb->trans),
		       tmp);
	drm_dbg_kms(&i915->drm, "WD Transocder: %05x, WD_STREAMCAP_CTL                    = %08x\n",
		WD_STREAMCAP_CTL(intel_wb->trans).reg, intel_de_read(i915, WD_STREAMCAP_CTL(intel_wb->trans)));

}

static enum drm_mode_status
intel_wb_mode_valid(struct drm_connector *connector,
		    struct drm_display_mode *mode)
{
	return MODE_OK;
}

static int intel_wb_get_modes(struct drm_connector *connector)
{
	return 0;
}

static void intel_wb_get_config(struct intel_encoder *encoder,
				struct intel_crtc_state *pipe_config)
{
	struct intel_crtc *intel_crtc =
		to_intel_crtc(pipe_config->uapi.crtc);

	if (intel_crtc) {
		memcpy(pipe_config, intel_crtc->config,
		       sizeof(*pipe_config));
		pipe_config->output_types |= BIT(INTEL_OUTPUT_WB);
	}
}

static int intel_wb_compute_config(struct intel_encoder *encoder,
				   struct intel_crtc_state *pipe_config,
				   struct drm_connector_state *conn_state)
{
	struct intel_wb *intel_wb = enc_to_intel_wb(encoder);
	struct drm_writeback_job *job;

	job = intel_get_writeback_job_from_queue(intel_wb);
	if (job || conn_state->writeback_job) {
		/*
		 * Saving reference of pipe/crtc for later use if
		 * writeback job is present
		 */
		intel_wb->wb_crtc = to_intel_crtc(pipe_config->uapi.crtc);
		return 0;
	}

	return 0;
}

static void intel_wb_get_power_domains(struct intel_encoder *encoder,
				       struct intel_crtc_state *crtc_state)
{
	struct drm_i915_private *i915 = to_i915(encoder->base.dev);
	struct intel_wb *intel_wb = enc_to_intel_wb(encoder);
	intel_wakeref_t wakeref;

	wakeref = intel_display_power_get(i915, encoder->power_domain);

	intel_wb->io_wakeref[0] = wakeref;
}

static bool intel_wb_get_hw_state(struct intel_encoder *encoder,
				  enum pipe *pipe)
{
	bool ret = false;
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_wb *intel_wb = enc_to_intel_wb(encoder);
	struct intel_crtc *wb_crtc = intel_wb->wb_crtc;
	intel_wakeref_t wakeref;
	u32 tmp;
	drm_dbg_kms(&dev_priv->drm, "intel_wb_get_hw_state\n");

	if (!wb_crtc)
		return false;

	wakeref = intel_display_power_get_if_enabled(dev_priv,
						     encoder->power_domain);

	if (!wakeref)
		goto out;

	tmp = intel_de_read(dev_priv, PIPECONF(intel_wb->trans));
	ret = tmp & WD_TRANS_ACTIVE;
	if (ret) {
		drm_dbg_kms(&dev_priv->drm, "intel_wb_get_hw_state WD Transcode active\n");
		*pipe = wb_crtc->pipe;
		return true;
	}

out:
	intel_display_power_put(dev_priv, encoder->power_domain, wakeref);
	drm_dbg_kms(&dev_priv->drm, "intel_wb_get_hw_state WD Transcode active fail\n");
	return false;
}

static int intel_wb_encoder_atomic_check(struct drm_encoder *encoder,
					 struct drm_crtc_state *crtc_st,
					 struct drm_connector_state *conn_st)
{
	/* Check for the format and buffers and property validity */
	struct drm_framebuffer *fb;
	struct drm_writeback_job *job = conn_st->writeback_job;
	struct drm_i915_private *i915 = to_i915(encoder->dev);
	const struct drm_display_mode *mode = &crtc_st->mode;
	int ret;

	if (!job) {
		drm_dbg_kms(&i915->drm, "No writeback job created returning\n");
		crtc_st->no_vblank = true;
		return 0;
	}

	fb = job->fb;
	if (!fb) {
		drm_dbg_kms(&i915->drm, "Invalid framebuffer\n");
		return -EINVAL;
	}

	if (fb->width != mode->hdisplay || fb->height != mode->vdisplay) {
		drm_dbg_kms(&i915->drm, "Invalid framebuffer size %ux%u\n",
			    fb->width, fb->height);
		return -EINVAL;
	}

	ret = intel_wb_verify_pix_format(fb->format->format);
	if (ret) {
		drm_dbg_kms(&i915->drm, "Unsupported framebuffer format %08x\n",
			    fb->format->format);
		return -EINVAL;
	}

	return 0;
}

static const struct drm_encoder_helper_funcs wb_encoder_helper_funcs = {
	.atomic_check = intel_wb_encoder_atomic_check,
};

static void intel_wb_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
}

static enum drm_connector_status
intel_wb_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static void intel_wb_cleanup_writeback_job(struct drm_writeback_connector *connector,
					   struct drm_writeback_job *job)
{
	struct intel_wb *intel_wb = wb_conn_to_intel_wb(connector);

	intel_unpin_fb_vma(intel_wb->vma, intel_wb->flags);
}

static const struct drm_connector_funcs wb_connector_funcs = {
	.detect = intel_wb_connector_detect,
	.reset = drm_atomic_helper_connector_reset,
	.destroy = intel_wb_connector_destroy,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
};

static const struct drm_connector_helper_funcs wb_connector_helper_funcs = {
	.get_modes = intel_wb_get_modes,
	.mode_valid = intel_wb_mode_valid,
	.cleanup_writeback_job = intel_wb_cleanup_writeback_job,
};

static const struct drm_encoder_funcs drm_writeback_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static bool intel_fastset_dis(struct intel_encoder *encoder,
			      struct intel_crtc_state *pipe_config)
{
	return false;
}

static void intel_wb_connector_init(struct intel_wb *intel_wb)
{
	drm_atomic_helper_connector_reset(&intel_wb->wb_conn.base);
}

static void intel_wb_disable_capture(struct intel_wb *intel_wb)
{
	struct drm_i915_private *dev_priv = to_i915(intel_wb->base.base.dev);
	u32 tmp;

	intel_de_write_fw(dev_priv, WD_IMR(intel_wb->trans), 0xFF);
	tmp = intel_de_read(dev_priv, PIPECONF(intel_wb->trans));
	tmp &= WD_TRANS_DISABLE;
	intel_de_write(dev_priv, PIPECONF(intel_wb->trans), tmp);
	tmp = intel_de_read(dev_priv, WD_TRANS_FUNC_CTL(intel_wb->trans));
	tmp |= WD_TRANS_DISABLE;
	intel_de_write(dev_priv, WD_TRANS_FUNC_CTL(intel_wb->trans), tmp);
}

void intel_wb_init(struct drm_i915_private *i915, enum transcoder trans)
{
	struct intel_wb *intel_wb;
	struct intel_encoder *encoder;
	struct drm_writeback_connector *wb_conn;
	int n_formats = ARRAY_SIZE(wb_fmts);
	struct drm_encoder *drm_enc;
	int err, ret;

	intel_wb = kzalloc(sizeof(*intel_wb), GFP_KERNEL);
	if (!intel_wb)
		return;

	intel_wb_connector_init(intel_wb);
	encoder = &intel_wb->base;
	drm_enc = &encoder->base;
	wb_conn = &intel_wb->wb_conn;
	intel_wb->trans = trans;
	intel_wb->triggered_cap_mode = 1;
	intel_wb->frame_num = 1;
	intel_wb->stream_cap = true;
	intel_wb->slicing_strategy = 1;
	encoder->get_config = intel_wb_get_config;
	encoder->compute_config = intel_wb_compute_config;
	encoder->get_hw_state = intel_wb_get_hw_state;
	encoder->type = INTEL_OUTPUT_WB;
	encoder->cloneable = 0;
	encoder->pipe_mask = ~0;
	encoder->power_domain = POWER_DOMAIN_TRANSCODER_B;
	encoder->get_power_domains = intel_wb_get_power_domains;
	encoder->initial_fastset_check = intel_fastset_dis;

	drm_encoder_helper_add(drm_enc,
			       &wb_encoder_helper_funcs);

	drm_enc->possible_crtcs = ~0;
	ret = drm_encoder_init(&i915->drm, drm_enc,
			       &drm_writeback_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, NULL);

	if (ret) {
		drm_dbg_kms(&i915->drm,
			    "Writeback drm_encoder init Failed: %d\n",
			    ret);
		goto cleanup;
	}

	err = drm_writeback_connector_init_with_encoder(&i915->drm,
							wb_conn,
							drm_enc,
							&wb_connector_funcs,
							wb_fmts, n_formats);

	if (err != 0) {
		drm_dbg_kms(&i915->drm,
			    "drm_writeback_connector_init: Failed: %d\n",
			    err);
		goto cleanup;
	}

	wb_conn->base.encoder = drm_enc;
	drm_connector_helper_add(&wb_conn->base, &wb_connector_helper_funcs);
	wb_conn->base.status = connector_status_connected;
	return;

cleanup:
	kfree(intel_wb);
	return;
}

static void intel_wb_writeback_complete(struct intel_wb *intel_wb,
					struct drm_writeback_job *job,
					int status)
{
	struct drm_writeback_connector *wb_conn =
		&intel_wb->wb_conn;
	drm_writeback_signal_completion(wb_conn, status);
}

static void wd_dump_details(struct intel_crtc *intel_crtc, struct intel_wb *intel_wb)
{
	struct drm_i915_private *dev_priv = to_i915(intel_crtc->base.dev);
	//int wd_pipe = intel_crtc->pipe;
	//int i = 0;
	drm_dbg_kms(&dev_priv->drm, "\nDumping WD Transcoder details\n");
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_FUNC_CTL(TRANS_WD_FUNC_CTL_0)    = %08x\n",
		 WD_TRANS_FUNC_CTL(intel_wb->trans).reg, intel_de_read(dev_priv, WD_TRANS_FUNC_CTL(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, Stride(WD_STRIDE_0)                 = %08x\n",
		 WD_STRIDE(intel_wb->trans).reg, intel_de_read(dev_priv, WD_STRIDE(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, Surf(WD_SURF_0)                     = %08x\n",
		 WD_SURF(intel_wb->trans).reg, intel_de_read(dev_priv, WD_SURF(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_TAIL_CFG                         = %08x\n",
		WD_TAIL_CFG(intel_wb->trans).reg, intel_de_read(dev_priv, WD_TAIL_CFG(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_TAIL_CFG2                        = %08x\n",
		WD_TAIL_CFG2(intel_wb->trans).reg, intel_de_read(dev_priv, WD_TAIL_CFG2(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, IMR(WD_IMR_0)                       = %08x\n",
		WD_IMR(intel_wb->trans).reg, intel_de_read(dev_priv, WD_IMR(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_IIR                              = %08x\n",
		WD_IIR(intel_wb->trans).reg, intel_de_read(dev_priv, WD_IIR(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_FRAME_STATUS                     = %08x\n",
		WD_FRAME_STATUS(intel_wb->trans).reg, intel_de_read(dev_priv, WD_FRAME_STATUS(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, HTotal(TRANS_HTOTAL_WD0)            = %08x\n",
		 HTOTAL(intel_wb->trans).reg, intel_de_read(dev_priv, HTOTAL(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, VTotal(TRANS_VTOTAL_WD0)            = %08x\n",
		 VTOTAL(intel_wb->trans).reg, intel_de_read(dev_priv, VTOTAL(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transcoder: %05x, Trans_Conf(TRANS_CONF_WD0)          = %08x\n",
		 PIPECONF(intel_wb->trans).reg, intel_de_read(dev_priv, PIPECONF(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_27_M_0                           = %08x\n",
		WD_27_M(intel_wb->trans).reg, intel_de_read(dev_priv, WD_27_M(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_27_N_0                           = %08x\n",
		WD_27_N(intel_wb->trans).reg, intel_de_read(dev_priv, WD_27_N(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_STREAMCAP_CTL                    = %08x\n",
		WD_STREAMCAP_CTL(intel_wb->trans).reg, intel_de_read(dev_priv, WD_STREAMCAP_CTL(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_VFID                             = %08x\n",
		 WD_VFID(intel_wb->trans).reg, intel_de_read(dev_priv, WD_VFID(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_CHICKEN                          = %08x\n",
		WD_CHICKEN(intel_wb->trans).reg, intel_de_read(dev_priv, WD_CHICKEN(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, DEBUG_1                             = %08x\n",
		WD_DEBUG1(intel_wb->trans).reg, intel_de_read(dev_priv, WD_DEBUG1(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, DEBUG_2                             = %08x\n",
		WD_DEBUG2(intel_wb->trans).reg, intel_de_read(dev_priv, WD_DEBUG2(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_PERF_CNT                         = %08x\n",
		WD_PERF_CNT(intel_wb->trans).reg, intel_de_read(dev_priv, WD_PERF_CNT(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_TAIL_MSG_DBG                     = %08x\n",
		 WD_TAIL_MSG_DBG(intel_wb->trans).reg, intel_de_read(dev_priv, WD_TAIL_MSG_DBG(intel_wb->trans)));
	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, Notific(WD_MSG_MASK_0)              = %08x\n",
		 WD_MSG_MASK(intel_wb->trans).reg, intel_de_read(dev_priv, WD_MSG_MASK(intel_wb->trans)));

	drm_dbg_kms(&dev_priv->drm, "WD Transocder: %05x, WD_CTL_MSG_DBG                      = %08x\n",
		WD_CTL_MSG_DBG(intel_wb->trans).reg, intel_de_read(dev_priv, WD_CTL_MSG_DBG(intel_wb->trans)));

}

#if WB_DUMP_DEBUG
static int wd_dump_first_time = 1;
#endif

static int intel_wb_setup_transcoder(struct intel_wb *intel_wb,
				     struct intel_crtc_state *pipe_config,
				     struct drm_connector_state *conn_state,
				     struct drm_writeback_job *job)
{
	struct intel_crtc *intel_crtc = to_intel_crtc(pipe_config->uapi.crtc);
	enum pipe pipe = intel_crtc->pipe;
	struct drm_framebuffer *fb;
	struct drm_i915_private *dev_priv = to_i915(intel_crtc->base.dev);
	struct drm_gem_object *wb_fb_obj;
	int ret;
	u32 stride, tmp;
	u16 hactive, vactive;
	drm_dbg_kms(&dev_priv->drm,
			"intel_wb_setup_transcoder start\n");

	fb = job->fb;
	wb_fb_obj = fb->obj[0];
	if (!wb_fb_obj) {
		drm_dbg_kms(&dev_priv->drm, "No framebuffer gem object created\n");
		return -EINVAL;
	}
#if WB_DUMP_DEBUG
	if(wd_dump_first_time)
	{
		drm_dbg_kms(&dev_priv->drm, "wd_go FIRST TIME!!!!!!!!!!!!!!!!!!!!!------------------------->>>>>>>>>>>>> \n");
		wd_dump_details(intel_crtc, intel_wb);
		drm_dbg_kms(&dev_priv->drm, "wd_go AFTER FIRST TIME!!!!!!!!!!!!!!!-------------------------<<<<<<<<<<<<< \n");
		wd_dump_first_time = 0;
	}
#endif
	ret = intel_wb_pin_fb(intel_wb, fb);
	drm_WARN_ON(&dev_priv->drm, ret != 0);
	/* Write stride and surface registers in that particular order */
	stride = intel_wb_get_stride(pipe_config, fb->format->format);

	tmp = intel_de_read(dev_priv, WD_STRIDE(intel_wb->trans));
	tmp &= ~WD_STRIDE_MASK;
	tmp |= (stride << WD_STRIDE_SHIFT);
	drm_dbg_kms(&dev_priv->drm, "WD_STRIDE: 0x%05x, tmp: 0x%08x\n", WD_STRIDE(intel_wb->trans).reg, tmp);

	intel_de_write(dev_priv, WD_STRIDE(intel_wb->trans), tmp);

	tmp = intel_de_read(dev_priv, WD_SURF(intel_wb->trans));
	drm_dbg_kms(&dev_priv->drm, "WD_SURF: 0x%05x, tmp: 0x%08x\n", WD_SURF(intel_wb->trans).reg, tmp);

	intel_de_write(dev_priv, WD_SURF(intel_wb->trans),
		       i915_ggtt_offset(intel_wb->vma));
	drm_dbg_kms(&dev_priv->drm, "WD_SURF: 0x%05x, i915_ggtt_offset: 0x%08x\n", WD_SURF(intel_wb->trans).reg, i915_ggtt_offset(intel_wb->vma));

	tmp = intel_de_read_fw(dev_priv, WD_IIR(intel_wb->trans));
	drm_dbg_kms(&dev_priv->drm, "WD_IIR: 0x%05x, tmp: 0x%08x\n", WD_IIR(intel_wb->trans).reg, tmp);

	intel_de_write_fw(dev_priv, WD_IIR(intel_wb->trans), tmp);

	tmp = ~(WD_GTT_FAULT_INT | WD_WRITE_COMPLETE_INT | WD_FRAME_COMPLETE_INT |
		WD_VBLANK_INT | WD_OVERRUN_INT | WD_CAPTURING_INT);
	drm_dbg_kms(&dev_priv->drm, "WD_IMR: 0x%05x, tmp: 0x08%x\n", WD_IMR(intel_wb->trans).reg, tmp);

	intel_de_write_fw(dev_priv, WD_IMR(intel_wb->trans), tmp);

	if (intel_wb->stream_cap) {
		tmp = intel_de_read(dev_priv,
				    WD_STREAMCAP_CTL(intel_wb->trans));
		tmp |= WD_STREAM_CAP_MODE_EN;
		drm_dbg_kms(&dev_priv->drm, "WD_STREAMCAP_CTL: 0x%8x\n", tmp);
		intel_configure_slicing_strategy(dev_priv, intel_wb, tmp);
	}

	hactive = pipe_config->uapi.mode.hdisplay;
	vactive = pipe_config->uapi.mode.vdisplay;
	tmp = intel_de_read(dev_priv, HTOTAL(intel_wb->trans));
	drm_dbg_kms(&dev_priv->drm, "HTOTAL: 0x%05x, tmp: 0x%08x\n", HTOTAL(intel_wb->trans).reg, tmp);
	drm_dbg_kms(&dev_priv->drm, "hactive: 0x%08x\n", hactive);

	tmp = intel_de_read(dev_priv, VTOTAL(intel_wb->trans));
	drm_dbg_kms(&dev_priv->drm, "VTOTAL: 0x%05x, tmp: 0x%08x\n", VTOTAL(intel_wb->trans).reg, tmp);
	drm_dbg_kms(&dev_priv->drm, "vactive: 0x%08x\n", vactive);

	/* minimum hactive as per bspec: 64 pixels */
	if (hactive < 64)
		drm_err(&dev_priv->drm, "hactive is less then 64 pixels\n");

	intel_de_write(dev_priv, HTOTAL(intel_wb->trans), hactive - 1);
	intel_de_write(dev_priv, VTOTAL(intel_wb->trans), vactive - 1);

	tmp = intel_de_read(dev_priv, WD_TRANS_FUNC_CTL(intel_wb->trans));
	/* select pixel format */
	tmp &= ~WD_PIX_FMT_MASK;

	switch (fb->format->format) {
	default:
	fallthrough;
	case DRM_FORMAT_YUYV:
		tmp |= WD_PIX_FMT_YUYV;
		break;
	case DRM_FORMAT_XYUV8888:
		tmp |= WD_PIX_FMT_XYUV8888;
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_XRGB8888:
		tmp |= WD_PIX_FMT_XBGR8888;
		break;
	case DRM_FORMAT_Y410:
		tmp |= WD_PIX_FMT_Y410;
		break;
	case DRM_FORMAT_YUV422:
		tmp |= WD_PIX_FMT_YUV422;
		break;
	case DRM_FORMAT_XBGR2101010:
		tmp |= WD_PIX_FMT_XBGR2101010;
		break;
	case DRM_FORMAT_RGB565:
		tmp |= WD_PIX_FMT_RGB565;
		break;
	}

	if (intel_wb->triggered_cap_mode)
		tmp |= WD_TRIGGERED_CAP_MODE_ENABLE;

	if (intel_wb->stream_cap)
		tmp |= WD_CTL_POINTER_DTDH;

	/* select input pipe */
	tmp &= ~WD_INPUT_SELECT_MASK;
	pipe = PIPE_A;
	switch (pipe) {
	default:
		fallthrough;
	case PIPE_A:
		tmp |= WD_INPUT_PIPE_A;
		break;
	case PIPE_B:
		tmp |= WD_INPUT_PIPE_B;
		break;
	case PIPE_C:
		tmp |= WD_INPUT_PIPE_C;
		break;
	case PIPE_D:
		tmp |= WD_INPUT_PIPE_D;
		break;
	}

	/* enable DDI buffer */
	if (!(tmp & TRANS_WD_FUNC_ENABLE))
		tmp |= TRANS_WD_FUNC_ENABLE;
	drm_dbg_kms(&dev_priv->drm, "WD_TRANS_FUNC_CTL: 0x%05x, tmp: 0x%08x\n", WD_TRANS_FUNC_CTL(intel_wb->trans).reg, tmp);

	intel_de_write(dev_priv, WD_TRANS_FUNC_CTL(intel_wb->trans), tmp);

#if WB_DUMP_DEBUG
	drm_dbg_kms(&dev_priv->drm, "wd_go5-A1 - Reg Dump Before Enable\n");
	drm_dbg_kms(&dev_priv->drm, "wd_go5-A2 ------------------------------------------------------------\n");
	wd_dump_details(intel_crtc, intel_wb);
	drm_dbg_kms(&dev_priv->drm, "wd_go5-A3 ------------------------------------------------------------\n");
#endif
	tmp = intel_de_read(dev_priv, PIPECONF(intel_wb->trans));
	drm_dbg_kms(&dev_priv->drm, "PIPECONF: 0x%05x\n", PIPECONF(intel_wb->trans).reg);
	ret = tmp & WD_TRANS_ACTIVE;
	if (!ret) {
		/* enable the transcoder */
		tmp = intel_de_read(dev_priv, PIPECONF(intel_wb->trans));
		tmp |= WD_TRANS_ENABLE;
		drm_dbg_kms(&dev_priv->drm, "PIPECONF: 0x%08x\n", tmp);

		intel_de_write(dev_priv, PIPECONF(intel_wb->trans), tmp);

		/* wait for transcoder to be enabled */
		if (intel_de_wait_for_set(dev_priv, PIPECONF(intel_wb->trans),
					  WD_TRANS_ACTIVE, 100))
			drm_err(&dev_priv->drm, "WD transcoder could not be enabled\n");
	}

	return 0;
}

static int intel_wb_capture(struct intel_wb *intel_wb,
			    struct intel_crtc_state *pipe_config,
			    struct drm_connector_state *conn_state,
			    struct drm_writeback_job *job)
{
	u32 tmp;
	struct drm_i915_private *i915 = to_i915(intel_wb->base.base.dev);
	int ret = 0, status = 0;
	struct intel_crtc *wb_crtc = intel_wb->wb_crtc;
	unsigned long flags;
	drm_dbg_kms(&i915->drm,
			"intel_wb_capture start\n");

	if (!job->out_fence)
		drm_dbg_kms(&i915->drm, "Not able to get out_fence for job\n");

	ret = intel_wb_setup_transcoder(intel_wb, pipe_config,
					conn_state, job);

	if (ret < 0) {
		drm_dbg_kms(&i915->drm,
			    "WD transcoder setup not completed aborting capture\n");
		return -1;
	}

	if (!wb_crtc) {
		drm_err(&i915->drm, "CRTC not attached\n");
		return -1;
	}

	tmp = intel_de_read_fw(i915, WD_TRANS_FUNC_CTL(intel_wb->trans));
	tmp |= START_TRIGGER_FRAME;
	tmp &= ~WD_FRAME_NUMBER_MASK;
	tmp |= intel_wb->frame_num;
	drm_dbg_kms(&i915->drm, "Capture WD_TRANS_FUNC_CTL: 0x%8x, tmp: 0x%8x\n", WD_TRANS_FUNC_CTL(intel_wb->trans).reg, tmp);
	intel_de_write_fw(i915,	WD_TRANS_FUNC_CTL(intel_wb->trans), tmp);

	if (!intel_de_wait_for_set(i915, WD_IIR(intel_wb->trans),
				   WD_FRAME_COMPLETE_INT, 100)){
		drm_dbg_kms(&i915->drm, "frame captured\n");
		status = 0;
	} else {
		drm_dbg_kms(&i915->drm, "frame not captured triggering stop frame\n");
		tmp = intel_de_read(i915, WD_TRANS_FUNC_CTL(intel_wb->trans));
		tmp |= STOP_TRIGGER_FRAME;
		intel_de_write(i915, WD_TRANS_FUNC_CTL(intel_wb->trans), tmp);
		status = -1;
	}

	intel_wb_writeback_complete(intel_wb, job, status);
	if (wb_crtc->wb.e) {
		spin_lock_irqsave(&i915->drm.event_lock, flags);
		drm_dbg_kms(&i915->drm, "send %p\n", wb_crtc->wb.e);
		drm_crtc_send_vblank_event(&wb_crtc->base,
					   wb_crtc->wb.e);
		spin_unlock_irqrestore(&i915->drm.event_lock, flags);
		wb_crtc->wb.e = NULL;
	} else {
		drm_err(&i915->drm, "Event NULL! %p, %p\n", &i915->drm,
		wb_crtc);
	}
	if (!intel_get_writeback_job_from_queue(intel_wb))
		intel_wb_disable_capture(intel_wb);
	return 0;
}

void intel_wb_enable_capture(struct intel_crtc_state *pipe_config,
			     struct drm_connector_state *conn_state)
{
	struct drm_i915_private *i915 =
		to_i915(conn_state->connector->dev);
	struct drm_writeback_connector *wb_conn =
		drm_connector_to_writeback(conn_state->connector);
	struct intel_wb *intel_wb = wb_conn_to_intel_wb(wb_conn);
	struct drm_writeback_job *job;

	job = intel_get_writeback_job_from_queue(intel_wb);
	if (!job) {
		drm_dbg_kms(&i915->drm,
			    "job queue is empty not capturing any frame\n");
		return;
	}

	intel_wb_capture(intel_wb, pipe_config,
			 conn_state, job);
	intel_wb->frame_num += 1;
}

void intel_wb_set_vblank_event(struct intel_atomic_state *state,
			       struct intel_crtc *intel_crtc,
			       struct intel_crtc_state *intel_crtc_state)
{
	struct drm_i915_private *i915 = to_i915(intel_crtc->base.dev);
	struct drm_crtc_state *crtc_state = &intel_crtc_state->uapi;
	struct intel_encoder *encoder;
	struct intel_wb *intel_wb;
	struct drm_connector_state *old_conn_state;
	struct drm_connector_state *new_conn_state;
	struct drm_connector *connector;
	int i;

	for_each_intel_encoder(&i915->drm, encoder) {
		if (encoder->type != INTEL_OUTPUT_WB)
			continue;

		intel_wb = enc_to_intel_wb(encoder);
		if (!intel_wb->wb_crtc)
			return;
	}

	if (intel_wb && intel_crtc == intel_wb->wb_crtc) {
		for_each_oldnew_connector_in_state(&state->base, connector,
						   old_conn_state,
						   new_conn_state, i) {
			if (new_conn_state->writeback_job) {
				intel_crtc->wb.e = crtc_state->event;
				crtc_state->event = NULL;
			}
		}

		if (crtc_state->event) {
			crtc_state->no_vblank = true;
			drm_atomic_helper_fake_vblank(&state->base);
		}
	}
}

void intel_wb_handle_isr(struct drm_i915_private *i915)
{
	u32 iir_value = 0;
	struct intel_encoder *encoder;
	struct intel_wb *intel_wb;

	iir_value = intel_de_read(i915, WD_IIR(TRANSCODER_WD_0));

	for_each_intel_encoder(&i915->drm, encoder) {
		if (encoder->type != INTEL_OUTPUT_WB)
			continue;

		intel_wb = enc_to_intel_wb(encoder);
		if (!intel_wb->wb_crtc) {
			drm_err(&i915->drm, "NO CRTC attached with WD\n");
			goto clear_iir;
		}
	}

	if (iir_value & WD_FRAME_COMPLETE_INT)
		return;

clear_iir:
	intel_de_write(i915, WD_IIR(TRANSCODER_WD_0), iir_value);
}
