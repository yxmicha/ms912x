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
 * msdisp_drm_mode.h -- Drm driver for MacroSilicon chip 913x and 912x
 */

#ifndef __MSDISP_DRM_MODE_H__
#define __MSDISP_DRM_MODE_H__

struct drm_display_mode;
struct drm_device;

struct drm_display_mode *msdisp_mode_from_cea_vic(struct drm_device *dev,
						  unsigned char video_code);

#endif
