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
 * usb_hal_dev.h -- Drm driver for MacroSilicon chip 913x and 912x
 */

#ifndef __USB_HAL_DEV_H__
#define __USB_HAL_DEV_H__

#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/scatterlist.h>
#include <linux/usb.h>

#include "usb_hal_interface.h"

#define USB_HAL_TRANS_MODE_FRAME		0
#define USB_HAL_TRANS_MODE_MANUAL_BLOCK		3

#define USB_HAL_COLOR_FORMAT_RGB565		0
#define USB_HAL_COLOR_FORMAT_RGB888		1
#define USB_HAL_COLOR_FORMAT_YUV422		2
#define USB_HAL_COLOR_FORMAT_YUV444		3

#define USB_HAL_DEV_STATE_UNKNOWN		0
#define USB_HAL_DEV_STATE_ENABLED		1
#define USB_HAL_DEV_STATE_DISABLED		2

#define MS9132_USB_BUS_STATUS_NORMAL		0
#define MS9132_USB_BUS_STATUS_SUSPEND		1

#define USB_HAL_BUF_SIZE			(8 * 1024 * 1024)

#define USB_HAL_CURSOR_WIDTH			64
#define USB_HAL_CURSOR_HEIGHT			64
#define USB_HAL_CURSOR_BUF_SIZE			(USB_HAL_CURSOR_WIDTH * USB_HAL_CURSOR_HEIGHT * 4)

#define USB_HAL_MAX_CUSTOM_MODE			16

struct sg_table;
struct usb_device;
struct kfifo;

struct msdisp_hal_dev;
struct usb_hal_dev;

struct usb_hal_frame_request {
	void *transfer_buffer;
	struct usb_hal_dev *usb_dev;
	size_t transfer_len;
	size_t alloc_len;
	struct sg_table transfer_sgt;
	struct usb_sg_request sgr;
	struct work_struct work;
	struct timer_list timer;
	struct completion done;
	bool in_flight;
	bool first_frame;
};

struct usb_hal_cursor_buffer {
	bool valid;
	u8 *buf;
	int x;
	int y;
	int x1;
	int y1;
	int x2;
	int y2;
	struct mutex mutex; /* guards cursor state and buf */
};

struct usb_hal_dev_frame_stat {
	u64 send_total;
	u64 send_success;
	u64 update_sequence;
	u64 dropped_stale;
	u64 state_error;
	u64 send_fail;
	u64 pack_last_ms;
	u64 pack_max_ms;
	u64 usb_last_ms;
	u64 usb_max_ms;
	u64 busy_drop;
	u64 first_frame_success;
	u64 keepalive_send;
};

struct usb_hal_dev {
	struct usb_hal *hal;
	const struct msdisp_hal_dev *hal_dev;
	struct usb_device *udev;
	struct kfifo *fifo;
	struct usb_hal_video_mode mode;
	struct device *dma_dev;
	atomic_t thread_run_flag;
	struct semaphore sema;
	struct task_struct *thread;
	int index;
	u8 vpack_in;
	u8 vpack_out;
	u8 color_out;
	u8 trans_mode;
	struct usb_hal_cursor_buffer cursor_buf;
	struct usb_hal_dev_frame_stat stat;
	int state;
	int bus_status;
	int first_buf_send;
	int current_request;
	unsigned long last_send_jiffies;
	struct usb_hal_frame_request requests[2];
	struct mutex sender_lock; /* guards current_request and requests[] */
	struct usb_hal_video_mode custom_mode[USB_HAL_MAX_CUSTOM_MODE];
	int custom_mode_cnt;
};

#endif
