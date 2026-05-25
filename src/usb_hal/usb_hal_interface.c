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
 * usb_hal_interface.c -- Drm driver for MacroSilicon chip 913x and 912x
 */

#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/timer.h>

#include <linux/mm_types.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/completion.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <linux/kfifo.h>

#include <drm/drm_fourcc.h>

#include "usb_hal_edid.h"
#include "usb_hal_chip.h"
#include "usb_hal_interface.h"
#include "usb_hal_event.h"
#include "usb_hal_dev.h"
#include "usb_hal_thread.h"
#include "hal_adaptor.h"
#include "usb_device_hid.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#ifndef from_timer
#define from_timer(var, callback_timer, timer_field) \
	timer_container_of(var, callback_timer, timer_field)
#endif
#ifndef del_timer_sync
#define del_timer_sync(timer) timer_delete_sync(timer)
#endif
#endif

#define USB_HAL_COLOR_FORMAT_RGB		0
#define USB_HAL_COLOR_FORMAT_YUV		1
#define USB_HAL_FRAME_TIMEOUT_MS		5000

#define MS913X_CUSTOM_TIMING_ADDR		0xfc50
#define MS912X_CUSTOM_TIMING_ADDR		0x1c00

struct fourcc_format_desc {
	u32 fourcc;
	u8 color_fmt;
	u8 bpp;
	u8 vpack_in;
};

struct usb_hal_frame_update_header {
	__be16 header;
	u8 x_high;
	u8 x_low_y_high;
	u8 y_low;
	u8 width_high;
	u8 width_low_height_high;
	u8 height_low;
} __packed;

static const u8 usb_hal_end_of_buffer[8] = { 0xff, 0xc0, 0x00, 0x00,
					      0x00, 0x00, 0x00, 0x00 };

static void usb_hal_frame_header_set(struct usb_hal_frame_update_header *header,
				     int x, int y, int width, int height)
{
	header->header = cpu_to_be16(0xff00);
	header->x_high = (x >> 4) & 0xff;
	header->x_low_y_high = ((x & 0x0f) << 4) | ((y >> 8) & 0x0f);
	header->y_low = y & 0xff;
	header->width_high = (width >> 4) & 0xff;
	header->width_low_height_high = ((width & 0x0f) << 4) | ((height >> 8) & 0x0f);
	header->height_low = height & 0xff;
}

static u8 usb_hal_range(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

static void usb_hal_frame_request_timeout(struct timer_list *timer)
{
	struct usb_hal_frame_request *request = from_timer(request, timer, timer);

	usb_sg_cancel(&request->sgr);
}

static void usb_hal_enable_screen_after_first_frame(struct usb_hal_frame_request *request)
{
	struct usb_hal_dev *usb_dev = request->usb_dev;
	struct usb_hal *hal = usb_dev->hal;
	int ret;

	if (!request->first_frame)
		return;

	mutex_lock(&usb_dev->sender_lock);
	if (usb_dev->state != USB_HAL_DEV_STATE_ENABLED || usb_dev->first_buf_send) {
		mutex_unlock(&usb_dev->sender_lock);
		return;
	}

	ret = usb_dev->hal_dev->funcs->set_video_enable(usb_dev->udev, 1);
	if (ret) {
		dev_err(&usb_dev->udev->dev, "start video failed! rtn = %d\n", ret);
		mutex_unlock(&usb_dev->sender_lock);
		return;
	}

	ret = usb_dev->hal_dev->funcs->set_screen_enable(usb_dev->udev, 1, hal->chip_id,
							  hal->port_type, hal->sdram_type);
	if (ret) {
		dev_err(&usb_dev->udev->dev, "start screen failed! rtn = %d\n", ret);
		mutex_unlock(&usb_dev->sender_lock);
		return;
	}

	usb_dev->first_buf_send = 1;
	usb_dev->stat.first_frame_success++;
	dev_info(&usb_dev->udev->dev, "start video success!\n");
	mutex_unlock(&usb_dev->sender_lock);
}

static void usb_hal_frame_request_work(struct work_struct *work)
{
	struct usb_hal_frame_request *request =
		container_of(work, struct usb_hal_frame_request, work);
	struct usb_hal_dev *usb_dev = request->usb_dev;
	struct usb_sg_request *sgr = &request->sgr;
	ktime_t usb_start;
	s64 usb_elapsed_ms;
	int ret;

	timer_setup(&request->timer, usb_hal_frame_request_timeout, 0);
	ret = usb_sg_init(sgr, usb_dev->udev,
			  usb_sndbulkpipe(usb_dev->udev,
					  usb_dev->hal_dev->funcs->get_transfer_bulk_ep()),
			  0,
			  request->transfer_sgt.sgl, request->transfer_sgt.nents,
			  request->transfer_len, GFP_KERNEL);
	if (ret) {
		dev_warn(&usb_dev->udev->dev, "usb_sg_init failed ret=%d len=%zu\n",
			 ret, request->transfer_len);
		usb_dev->stat.send_fail++;
		complete(&request->done);
		return;
	}

	WRITE_ONCE(request->in_flight, true);
	mod_timer(&request->timer, jiffies + msecs_to_jiffies(USB_HAL_FRAME_TIMEOUT_MS));
	usb_start = ktime_get();
	usb_sg_wait(sgr);
	usb_elapsed_ms = ktime_to_ms(ktime_sub(ktime_get(), usb_start));
	WRITE_ONCE(request->in_flight, false);
	del_timer_sync(&request->timer);

	usb_dev->stat.usb_last_ms = usb_elapsed_ms;
	if (usb_elapsed_ms > usb_dev->stat.usb_max_ms)
		usb_dev->stat.usb_max_ms = usb_elapsed_ms;

	if (sgr->status) {
		usb_dev->stat.send_fail++;
		dev_warn(&usb_dev->udev->dev, "bulk transfer failed status=%d len=%zu\n",
			 sgr->status, request->transfer_len);
	} else {
		usb_dev->stat.send_success++;
		WRITE_ONCE(usb_dev->last_send_jiffies, jiffies);
		usb_hal_enable_screen_after_first_frame(request);
	}

	complete(&request->done);
}

static void usb_hal_free_frame_request(struct usb_hal_frame_request *request)
{
	if (!request->transfer_buffer)
		return;

	if (READ_ONCE(request->in_flight))
		usb_sg_cancel(&request->sgr);
	flush_work(&request->work);
	sg_free_table(&request->transfer_sgt);
	vfree(request->transfer_buffer);
	request->transfer_buffer = NULL;
	request->alloc_len = 0;
}

static int usb_hal_init_frame_request(struct usb_hal_dev *usb_dev,
				      struct usb_hal_frame_request *request,
				      size_t len)
{
	int ret, i;
	unsigned int num_pages;
	void *data;
	void *ptr;
	struct page **pages;

	data = vmalloc_32(len);
	if (!data)
		return -ENOMEM;

	num_pages = DIV_ROUND_UP(len, PAGE_SIZE);
	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_vfree;
	}

	for (i = 0, ptr = data; i < num_pages; i++, ptr += PAGE_SIZE)
		pages[i] = vmalloc_to_page(ptr);

	ret = sg_alloc_table_from_pages(&request->transfer_sgt, pages, num_pages, 0, len,
					GFP_KERNEL);
	kfree(pages);
	if (ret)
		goto err_vfree;

	request->transfer_buffer = data;
	request->alloc_len = len;
	request->usb_dev = usb_dev;
	init_completion(&request->done);
	complete(&request->done);
	INIT_WORK(&request->work, usb_hal_frame_request_work);
	return 0;

err_vfree:
	vfree(data);
	return ret;
}

