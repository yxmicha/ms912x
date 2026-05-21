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
 * usb_device.h -- Drm driver for MacroSilicon chip 913x and 912x
 */

#ifndef __MSDISP_MS9132_H__
#define __MSDISP_MS9132_H__

#include "hal_adaptor.h"

#define MSDISP_913X_VENDOR	0x345f
#define MSDISP_9132_PRODUCT	0x9132
#define MSDISP_9133_PRODUCT	0x9133
#define MSDISP_9135_PRODUCT	0x9135

extern const struct msdisp_hal_id ms9132_id;
extern const struct msdisp_hal_id ms9133_id;
extern const struct msdisp_hal_id ms9135_id;
extern struct msdisp_hal_dev ms9132_dev;
extern struct msdisp_hal_dev ms9133_dev;
extern struct msdisp_hal_dev ms9135_dev;

struct usb_device;
s32 ms9132_xdata_write_byte(struct usb_device *udev, u16 addr, u8 data);

#endif
