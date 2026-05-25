/* Copyright (C) 2023 MacroSilicon Technology Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * msdisp_drm_modeset.c -- Drm driver for MacroSilicon chip 913x and 912x
 */

#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#if KERNEL_VERSION(5, 16, 0) <= LINUX_VERSION_CODE
#include <drm/drm_vblank.h>
#include <drm/drm_damage_helper.h>
#elif KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_damage_helper.h>
#else
#include <drm/drmP.h>
#endif
#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_atomic_helper.h>
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
#include <drm/drm_gem_atomic_helper.h>
#else
#include <drm/drm_gem_framebuffer_helper.h>
#endif
#include <linux/dma-buf.h>

#include "msdisp_drm_drv.h"
#include "msdisp_drm_event.h"
#include "msdisp_common_util.h"
#include "msdisp_usb_interface.h"

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
#define CRTC_ATOMIC_STATE_T struct drm_atomic_state
#define CRTC_FLUSH_STATE_PARAM struct drm_atomic_state *state
#define CRTC_CHECK_STATE_PARAM struct drm_atomic_state *state
#define CRTC_ENABLE_STATE_PARAM struct drm_atomic_state *state
#define CRTC_DISABLE_STATE_PARAM struct drm_atomic_state *state
#else
#define CRTC_FLUSH_STATE_PARAM struct drm_crtc_state *old_state
#define CRTC_CHECK_STATE_PARAM struct drm_crtc_state *new_state
#define CRTC_ENABLE_STATE_PARAM struct drm_crtc_state *old_crtc_state
#define CRTC_DISABLE_STATE_PARAM struct drm_crtc_state *old_crtc_state
#endif

#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
#define PLANE_ATOMIC_STATE_PARAM struct drm_atomic_state *atom_state
#else
#define PLANE_ATOMIC_STATE_PARAM struct drm_plane_state *old_state
#endif

static struct msdisp_drm_pipeline *
get_pipeline_by_plane(struct drm_plane *plane)
{
	struct msdisp_drm_device *msdisp_drm = to_msdisp_drm(plane->dev);
	int i;

	for (i = 0; i < msdisp_drm->pipeline_cnt; i++) {
		struct drm_crtc *crtc = msdisp_drm->pipeline[i].crtc;

		if (crtc->primary == plane || crtc->cursor == plane)
			return &msdisp_drm->pipeline[i];
	}
	return NULL;
}

static struct msdisp_drm_pipeline *
get_pipeline_by_crtc(struct drm_crtc *crtc)
{
	struct msdisp_drm_device *msdisp_drm = to_msdisp_drm(crtc->dev);
	struct msdisp_drm_pipeline *pipeline = NULL;
	int i;

	for (i = 0; i < msdisp_drm->pipeline_cnt; i++) {
		if (msdisp_drm->pipeline[i].crtc == crtc) {
			pipeline = &msdisp_drm->pipeline[i];
			break;
		}
	}

	return pipeline;
}

void msdisp_crtc_update_event(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct msdisp_drm_pipeline *pipeline = get_pipeline_by_crtc(crtc);

	if (crtc->state->event) {
		unsigned long flags;

		crtc->state->event->pipe = drm_crtc_index(crtc);
		spin_lock_irqsave(&dev->event_lock, flags);
		pipeline->event = crtc->state->event;
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}

static void msdisp_drm_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
	kfree(crtc);
}

void msdisp_drm_crtc_atomic_flush(struct drm_crtc *crtc,
				  CRTC_FLUSH_STATE_PARAM)
{
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_old_crtc_state(state, crtc);
#else
	struct drm_crtc_state *crtc_state = old_state;
#endif

	if (crtc->state->active && crtc_state->active)
		msdisp_crtc_update_event(crtc);
}