static int usb_hal_pack_xrgb8888_rect(struct usb_hal_frame_request *request,
				      const u8 *buf, int pitch,
				      int x1, int y1, int x2, int y2)
{
	struct usb_hal_dev *usb_dev = request->usb_dev;
	struct usb_hal_cursor_buffer *cursor = &usb_dev->cursor_buf;
	struct usb_hal_frame_update_header *header = request->transfer_buffer;
	u8 *dst = request->transfer_buffer + sizeof(*header);
	int row, col;
	int width = x2 - x1;
	int height = y2 - y1;
	size_t transfer_len = width * height * 2 + sizeof(*header) + sizeof(usb_hal_end_of_buffer);
	bool cursor_valid;
	int cur_x, cur_y, cur_x1, cur_y1, cur_x2, cur_y2;

	if (transfer_len > request->alloc_len)
		return -EINVAL;

	usb_hal_frame_header_set(header, x1, y1, width, height);

	mutex_lock(&cursor->mutex);
	cursor_valid = cursor->valid;
	cur_x  = cursor->x;
	cur_y  = cursor->y;
	cur_x1 = cursor->x1;
	cur_y1 = cursor->y1;
	cur_x2 = cursor->x2;
	cur_y2 = cursor->y2;
	mutex_unlock(&cursor->mutex);

	for (row = y1; row < y2; row++) {
		const u8 *src = buf + row * pitch + x1 * 4;
		bool in_cursor_row;
		int cy = 0;

		if (cursor_valid) {
			cy = row - cur_y;
			in_cursor_row = cy >= cur_y1 && cy < cur_y2;
		} else {
			in_cursor_row = false;
		}

		for (col = 0; col < width; col += 2) {
			int b1 = src[0];
			int g1 = src[1];
			int r1 = src[2];
			int b2 = src[4];
			int g2 = src[5];
			int r2 = src[6];

			if (in_cursor_row) {
				int cx1 = (x1 + col) - cur_x;
				int cx2 = cx1 + 1;

				if (cx1 >= cur_x1 && cx1 < cur_x2) {
					const u8 *cp = cursor->buf +
						(cy * USB_HAL_CURSOR_WIDTH + cx1) * 4;
					int a = cp[3];

					b1 = ((a * cp[0]) + ((255 - a) * b1)) >> 8;
					g1 = ((a * cp[1]) + ((255 - a) * g1)) >> 8;
					r1 = ((a * cp[2]) + ((255 - a) * r1)) >> 8;
				}
				if (cx2 >= cur_x1 && cx2 < cur_x2) {
					const u8 *cp = cursor->buf +
						(cy * USB_HAL_CURSOR_WIDTH + cx2) * 4;
					int a = cp[3];

					b2 = ((a * cp[0]) + ((255 - a) * b2)) >> 8;
					g2 = ((a * cp[1]) + ((255 - a) * g2)) >> 8;
					r2 = ((a * cp[2]) + ((255 - a) * r2)) >> 8;
				}
			}

			int y0 = ((263 * r1 + 516 * g1 + 97 * b1) >> 10) + 16;
			int u0 = ((-152 * r1 - 298 * g1 + 450 * b1) >> 10) + 128;
			int v0 = ((450 * r1 - 377 * g1 - 73 * b1) >> 10) + 128;
			int y1_value = ((263 * r2 + 516 * g2 + 97 * b2) >> 10) + 16;

			u0 += ((-152 * r2 - 298 * g2 + 450 * b2) >> 10) + 128;
			v0 += ((450 * r2 - 377 * g2 - 73 * b2) >> 10) + 128;
			u0 >>= 1;
			v0 >>= 1;

			*dst++ = usb_hal_range(u0);
			*dst++ = usb_hal_range(y0);
			*dst++ = usb_hal_range(v0);
			*dst++ = usb_hal_range(y1_value);
			src += 8;
		}
	}

	memcpy(dst, usb_hal_end_of_buffer, sizeof(usb_hal_end_of_buffer));
	request->transfer_len = transfer_len;
	return 0;
}

