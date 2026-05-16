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
 * msdisp_drm_drv.h -- Drm driver for MacroSilicon chip 913x and 912x
 */

#ifndef __MSDISP_DRM_DRV_H__
#define __MSDISP_DRM_DRV_H__

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#ifndef from_timer
#define from_timer(var, callback_timer, timer_field)	\
	timer_container_of(var, callback_timer, timer_field)
#endif
#ifndef del_timer
#define del_timer(timer) timer_delete(timer)
#endif
#endif
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vblank.h>
#else
#include <drm/drmP.h>
#endif
#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
#include <drm/drm_legacy.h>
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#include <drm/drm_irq.h>
#endif
#include <drm/drm_framebuffer.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_encoder.h>
#include <drm/drm_rect.h>
#include <drm/drm_gem.h>
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE || defined(EL8)
#include <linux/dma-resv.h>
#else
#include <linux/reservation.h>
#endif

#define MSDISP_DRM_STATUS_DISABLE		0
#define MSDISP_DRM_STATUS_ENABLE		1

#define MSDISP_DRM_MAX_PIPELINE_CNT		3

struct edid;
struct msdisp_usb_hal;
struct drm_mode_create_dumb;
struct msdisp_drm_connector;
struct drm_pending_vblank_event;

struct msdisp_drm_gem_object {
	struct drm_gem_object base;
	struct page **pages;
	unsigned int pages_pin_count;
	struct mutex pages_lock; /* guards pages and pages_pin_count */
	void *vmapping;
	struct sg_table *sg;
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE || defined(EL8)
	struct dma_resv *resv;
	struct dma_resv _resv;
#else
	struct reservation_object *resv;
	struct reservation_object _resv;
#endif
};

#define to_msdisp_drm_bo(x) container_of(x, struct msdisp_drm_gem_object, base)

struct msdisp_drm_framebuffer {
	struct drm_framebuffer base;
	struct msdisp_drm_gem_object *obj;
	bool active;
};

#define to_msdisp_drm_fb(x) container_of(x, struct msdisp_drm_framebuffer, base)

struct msdisp_drm_frame_stat {
	u64 total;
	u64 no_usb_hal;
	u64 no_old_state;
	u64 no_fb;
	u64 vmap_fail;
	u64 vmap_null;
	u64 cpu_access_fail;
	u64 acquire_buf_fail;
	u64 handle_fail;
	u64 full_needs_refresh;
	u64 full_no_old_state;
	u64 full_no_old_fb;
	u64 full_no_damage;
};

struct msdisp_drm_pipeline {
	struct device dev;
	int dev_init;
	struct drm_crtc *crtc;
	struct drm_encoder *encoder;
	struct msdisp_drm_connector *connector;
	struct msdisp_usb_hal *usb_hal;
	struct drm_pending_vblank_event *event;
	struct mutex hal_lock; /* guards usb_hal pointer */
	struct kfifo fifo;
	struct msdisp_drm_frame_stat frame_stat;
	atomic_t dump_fb_flag;
	int reg_flag;
	int drm_status;
	int drm_width;
	int drm_height;
	int drm_rate;
	int drm_fb_format;
	bool needs_full_refresh;
	char dump_fb_filename[256];
};

/* must be first field so drmm_add_final_kfree is not needed */
struct msdisp_drm_device {
	struct drm_device drm;
	struct timer_list vblank_timer;
	struct device *parent;
	int pipeline_cnt;
	struct msdisp_drm_pipeline pipeline[MSDISP_DRM_MAX_PIPELINE_CNT];
};

#define to_msdisp_drm(x) container_of(x, struct msdisp_drm_device, drm)

void msdisp_platform_device_remove(struct platform_device *pdev);

struct drm_framebuffer *
msdisp_drm_fb_user_fb_create(struct drm_device *dev,
			     struct drm_file *file,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
			     const struct drm_format_info *info,
#endif
			     const struct drm_mode_fb_cmd2 *mode_cmd);

int msdisp_drm_dumb_create(struct drm_file *file_priv,
			   struct drm_device *dev,
			   struct drm_mode_create_dumb *args);
int msdisp_drm_gem_mmap_offset(struct drm_file *file_priv,
			       struct drm_device *dev, u32 handle,
			       u64 *offset);

void msdisp_drm_gem_free_object(struct drm_gem_object *gem_obj);
struct msdisp_drm_gem_object *msdisp_drm_gem_alloc_object(struct drm_device *dev,
							  size_t size);
u32 msdisp_drm_gem_object_handle_lookup(struct drm_file *filp,
					struct drm_gem_object *obj);

int msdisp_drm_gem_vmap(struct msdisp_drm_gem_object *obj);
void msdisp_drm_gem_vunmap(struct msdisp_drm_gem_object *obj);
int msdisp_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma);

#if KERNEL_VERSION(4, 17, 0) <= LINUX_VERSION_CODE
vm_fault_t msdisp_drm_gem_fault(struct vm_fault *vmf);
#else
int msdisp_drm_gem_fault(struct vm_fault *vmf);
#endif

int msdisp_drm_modeset_init(struct drm_device *dev);
struct drm_encoder *msdisp_drm_encoder_init(struct drm_device *dev);
struct msdisp_drm_connector *
msdisp_drm_connector_init(struct drm_device *dev, struct drm_encoder *encoder,
			  int index);
struct drm_device *msdisp_drm_device_create(struct device *parent);
int msdisp_drm_device_remove(struct drm_device *dev);
int msdisp_drm_get_pipeline_init_count(void);

void msdisp_drm_sysfs_init(struct msdisp_drm_device *msdisp_drm);
void msdisp_drm_sysfs_exit(struct msdisp_drm_device *msdisp_drm);

#endif