int msdisp_drm_crtc_atomic_check(struct drm_crtc *crtc,
				 CRTC_CHECK_STATE_PARAM)
{
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
#else
	struct drm_crtc_state *crtc_state = new_state;
#endif
	bool has_primary;
	int ret;

	/*
	 * Add all currently-active planes to the commit first so that a
	 * cursor-only update (which arrives with only the cursor plane in
	 * plane_mask) also drags in the primary plane.  The has_primary check
	 * below must run after this to see the updated plane_mask.
	 */
	ret = drm_atomic_add_affected_planes(crtc_state->state, crtc);
	if (ret)
		return ret;

	has_primary = crtc_state->plane_mask & drm_plane_mask(crtc->primary);
	if (has_primary != crtc_state->enable)
		return -EINVAL;

	return 0;
}

void msdisp_drm_crtc_atomic_enable(struct drm_crtc *crtc,
				   CRTC_ENABLE_STATE_PARAM)
{
	struct drm_device *dev = crtc->dev;
	struct drm_plane *primary_plane = crtc->primary;
	struct drm_plane_state *plane_state = primary_plane->state;
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_crtc_state *crtc_state = crtc->state;
	struct drm_display_mode *mode = &crtc_state->mode;
	struct msdisp_drm_pipeline *pipeline;
	struct msdisp_usb_hal *usb_hal;
	int width, height, rate;

	drm_crtc_vblank_on(crtc);
	msdisp_crtc_update_event(crtc);

	dev_info(dev->dev, "enable: pid=%d comm=%s\n",
		 task_pid_nr(current), current->comm);
	dev_info(dev->dev, "fb size:width:%d height:%d mode size:width:%d height:%d\n",
		 fb->width, fb->height, mode->hdisplay, mode->vdisplay);

	pipeline = get_pipeline_by_crtc(crtc);
	mutex_lock(&pipeline->hal_lock);
	usb_hal = pipeline->usb_hal;
	if (!usb_hal) {
		dev_info(dev->dev, "usb hal is null\n");
		goto out;
	}

	width = mode->hdisplay;
	height = mode->vdisplay;
	rate = drm_mode_vrefresh(mode);

	pipeline->drm_width = width;
	pipeline->drm_height = height;
	pipeline->drm_rate = rate;
	pipeline->drm_fb_format = fb->format->format;
	pipeline->drm_status = MSDISP_DRM_STATUS_ENABLE;
	pipeline->needs_full_refresh = true;

	usb_hal->funcs->enable(usb_hal, width, height, rate, fb->format->format);
	dev_info(dev->dev, "enable event:format=0x%x width=%d height=%d rate=%d\n",
		 fb->format->format, width, height, rate);
out:
	mutex_unlock(&pipeline->hal_lock);
}