int usb_hal_send_xrgb8888_rect(struct usb_hal *hal, const u8 *buf,
			       int width, int height, int pitch,
			       int x1, int y1, int x2, int y2)
{
	struct usb_hal_dev *usb_dev;
	struct usb_hal_frame_request *request = NULL;
	ktime_t pack_start_time;
	ktime_t pack_end_time;
	s64 pack_elapsed_ms;
	bool first_frame;
	int i;
	int ret;

	if (!hal || !buf)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;
	if (usb_dev->state != USB_HAL_DEV_STATE_ENABLED) {
		usb_dev->stat.state_error++;
		return -EAGAIN;
	}

	if (width != usb_dev->mode.width || height != usb_dev->mode.height || pitch < width * 4)
		return -EINVAL;

	x1 = max(0, x1) & 0xFFC;
	y1 = max(0, y1) & 0xFFE;
	x2 = min(width, (x2 + 3) & 0xFFC);
	y2 = min(height, (y2 + 1) & 0xFFE);
	if (x1 >= x2 || y1 >= y2 || (x2 - x1) % 2)
		return 0;

	mutex_lock(&usb_dev->sender_lock);
	first_frame = !usb_dev->first_buf_send;
	for (i = 0; i < 2; i++) {
		int index = (usb_dev->current_request + i) % 2;

		if (try_wait_for_completion(&usb_dev->requests[index].done)) {
			request = &usb_dev->requests[index];
			usb_dev->current_request = 1 - index;
			break;
		}
	}

	if (!request) {
		usb_dev->stat.busy_drop++;
		usb_dev->stat.dropped_stale++;
		mutex_unlock(&usb_dev->sender_lock);
		return first_frame ? -EBUSY : 0;
	}
	request->first_frame = first_frame;
	mutex_unlock(&usb_dev->sender_lock);

	pack_start_time = ktime_get();
	ret = usb_hal_pack_xrgb8888_rect(request, buf, pitch, x1, y1, x2, y2);
	pack_end_time = ktime_get();
	pack_elapsed_ms = ktime_to_ms(ktime_sub(pack_end_time, pack_start_time));
	usb_dev->stat.pack_last_ms = pack_elapsed_ms;
	if (pack_elapsed_ms > usb_dev->stat.pack_max_ms)
		usb_dev->stat.pack_max_ms = pack_elapsed_ms;

	if (ret) {
		complete(&request->done);
		return ret;
	}

	usb_dev->stat.update_sequence++;
	usb_dev->stat.send_total++;
	queue_work(system_long_wq, &request->work);
	return 0;
}

static void usb_hal_pack_keepalive_block(struct usb_hal_frame_request *request)
{
	struct usb_hal_frame_update_header *header = request->transfer_buffer;
	u8 *dst = request->transfer_buffer + sizeof(*header);
	int i;

	usb_hal_frame_header_set(header, 0, 0, 16, 2);

	for (i = 0; i < 16 * 2 / 2; i++) {
		*dst++ = 128;
		*dst++ = 16;
		*dst++ = 128;
		*dst++ = 16;
	}

	memcpy(dst, usb_hal_end_of_buffer, sizeof(usb_hal_end_of_buffer));
	request->transfer_len = sizeof(*header) + 16 * 2 * 2 + sizeof(usb_hal_end_of_buffer);
}

int usb_hal_send_keepalive(struct usb_hal *hal)
{
	struct usb_hal_dev *usb_dev;
	struct usb_hal_frame_request *request = NULL;
	int i;

	if (!hal)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;
	if (usb_dev->state != USB_HAL_DEV_STATE_ENABLED)
		return -EAGAIN;

	mutex_lock(&usb_dev->sender_lock);
	for (i = 0; i < 2; i++) {
		int index = (usb_dev->current_request + i) % 2;

		if (try_wait_for_completion(&usb_dev->requests[index].done)) {
			request = &usb_dev->requests[index];
			usb_dev->current_request = 1 - index;
			break;
		}
	}

	if (!request) {
		mutex_unlock(&usb_dev->sender_lock);
		return -EBUSY;
	}
	request->first_frame = false;
	mutex_unlock(&usb_dev->sender_lock);

	usb_hal_pack_keepalive_block(request);
	usb_dev->stat.keepalive_send++;
	queue_work(system_long_wq, &request->work);
	return 0;
}

