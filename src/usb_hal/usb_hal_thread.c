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
 * usb_hal_thread.c -- Drm driver for MacroSilicon chip 913x and 912x
 */

#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "usb_hal_interface.h"
#include "usb_hal_dev.h"
#include "usb_hal_event.h"
#include "usb_hal_thread.h"
#include "hal_adaptor.h"

static void usb_hal_dev_do_enable(struct usb_hal_dev *usb_dev,
				  struct usb_hal_event *event)
{
	const struct msdisp_hal_dev *hal_dev = usb_dev->hal_dev;
	struct usb_device *udev = usb_dev->udev;
	struct usb_hal *hal = usb_dev->hal;
	int ret;

	ret = hal_dev->funcs->event_proc(usb_dev->udev, event, hal->chip_id,
					 hal->port_type, hal->sdram_type);
	if (ret)
		dev_err(&udev->dev, "hal dev enable event proc failed! ret=%d\n",
			ret);

	usb_dev->state = USB_HAL_DEV_STATE_ENABLED;
	usb_dev->first_buf_send = 0;
}

static void usb_hal_dev_do_disable(struct usb_hal_dev *usb_dev,
				   struct usb_hal_event *event)
{
	const struct msdisp_hal_dev *hal_dev = usb_dev->hal_dev;
	struct usb_device *udev = usb_dev->udev;
	struct usb_hal *hal = usb_dev->hal;
	int ret;

	ret = hal_dev->funcs->event_proc(usb_dev->udev, event, hal->chip_id,
					 hal->port_type, hal->sdram_type);
	if (ret)
		dev_err(&udev->dev, "hal dev disable event proc failed! ret=%d\n",
			ret);

	usb_dev->state = USB_HAL_DEV_STATE_DISABLED;
}

static void usb_hal_dev_state_unknown(struct usb_hal_dev *usb_dev,
				      struct usb_hal_event *event)
{
	if (event->base.type == USB_HAL_EVENT_TYPE_ENABLE)
		dev_info(&usb_dev->udev->dev, "event:%x width:%d height:%d\n",
			 event->base.type, event->para.enable.width,
			 event->para.enable.height);

	/* in unknown state, only process enable event */
	if (event->base.type != USB_HAL_EVENT_TYPE_ENABLE)
		return;

	usb_hal_dev_do_enable(usb_dev, event);
}

static void usb_hal_state_machine(struct usb_hal_dev *usb_dev,
				  struct kfifo *fifo)
{
	struct usb_hal_event event;
	int ret;

	ret = down_timeout(&usb_dev->sema, 1);
	if (usb_dev->bus_status == MS9132_USB_BUS_STATUS_SUSPEND)
		return;

	if (!ret) {
		while (kfifo_out(fifo, &event, sizeof(event)) != 0) {
			switch (usb_dev->state) {
			case USB_HAL_DEV_STATE_UNKNOWN:
			case USB_HAL_DEV_STATE_DISABLED:
				usb_hal_dev_state_unknown(usb_dev, &event);
				break;
			case USB_HAL_DEV_STATE_ENABLED:
				if (event.base.type == USB_HAL_EVENT_TYPE_DISABLE)
					usb_hal_dev_do_disable(usb_dev, &event);
				break;
			}
		}
	}

	if (usb_dev->state != USB_HAL_DEV_STATE_ENABLED ||
	    !usb_dev->first_buf_send)
		return;

	if (time_after(jiffies, READ_ONCE(usb_dev->last_send_jiffies) + 2 * HZ)) {
		ret = usb_hal_send_keepalive(usb_dev->hal);
		if (ret && ret != -EBUSY)
			WRITE_ONCE(usb_dev->last_send_jiffies, jiffies);
	}
}

int usb_hal_state_machine_entry(void *data)
{
	struct usb_hal *usb_hal = (struct usb_hal *)data;
	struct usb_hal_dev *usb_dev = (struct usb_hal_dev *)usb_hal->private;
	struct kfifo *fifo;

	dev_info(&usb_dev->udev->dev, "state machine proc enter!\n");

	fifo = usb_dev->fifo;

	while (atomic_read(&usb_dev->thread_run_flag))
		usb_hal_state_machine(usb_dev, fifo);

	return 0;
}

void usb_hal_stop_thread(struct usb_hal_dev *usb_dev)
{
	atomic_set(&usb_dev->thread_run_flag, 0);
}