void msdisp_drm_crtc_atomic_disable(struct drm_crtc *crtc,
				    CRTC_DISABLE_STATE_PARAM)
{
	struct drm_device *dev = crtc->dev;
	struct msdisp_drm_pipeline *pipeline;
	struct msdisp_usb_hal *usb_hal;

	drm_crtc_vblank_off(crtc);
	if (crtc->state->event) {
		unsigned long flags;

		spin_lock_irqsave(&dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
	pipeline = get_pipeline_by_crtc(crtc);
	pipeline->drm_status = MSDISP_DRM_STATUS_DISABLE;
	dev_info(dev->dev, "disable: pid=%d! comm=%s\n",
		 task_pid_nr(current), current->comm);
	mutex_lock(&pipeline->hal_lock);
	usb_hal = pipeline->usb_hal;

	if (!usb_hal) {
		dev_info(dev->dev, "usb hal is null\n");
		goto out;
	}

	usb_hal->funcs->disable(usb_hal);
out:
	mutex_unlock(&pipeline->hal_lock);
}

static struct drm_crtc_helper_funcs msdisp_drm_helper_funcs = {
	.atomic_check = msdisp_drm_crtc_atomic_check,
	.atomic_enable = msdisp_drm_crtc_atomic_enable,
	.atomic_disable = msdisp_drm_crtc_atomic_disable,
	.atomic_flush = msdisp_drm_crtc_atomic_flush,
};

#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
static int msdisp_drm_enable_vblank(struct drm_crtc *crtc)
{
	return 0;
}

static void msdisp_drm_disable_vblank(struct drm_crtc *crtc)
{
}
#endif

static const struct drm_crtc_funcs msdisp_drm_crtc_funcs = {
	.reset                  = drm_atomic_helper_crtc_reset,
	.destroy                = msdisp_drm_crtc_destroy,
	.set_config             = drm_atomic_helper_set_config,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state   = drm_atomic_helper_crtc_destroy_state,
#if KERNEL_VERSION(5, 11, 0) <= LINUX_VERSION_CODE || defined(RPI) || defined(EL8)
	.enable_vblank          = msdisp_drm_enable_vblank,
	.disable_vblank         = msdisp_drm_disable_vblank,
#endif
};

static int msdisp_drm_handle_damage(struct msdisp_drm_framebuffer *efb,
				    struct msdisp_drm_pipeline *pipeline,
				    struct drm_rect *damage, u8 *src)
{
	struct drm_framebuffer *fb = &efb->base;
	struct msdisp_usb_hal *usb_hal = pipeline->usb_hal;

	return usb_hal->funcs->send_xrgb8888_rect(usb_hal, src, fb->width,
						  fb->height, fb->pitches[0],
						  damage->x1, damage->y1,
						  damage->x2, damage->y2);
}

static void msdisp_drm_plane_atomic_update(struct drm_plane *plane,
					   PLANE_ATOMIC_STATE_PARAM)
{
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(atom_state, plane);
#endif
	struct drm_device *dev;
	struct msdisp_drm_device *msdisp_drm;
	struct msdisp_usb_hal *usb_hal;
	struct drm_framebuffer *fb;
	struct msdisp_drm_framebuffer *efb;
	struct drm_plane_state *state;
	struct drm_rect damage;
	struct msdisp_drm_frame_stat *stat;
	struct msdisp_drm_pipeline *pipeline;
	struct drm_shadow_plane_state *shadow_state = NULL;
	u8 *src = NULL;
	int lock_flag = 0;

	if (!plane || !plane->state || !plane->dev)
		return;

	dev = plane->dev;
	msdisp_drm = to_msdisp_drm(dev);

	pipeline = get_pipeline_by_plane(plane);
	stat = &pipeline->frame_stat;
	stat->total++;
	state = plane->state;

	if (pipeline->needs_full_refresh) {
		stat->no_old_state++;
		stat->full_needs_refresh++;
		damage = (struct drm_rect){
			.x1 = 0,
			.y1 = 0,
			.x2 = state->fb ? state->fb->width : 0,
			.y2 = state->fb ? state->fb->height : 0,
		};
	} else if (!old_state) {
		stat->no_old_state++;
		stat->full_no_old_state++;
		damage = (struct drm_rect){
			.x1 = 0,
			.y1 = 0,
			.x2 = state->fb ? state->fb->width : 0,
			.y2 = state->fb ? state->fb->height : 0,
		};
	} else if (!old_state->fb) {
		stat->no_old_state++;
		stat->full_no_old_fb++;
		damage = (struct drm_rect){
			.x1 = 0,
			.y1 = 0,
			.x2 = state->fb ? state->fb->width : 0,
			.y2 = state->fb ? state->fb->height : 0,
		};
	} else if (!drm_atomic_helper_damage_merged(old_state, state, &damage)) {
		stat->full_no_damage++;
		damage = (struct drm_rect){
			.x1 = 0,
			.y1 = 0,
			.x2 = state->fb ? state->fb->width : 0,
			.y2 = state->fb ? state->fb->height : 0,
		};
	}

	fb = state->fb;
	if (!fb) {
		stat->no_fb++;
		return;
	}

	efb = to_msdisp_drm_fb(fb);
	shadow_state = to_drm_shadow_plane_state(state);
	if (shadow_state && !iosys_map_is_null(&shadow_state->data[0]))
		src = shadow_state->data[0].vaddr;

	drm_framebuffer_get(&efb->base);

	if (!src) {
		stat->vmap_null++;
		goto err_fb;
	}

	if (atomic_read(&pipeline->dump_fb_flag)) {
		msdisp_common_save_buf_to_bmp(src, fb->width, fb->height,
					      fb->format->cpp[0], NULL,
					      pipeline->dump_fb_filename);
		atomic_set(&pipeline->dump_fb_flag, 0);
		dev_info(msdisp_drm->drm.dev,
			 "msdisp finished save raw fb data to file:%s\n",
			 pipeline->dump_fb_filename);
	}

	mutex_lock(&pipeline->hal_lock);
	lock_flag = 1;
	usb_hal = pipeline->usb_hal;
	if (!pipeline->usb_hal) {
		stat->no_usb_hal++;
		goto end_cpu_access;
	}

	if (msdisp_drm_handle_damage(efb, pipeline, &damage, src))
		stat->handle_fail++;
	else
		pipeline->needs_full_refresh = false;

end_cpu_access:
err_fb:
	drm_framebuffer_put(&efb->base);

	if (lock_flag)
		mutex_unlock(&pipeline->hal_lock);
}

static const struct drm_plane_helper_funcs msdisp_drm_plane_helper_funcs = {
	.atomic_update = msdisp_drm_plane_atomic_update,
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS
#else
	.prepare_fb = drm_gem_fb_prepare_fb
#endif
};

#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
static void msdisp_drm_cursor_plane_atomic_update(struct drm_plane *plane,
						  PLANE_ATOMIC_STATE_PARAM)
{
	struct msdisp_drm_pipeline *pipeline = get_pipeline_by_plane(plane);
	struct drm_plane_state *state = plane->state;
	struct msdisp_usb_hal *usb_hal;
	struct drm_shadow_plane_state *shadow_state;

	if (!pipeline)
		return;

	mutex_lock(&pipeline->hal_lock);
	usb_hal = pipeline->usb_hal;
	if (!usb_hal)
		goto out;

	if (state->fb) {
		shadow_state = to_drm_shadow_plane_state(state);
		if (!iosys_map_is_null(&shadow_state->data[0]))
			usb_hal->funcs->cursor_set(usb_hal,
						   (u8 *)shadow_state->data[0].vaddr);
		usb_hal->funcs->cursor_move(usb_hal, state->crtc_x, state->crtc_y);
	} else {
		usb_hal->funcs->cursor_set(usb_hal, NULL);
	}

out:
	mutex_unlock(&pipeline->hal_lock);
}

static const struct drm_plane_helper_funcs msdisp_drm_cursor_helper_funcs = {
	.atomic_update = msdisp_drm_cursor_plane_atomic_update,
	.prepare_fb    = drm_gem_plane_helper_prepare_fb,
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS
};
#endif

static const struct drm_plane_funcs msdisp_drm_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
	DRM_GEM_SHADOW_PLANE_FUNCS,
#else
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
#endif
};

static const u32 formats[] = {
	DRM_FORMAT_XRGB8888,
};

#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
static const u32 cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};
#endif