static struct fourcc_format_desc g_support_arr[] = {
	{DRM_FORMAT_RGB565,   USB_HAL_COLOR_FORMAT_RGB, 16, USB_HAL_COLOR_FORMAT_RGB565},
	{DRM_FORMAT_RGB888,   USB_HAL_COLOR_FORMAT_RGB, 24, USB_HAL_COLOR_FORMAT_RGB888},
	{DRM_FORMAT_BGR888,   USB_HAL_COLOR_FORMAT_RGB, 24, USB_HAL_COLOR_FORMAT_RGB888},
	{DRM_FORMAT_XRGB8888, USB_HAL_COLOR_FORMAT_RGB, 32, USB_HAL_COLOR_FORMAT_RGB888},
	{DRM_FORMAT_XBGR8888, USB_HAL_COLOR_FORMAT_RGB, 32, USB_HAL_COLOR_FORMAT_RGB888},
	{DRM_FORMAT_ARGB8888, USB_HAL_COLOR_FORMAT_RGB, 32, USB_HAL_COLOR_FORMAT_RGB888},
	{DRM_FORMAT_ABGR8888, USB_HAL_COLOR_FORMAT_RGB, 32, USB_HAL_COLOR_FORMAT_RGB888},
	{DRM_FORMAT_NV12,     USB_HAL_COLOR_FORMAT_YUV, 12, USB_HAL_COLOR_FORMAT_YUV422},
	{DRM_FORMAT_YUV420,   USB_HAL_COLOR_FORMAT_YUV, 12, USB_HAL_COLOR_FORMAT_YUV422}
};

static int g_support_num = (sizeof(g_support_arr) / sizeof(struct fourcc_format_desc));

int usb_hal_get_hpd_status(struct usb_hal *hal, u32 *status)
{
	struct usb_hal_dev *usb_dev;

	if (!hal || !status)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;
	return usb_dev->hal_dev->funcs->get_hpd_status(usb_dev->udev, status);
}

int usb_hal_get_edid(struct usb_hal *hal, int block, u8 *buf, u32 len)
{
	struct usb_hal_dev *usb_dev;

	if (!hal || !buf)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;
	return usb_dev->hal_dev->funcs->get_edid(usb_dev->udev, hal->misc_timing,
						  hal->detail_count, hal->chip_id,
						  hal->port_type, hal->sdram_type,
						  block, buf, len);
}

int usb_hal_check_mode_for_custom_mode(struct usb_hal_dev *usb_dev,
				       struct usb_hal_video_mode *mode)
{
	int ret = 1;
	int i;

	for (i = 0; i < usb_dev->custom_mode_cnt; i++) {
		if (usb_dev->custom_mode[i].width == mode->width &&
		    usb_dev->custom_mode[i].height == mode->height &&
		    usb_dev->custom_mode[i].rate == mode->rate) {
			ret = 0;
			break;
		}
	}

	return ret;
}

/* may be called multi times, can't record log */
int usb_hal_video_mode_valid(struct usb_hal *hal, struct usb_hal_video_mode *mode)
{
	struct usb_hal_dev *usb_dev;
	u32 min_sram_size, sram_size;
	u8 rate, vic = 0;
	u16 width, height;
	int ret;

	if (!hal || !mode)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;

	ret = usb_hal_check_mode_for_custom_mode(usb_dev, mode);
	if (!ret)
		return ret;

	width = (u16)mode->width;
	height = (u16)mode->height;
	rate = (u8)mode->rate;
	ret = usb_dev->hal_dev->funcs->get_mode_vic(width, height, rate, &vic);
	if (ret)
		return ret;

	/* min pix size is 2 bytes in mem, sdram is divided 2 frames,
	 * so min sram size is w * h * 2 * 2
	 */
	min_sram_size = (((u32)(mode->width * mode->height)) << 2);
	sram_size = SDRAM_TYPE_TO_SIZE(hal->sdram_type);
	if (min_sram_size > sram_size)
		return -EINVAL;

	return 0;
}

int usb_hal_get_vic_for_cunstom_mode(struct usb_hal_dev *usb_dev,
				     u16 width, u16 height, u8 rate, u8 *vic)
{
	int ret = 1;
	int i;

	for (i = 0; i < usb_dev->custom_mode_cnt; i++) {
		if (usb_dev->custom_mode[i].width == width &&
		    usb_dev->custom_mode[i].height == height &&
		    usb_dev->custom_mode[i].rate == rate) {
			*vic = usb_dev->custom_mode[i].vic;
			ret = 0;
			break;
		}
	}

	return ret;
}

int usb_hal_get_vic(struct usb_hal *hal, u16 width, u16 height, u8 rate, u8 *vic)
{
	int ret;
	struct usb_hal_dev *usb_dev;

	if (!hal || !vic)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;
	ret = usb_hal_get_vic_for_cunstom_mode(usb_dev, width, height, rate, vic);
	if (!ret)
		return ret;

	return usb_dev->hal_dev->funcs->get_mode_vic(width, height, rate, vic);
}

static struct fourcc_format_desc *usb_hal_find_desc(u32 fourcc)
{
	int i;
	struct fourcc_format_desc *desc = NULL;

	for (i = 0; i < g_support_num; i++) {
		if (g_support_arr[i].fourcc == fourcc) {
			desc = &g_support_arr[i];
			break;
		}
	}

	return desc;
}

unsigned int usb_hal_get_bpp_by_fourcc(u32 fourcc)
{
	struct fourcc_format_desc *desc = usb_hal_find_desc(fourcc);

	return desc ? desc->bpp : 0;
}

int usb_hal_is_support_fourcc(u32 fourcc)
{
	struct fourcc_format_desc *desc = usb_hal_find_desc(fourcc);

	return desc ? 1 : 0;
}

int usb_hal_enable(struct usb_hal *hal, struct usb_hal_video_mode *mode, u32 fourcc)
{
	struct usb_hal_dev *usb_dev;
	struct usb_hal_event event;
	u64 frame_bytes;
	u64 frame_mib_per_second;
	u8 color_in;
	struct fourcc_format_desc *desc;

	if (!hal || !mode)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;

	if (usb_hal_video_mode_valid(hal, mode)) {
		dev_err(&usb_dev->udev->dev,
			"mode is invalid! width:%d height:%d chip_type:%d\n",
			mode->width, mode->height, hal->sdram_type);
		return -EINVAL;
	}

	if (!usb_hal_is_support_fourcc(fourcc)) {
		dev_err(&usb_dev->udev->dev, "invalid fourcc:0x%x!\n", fourcc);
		return -EINVAL;
	}

	usb_dev->mode = *mode;

	frame_bytes = (u64)mode->width * mode->height * 2 + 16;
	frame_mib_per_second = frame_bytes * mode->rate / (1024 * 1024);
	dev_info(&usb_dev->udev->dev,
		 "mode bandwidth budget:%dx%d@%d full-frame:%llu bytes yuv422:%llu MiB/s\n",
		 mode->width, mode->height, mode->rate,
		 (unsigned long long)frame_bytes,
		 (unsigned long long)frame_mib_per_second);

	desc = usb_hal_find_desc(fourcc);
	usb_dev->color_out = (desc->bpp > 16) ? USB_HAL_COLOR_FORMAT_RGB888
					       : USB_HAL_COLOR_FORMAT_RGB565;
	usb_dev->vpack_in = USB_HAL_COLOR_FORMAT_YUV422;

	color_in = ((usb_dev->vpack_out << 4) | usb_dev->vpack_in);

	memset(&event, 0, sizeof(event));
	event.base.type = USB_HAL_EVENT_TYPE_ENABLE;
	event.base.length = sizeof(event);
	event.para.enable.width = mode->width;
	event.para.enable.height = mode->height;
	event.para.enable.vic = mode->vic;
	event.para.enable.trans_mode = usb_dev->trans_mode;
	event.para.enable.color_in = color_in;
	event.para.enable.color_out = usb_dev->color_out;

	kfifo_in(usb_dev->fifo, &event, sizeof(event));
	up(&usb_dev->sema);

	return 0;
}

int usb_hal_disable(struct usb_hal *hal)
{
	struct usb_hal_dev *usb_dev;
	struct usb_hal_event event;

	if (!hal)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;

	memset(&event, 0, sizeof(event));
	event.base.type = USB_HAL_EVENT_TYPE_DISABLE;
	event.base.length = sizeof(event);
	kfifo_in(usb_dev->fifo, &event, sizeof(event));
	up(&usb_dev->sema);

	return 0;
}

int usb_hal_is_disabled(struct usb_hal *hal)
{
	struct usb_hal_dev *usb_dev;

	if (!hal)
		return 1;

	usb_dev = (struct usb_hal_dev *)hal->private;
	return usb_dev->state == USB_HAL_DEV_STATE_DISABLED ? 1 : 0;
}

bool usb_hal_find_valid_pixman(u8 *buf, int stride, int len)
{
	int index;
	u8 *pbuf = buf;

	for (index = 0; index < len; index++) {
		if (pbuf[3] >= 64)
			return true;
		pbuf += stride;
	}
	return false;
}