/*
 * Declare DRM_FORMAT_MOD_LINEAR as the only supported modifier so that
 * drmModeAddFB2WithModifiers (used by GBM for cursor buffers) finds a
 * matching plane and does not reject the FB with -EINVAL.  The sentinel
 * DRM_FORMAT_MOD_INVALID terminates the list as required by
 * drm_universal_plane_init().
 */
static const u64 plane_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static struct drm_plane *msdisp_drm_create_plane(struct drm_device *dev,
						 enum drm_plane_type type,
						 const u32 *fmts, int num_fmts,
						 const struct drm_plane_helper_funcs *helper_funcs)
{
	struct drm_plane *plane;
	int ret;
	char *plane_type = (type == DRM_PLANE_TYPE_CURSOR) ? "cursor" : "primary";

	plane = kzalloc(sizeof(*plane), GFP_KERNEL);
	if (!plane)
		return NULL;
	plane->format_default = true;

	ret = drm_universal_plane_init(dev, plane, 0xFF,
				       &msdisp_drm_plane_funcs,
				       fmts, num_fmts,
				       plane_modifiers, type, plane_type);
	if (ret) {
		dev_err(dev->dev, "Failed to initialize %s plane\n", plane_type);
		kfree(plane);
		return NULL;
	}

	drm_plane_helper_add(plane, helper_funcs);

	return plane;
}