int usb_hal_cursor_set(struct usb_hal *hal, u8 *buf)
{
	u8 *pbuf;
	int x, y;
	int x1 = 0, y1 = 0, x2 = USB_HAL_CURSOR_WIDTH, y2 = USB_HAL_CURSOR_HEIGHT;
	struct usb_hal_dev *usb_dev;

	if (!hal)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;

	if (usb_dev->state != USB_HAL_DEV_STATE_ENABLED) {
		usb_dev->stat.state_error++;
		return -EPERM;
	}

	mutex_lock(&usb_dev->cursor_buf.mutex);
	if (!buf) {
		usb_dev->cursor_buf.valid = false;
		mutex_unlock(&usb_dev->cursor_buf.mutex);
		return 0;
	}

	memcpy(usb_dev->cursor_buf.buf, buf, USB_HAL_CURSOR_BUF_SIZE);

	pbuf = buf;
	for (x = 0; x < USB_HAL_CURSOR_WIDTH; x++) {
		if (usb_hal_find_valid_pixman(pbuf, 256, USB_HAL_CURSOR_HEIGHT)) {
			x1 = x;
			break;
		}
		pbuf += 4;
	}

	pbuf = buf + (USB_HAL_CURSOR_WIDTH - 1) * 4;
	for (x = USB_HAL_CURSOR_WIDTH; x > x1; x--) {
		if (usb_hal_find_valid_pixman(pbuf, 256, USB_HAL_CURSOR_HEIGHT)) {
			x2 = x + 1;
			break;
		}
		pbuf -= 4;
	}

	if (x1 >= x2) {
		usb_dev->cursor_buf.valid = false;
		mutex_unlock(&usb_dev->cursor_buf.mutex);
		return 0;
	}

	pbuf = buf + (x1 * 4);
	for (y = 0; y < USB_HAL_CURSOR_HEIGHT; y++) {
		if (usb_hal_find_valid_pixman(pbuf, 4, x2 - x1)) {
			y1 = y;
			break;
		}
		pbuf += USB_HAL_CURSOR_WIDTH * 4;
	}

	pbuf = buf + USB_HAL_CURSOR_WIDTH * USB_HAL_CURSOR_HEIGHT * 4
	       - (USB_HAL_CURSOR_WIDTH - x1) * 4;
	for (y = USB_HAL_CURSOR_HEIGHT; y > y1; y--) {
		if (usb_hal_find_valid_pixman(pbuf, 4, x2 - x1)) {
			y2 = y + 1;
			break;
		}
		pbuf -= USB_HAL_CURSOR_WIDTH * 4;
	}

	if (y1 >= y2) {
		usb_dev->cursor_buf.valid = false;
		mutex_unlock(&usb_dev->cursor_buf.mutex);
		return 0;
	}

	usb_dev->cursor_buf.x1 = x1;
	usb_dev->cursor_buf.x2 = x2;
	usb_dev->cursor_buf.y1 = y1;
	usb_dev->cursor_buf.y2 = y2;
	usb_dev->cursor_buf.valid = true;

	mutex_unlock(&usb_dev->cursor_buf.mutex);

	return 0;
}

int usb_hal_cursor_move(struct usb_hal *hal, int x, int y)
{
	struct usb_hal_dev *usb_dev;

	if (!hal)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;
	if (usb_dev->state != USB_HAL_DEV_STATE_ENABLED) {
		usb_dev->stat.state_error++;
		return -EPERM;
	}

	mutex_lock(&usb_dev->cursor_buf.mutex);
	usb_dev->cursor_buf.x = x;
	usb_dev->cursor_buf.y = y;
	mutex_unlock(&usb_dev->cursor_buf.mutex);

	return 0;
}

int usb_hal_add_custom_mode(struct usb_hal *hal, int width, int height, int rate,
			    unsigned char vic)
{
	struct usb_hal_dev *usb_dev;

	if (!hal)
		return -EINVAL;

	usb_dev = (struct usb_hal_dev *)hal->private;
	if (usb_dev->custom_mode_cnt >= USB_HAL_MAX_CUSTOM_MODE)
		return -ERANGE;

	usb_dev->custom_mode[usb_dev->custom_mode_cnt].width = width;
	usb_dev->custom_mode[usb_dev->custom_mode_cnt].height = height;
	usb_dev->custom_mode[usb_dev->custom_mode_cnt].rate = rate;
	usb_dev->custom_mode[usb_dev->custom_mode_cnt].vic = vic;
	usb_dev->custom_mode_cnt++;

	return 0;
}

static void usb_hal_free_buf(struct usb_hal_dev *usb_dev)
{
	int i;

	for (i = 0; i < 2; i++)
		usb_hal_free_frame_request(&usb_dev->requests[i]);

	if (usb_dev->cursor_buf.buf) {
		vfree(usb_dev->cursor_buf.buf);
		usb_dev->cursor_buf.buf = NULL;
	}
}

static int usb_dev_alloc_buf(struct usb_hal_dev *usb_dev)
{
	int i;
	int ret;

	usb_dev->cursor_buf.valid = false;
	usb_dev->cursor_buf.buf = vmalloc(USB_HAL_CURSOR_BUF_SIZE);
	if (!usb_dev->cursor_buf.buf) {
		dev_err(&usb_dev->udev->dev, "cursor_buf vmalloc failed!\n");
		return -ENOMEM;
	}
	memset(usb_dev->cursor_buf.buf, 0, USB_HAL_CURSOR_BUF_SIZE);

	for (i = 0; i < 2; i++) {
		ret = usb_hal_init_frame_request(usb_dev, &usb_dev->requests[i], USB_HAL_BUF_SIZE);
		if (ret) {
			while (i--)
				usb_hal_free_frame_request(&usb_dev->requests[i]);
			vfree(usb_dev->cursor_buf.buf);
			usb_dev->cursor_buf.buf = NULL;
			return ret;
		}
	}
	return 0;
}

static struct device *usb_hal_intf_get_dma_device(struct usb_interface *intf)
{
#if KERNEL_VERSION(4, 12, 0) <= LINUX_VERSION_CODE
	struct usb_device *udev = interface_to_usbdev(intf);
	struct device *dmadev;

	if (!udev->bus)
		return NULL;

	dmadev = get_device(udev->bus->sysdev);
	if (!dmadev || !dmadev->dma_mask) {
		put_device(dmadev);
		return NULL;
	}

	return dmadev;
#else
	return NULL;
#endif
}

static int usb_dev_hal_init(struct usb_hal *usb_hal)
{
	int ret = 0;
	struct usb_hal_dev *usb_dev = (struct usb_hal_dev *)usb_hal->private;
	struct usb_device *udev = usb_dev->udev;

	ret = usb_dev->hal_dev->funcs->get_chip_id(udev, &usb_hal->chip_id);
	if (ret) {
		dev_err(&udev->dev, "get chip id failed! ret=%d\n", ret);
		return -ENOENT;
	}

	ret = usb_dev->hal_dev->funcs->get_port_type(udev, &usb_hal->port_type);
	if (ret) {
		dev_err(&udev->dev, "get video port type failed! ret=%d\n", ret);
		return -ENOENT;
	}

	ret = usb_dev->hal_dev->funcs->get_sdram_type(udev, &usb_hal->sdram_type);
	if (ret) {
		dev_err(&udev->dev, "get sdram type failed! ret=%d\n", ret);
		return -ENOENT;
	}

	if (usb_hal->port_type == VIDEO_PORT_CVBS_SVIDEO) {
		ret = usb_hal_add_custom_mode(usb_hal, 720, 480, 60, VFMT_CEA_02_720X480P_60HZ);
		if (ret)
			dev_err(&udev->dev, "add 720*480 failed! ret=%d\n", ret);
	}

	return usb_dev->hal_dev->funcs->init_dev(udev, usb_hal->chip_id,
						  usb_hal->port_type, usb_hal->sdram_type);
}

struct usb_hal *usb_hal_init(struct usb_interface *interface,
			     const struct usb_device_id *id,
			     struct kfifo *fifo, u32 index)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usb_hal *usb_hal = NULL;
	struct usb_hal_dev *usb_dev = NULL;
	char name[32];
	int ret = 0;

	if (!interface || !id || !fifo) {
		pr_err("%s: null pointer!\n", __func__);
		return NULL;
	}

	usb_hal = kzalloc(sizeof(*usb_hal), GFP_KERNEL);
	if (!usb_hal)
		goto err;

	usb_dev = kzalloc(sizeof(*usb_dev), GFP_KERNEL);
	if (!usb_dev)
		goto err;

	usb_dev->hal_dev = msdisp_hal_find_dev(id, udev);
	if (!usb_dev->hal_dev) {
		dev_err(&udev->dev, "Can't find hal dev! vid=0x%x pid=0x%x\n",
			id->idVendor, id->idProduct);
		goto err;
	}

	usb_hal->private = usb_dev;
	usb_hal->interface = interface;

	usb_dev->udev = udev;
	usb_dev->hal = usb_hal;
	usb_dev->fifo = fifo;
	usb_dev->index = index;
	usb_dev->vpack_out = USB_HAL_COLOR_FORMAT_YUV422;
	usb_dev->trans_mode = USB_HAL_TRANS_MODE_MANUAL_BLOCK;
	usb_dev->state = USB_HAL_DEV_STATE_UNKNOWN;

	usb_dev->dma_dev = usb_hal_intf_get_dma_device(interface);
	if (!usb_dev->dma_dev)
		dev_warn(&udev->dev, "buffer sharing not supported"); /* not an error */

	ret = usb_dev_hal_init(usb_hal);
	if (ret) {
		dev_err(&udev->dev, "usb dev hal init failed! ret=%d\n", ret);
		goto err;
	} else {
		dev_info(&udev->dev, "chip id:0x%x port:0x%x sdram:0x%x\n",
			 usb_hal->chip_id, usb_hal->port_type, usb_hal->sdram_type);
	}

	dev_info(&udev->dev, "usb speed:%s transfer ep:0x%x max packet:%d dma:%s\n",
		 usb_speed_string(udev->speed),
		 usb_dev->hal_dev->funcs->get_transfer_bulk_ep(),
		 usb_maxpacket(udev,
			       usb_sndbulkpipe(udev,
					       usb_dev->hal_dev->funcs->get_transfer_bulk_ep())),
		 usb_dev->dma_dev ? "yes" : "no");

	ret = usb_dev_alloc_buf(usb_dev);
	if (ret) {
		dev_err(&udev->dev, "alloc buf failed!\n");
		goto err;
	}

	usb_dev->state = USB_HAL_DEV_STATE_UNKNOWN;
	usb_dev->bus_status = MS9132_USB_BUS_STATUS_NORMAL;

	mutex_init(&usb_dev->sender_lock);
	mutex_init(&usb_dev->cursor_buf.mutex);

	sema_init(&usb_dev->sema, 1);

	memset(name, 0, 32);
	snprintf(name, 32, "msdisp%d_send", index);
	atomic_set(&usb_dev->thread_run_flag, 1);
	usb_dev->thread = kthread_run(usb_hal_state_machine_entry, usb_hal, name);

	usb_hal_sysfs_init(interface);
	goto out;

err:
	if (usb_dev && usb_dev->dma_dev)
		put_device(usb_dev->dma_dev);

	kfree(usb_dev);
	usb_dev = NULL;

	kfree(usb_hal);
	usb_hal = NULL;

out:
	dev_info(&udev->dev, "init usb_hal%d %s!\n", index,
		 usb_hal ? "success" : "failed");
	return usb_hal;
}

void usb_hal_destroy(struct usb_hal *hal)
{
	struct usb_hal_dev *usb_dev = (struct usb_hal_dev *)hal->private;
	struct usb_interface *interface = hal->interface;
	int index = usb_dev->index;

	if (usb_dev->thread) {
		usb_hal_stop_thread(usb_dev);
		msleep(300);
		usb_dev->thread = NULL;
	}

	usb_hal_sysfs_exit(interface);
	usb_hal_free_buf(usb_dev);
	if (usb_dev->dma_dev)
		put_device(usb_dev->dma_dev);
	kfree(usb_dev);
	kfree(hal);

	dev_info(&usb_dev->udev->dev, "destroy usb_hal%d success!\n", index);
}