static struct drm_crtc *msdisp_drm_crtc_init(struct drm_device *dev)
{
	struct drm_crtc *crtc;
	struct drm_plane *primary_plane;
	struct drm_plane *cursor_plane = NULL;

	crtc = kzalloc(sizeof(*crtc), GFP_KERNEL);
	if (!crtc)
		return NULL;

	primary_plane = msdisp_drm_create_plane(dev, DRM_PLANE_TYPE_PRIMARY,
						formats, ARRAY_SIZE(formats),
						&msdisp_drm_plane_helper_funcs);
	if (!primary_plane) {
		kfree(crtc);
		return NULL;
	}

#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
	cursor_plane = msdisp_drm_create_plane(dev, DRM_PLANE_TYPE_CURSOR,
					       cursor_formats,
					       ARRAY_SIZE(cursor_formats),
					       &msdisp_drm_cursor_helper_funcs);
	if (!cursor_plane) {
		kfree(crtc);
		return NULL;
	}
#endif

#if KERNEL_VERSION(5, 0, 0) <= LINUX_VERSION_CODE || defined(EL8)
	drm_plane_enable_fb_damage_clips(primary_plane);
#endif

	drm_crtc_init_with_planes(dev, crtc, primary_plane, cursor_plane,
				  &msdisp_drm_crtc_funcs, NULL);
	drm_crtc_helper_add(crtc, &msdisp_drm_helper_funcs);

	return crtc;
}

static const struct drm_mode_config_funcs msdisp_drm_mode_funcs = {
	.fb_create = msdisp_drm_fb_user_fb_create,
	.atomic_commit = drm_atomic_helper_commit,
	.atomic_check = drm_atomic_helper_check
};

int msdisp_drm_modeset_init(struct drm_device *dev)
{
	struct msdisp_drm_device *msdisp_drm = to_msdisp_drm(dev);
	struct drm_crtc *crtc;
	struct msdisp_drm_connector *connector;
	struct drm_encoder *encoder;
	int i, pipeline_cnt;

	drm_mode_config_init(dev);

	dev->mode_config.min_width = 1;
	dev->mode_config.min_height = 1;
	dev->mode_config.max_width = 1920;
	dev->mode_config.max_height = 1600;
	dev->mode_config.prefer_shadow = 1;
	dev->mode_config.preferred_depth = 32;
#if KERNEL_VERSION(5, 13, 0) <= LINUX_VERSION_CODE
	dev->mode_config.cursor_width = 64;
	dev->mode_config.cursor_height = 64;
#endif
	dev->mode_config.funcs = &msdisp_drm_mode_funcs;

	pipeline_cnt = msdisp_drm_get_pipeline_init_count();
	for (i = 0; i < pipeline_cnt; i++) {
		crtc = msdisp_drm_crtc_init(dev);
		if (!crtc) {
			dev_err(dev->dev, "Failed to init crtc%d\n", i);
			goto err;
		}

		encoder = msdisp_drm_encoder_init(dev);
		if (!encoder) {
			dev_err(dev->dev, "Failed to init encoder%d\n", i);
			goto err;
		}
		encoder->possible_crtcs = (1 << i);

		connector = msdisp_drm_connector_init(dev, encoder, i);
		if (!connector) {
			dev_err(dev->dev, "Failed to init connector%d\n", i);
			goto err;
		}

		msdisp_drm->pipeline[i].crtc = crtc;
		msdisp_drm->pipeline[i].encoder = encoder;
		msdisp_drm->pipeline[i].connector = connector;

		continue;
err:
		kfree(crtc);
		kfree(encoder);
		kfree(connector);
		return -ENOMEM;
	}
	drm_mode_config_reset(dev);

	return 0;
}