void usb_hal_init_gpio(struct usb_hal *hal)
{
	u8 value;
	u8 chip_id;
	struct usb_hal_dev *usb_dev;

	usb_dev = (struct usb_hal_dev *)hal->private;

	usb_dev->hal_dev->funcs->get_chip_id(usb_dev->udev, &chip_id);

	if (chip_id == CHIP_ID_9132) {
		usb_dev->hal_dev->funcs->sfr_read_byte(usb_dev->udev, 0xB0, &value);
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xB0, (value & ~0x04));
		usb_dev->hal_dev->funcs->sfr_read_byte(usb_dev->udev, 0xA0, &value);
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xA0, (value | 0x04));

		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xC7, 0xD1);
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xC8, 0xC0);
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xCA, 0x00);

		usb_dev->hal_dev->funcs->xdata_read_byte(usb_dev->udev, 0xF01F, &value);
		value |= 0x10;
		value &= ~0x80;
		usb_dev->hal_dev->funcs->xdata_write_byte(usb_dev->udev, 0xF01F, value);
	} else {
		usb_dev->hal_dev->funcs->sfr_read_byte(usb_dev->udev, 0xB0, &value);
		value |= 0xDC;
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xB0, value);

		usb_dev->hal_dev->funcs->sfr_read_byte(usb_dev->udev, 0xA0, &value);
		value &= ~0x18;
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xA0, value);

		usb_dev->hal_dev->funcs->sfr_read_byte(usb_dev->udev, 0xA0, &value);
		value |= ~0x04;
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xA0, value);

		usb_dev->hal_dev->funcs->xdata_read_byte(usb_dev->udev, 0xF016, &value);
		value &= ~0x04;
		usb_dev->hal_dev->funcs->xdata_write_byte(usb_dev->udev, 0xF016, value);
	}
}

void usb_hal_resume_gpio(struct usb_hal *hal)
{
	u8 value;
	u8 chip_id;
	struct usb_hal_dev *usb_dev;

	usb_dev = (struct usb_hal_dev *)hal->private;

	usb_dev->hal_dev->funcs->get_chip_id(usb_dev->udev, &chip_id);
	if (chip_id == CHIP_ID_9132) {
		usb_dev->hal_dev->funcs->xdata_read_byte(usb_dev->udev, 0xF01F, &value);
		value &= ~0x10;
		value &= ~0x80;
		usb_dev->hal_dev->funcs->xdata_write_byte(usb_dev->udev, 0xF01F, value);

		usb_dev->hal_dev->funcs->sfr_read_byte(usb_dev->udev, 0xB0, &value);
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xB0, (value | 0x3c));
		usb_dev->hal_dev->funcs->sfr_read_byte(usb_dev->udev, 0xB1, &value);
		usb_dev->hal_dev->funcs->sfr_write_byte(usb_dev->udev, 0xB1, (value | 0x3c));
	} else {
		/* 912x no need to resume */
	}
}

void usb_hal_read_custom_timing(struct usb_hal *hal)
{
	int i;
	int ret;
	int count = 0, rate = 0;

	u8 data[32];
	u8 magic[8] = "modify";
	u16 base_addr;
	struct misctiming timing[2];

	struct usb_hal_dev *usb_dev;

	usb_dev = (struct usb_hal_dev *)hal->private;

	if (hal->chip_id == 0)
		base_addr = MS913X_CUSTOM_TIMING_ADDR;
	else
		base_addr = MS912X_CUSTOM_TIMING_ADDR;

	usb_dev->hal_dev->funcs->read_flash(usb_dev->udev, base_addr, data, 7);
	if (memcmp(data, magic, 6) != 0)
		return;

	if (data[6] == 0x31)
		count = 1;
	else if (data[6] == 0x32)
		count = 2;
	else
		return;

	usb_dev->hal_dev->funcs->read_flash(usb_dev->udev, base_addr + 0x10, data,
					    sizeof(struct misctiming));
	memcpy(&timing[0], data, sizeof(struct misctiming));

	if (count == 2) {
		usb_dev->hal_dev->funcs->read_flash(usb_dev->udev, base_addr + 0x30, data,
						    sizeof(struct misctiming));
		memcpy(&timing[1], data, sizeof(struct misctiming));
	}

	for (i = 0; i < count; i++) {
		rate = (timing[i].vfreq + 50) / 100;
		ret = usb_hal_add_custom_mode(hal, timing[i].hactive, timing[i].vactive,
					      rate, timing[i].vic);
		if (!ret)
			dev_info(&usb_dev->udev->dev, "add mode:vic_%d %dx%d@%d success\n",
				 timing[i].vic, timing[i].hactive, timing[i].vactive, rate);
		else
			dev_warn(&usb_dev->udev->dev, "add mode:vic_%d %dx%d@%d failed\n",
				 timing[i].vic, timing[i].hactive, timing[i].vactive, rate);
	}

	hal->detail_count = count;
	memcpy(hal->misc_timing, timing, sizeof(timing));
}
