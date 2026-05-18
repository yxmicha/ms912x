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
 * usb_hal_sysfs.c -- Drm driver for MacroSilicon chip 913x and 912x
 */

#include <linux/types.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/usb.h>

#include "hal_adaptor.h"
#include "usb_hal_dev.h"
#include "usb_hal_event.h"
#include "usb_hal_interface.h"

static ssize_t usb_hal_frame_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct usb_hal *usb_hal = usb_intf_device_to_hal_func(dev);
	struct usb_hal_dev *usb_dev = usb_hal->private;
	struct usb_hal_dev_frame_stat *stat = &usb_dev->stat;
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "send total:%lld\n", stat->send_total);
	len += snprintf(buf + len, PAGE_SIZE - len, "send success:%lld\n", stat->send_success);
	len += snprintf(buf + len, PAGE_SIZE - len, "update sequence:%lld\n",
			stat->update_sequence);
	len += snprintf(buf + len, PAGE_SIZE - len, "dropped stale:%lld\n", stat->dropped_stale);
	len += snprintf(buf + len, PAGE_SIZE - len, "state error count:%lld\n", stat->state_error);
	len += snprintf(buf + len, PAGE_SIZE - len, "send fail:%lld\n", stat->send_fail);
	len += snprintf(buf + len, PAGE_SIZE - len, "pack last ms:%lld\n", stat->pack_last_ms);
	len += snprintf(buf + len, PAGE_SIZE - len, "pack max ms:%lld\n", stat->pack_max_ms);
	len += snprintf(buf + len, PAGE_SIZE - len, "usb last ms:%lld\n", stat->usb_last_ms);
	len += snprintf(buf + len, PAGE_SIZE - len, "usb max ms:%lld\n", stat->usb_max_ms);
	len += snprintf(buf + len, PAGE_SIZE - len, "busy drop:%lld\n", stat->busy_drop);
	len += snprintf(buf + len, PAGE_SIZE - len, "first frame success:%lld\n",
			stat->first_frame_success);
	len += snprintf(buf + len, PAGE_SIZE - len, "keepalive send:%lld\n", stat->keepalive_send);

	return len;
}

static ssize_t usb_hal_reset_stats_store(struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct usb_hal *usb_hal = usb_intf_device_to_hal_func(dev);
	struct usb_hal_dev *usb_dev = usb_hal->private;

	memset(&usb_dev->stat, 0, sizeof(usb_dev->stat));

	return count;
}

static ssize_t usb_hal_dev_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct usb_hal *usb_hal = usb_intf_device_to_hal_func(dev);
	struct usb_hal_dev *usb_dev = usb_hal->private;
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "chip id:0x%x\n", usb_hal->chip_id);
	len += snprintf(buf + len, PAGE_SIZE - len, "video port:0x%x\n", usb_hal->port_type);
	len += snprintf(buf + len, PAGE_SIZE - len, "sdram type:0x%x\n", usb_hal->sdram_type);
	len += snprintf(buf + len, PAGE_SIZE - len, "dev state:0x%x\n", usb_dev->state);

	return len;
}

static ssize_t usb_hal_custom_mode_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct usb_hal *usb_hal = usb_intf_device_to_hal_func(dev);
	struct usb_hal_dev *usb_dev = usb_hal->private;
	int len = 0;
	int i;

	len += snprintf(buf + len, PAGE_SIZE - len, "custom mode cnt:%d\n",
			usb_dev->custom_mode_cnt);
	for (i = 0; i < usb_dev->custom_mode_cnt; i++) {
		len += snprintf(buf + len, PAGE_SIZE - len,
				"width:%d height:%d rate:%d vid:%d\n",
				usb_dev->custom_mode[i].width, usb_dev->custom_mode[i].height,
				usb_dev->custom_mode[i].rate, usb_dev->custom_mode[i].vic);
	}

	return len;
}

static ssize_t usb_hal_write_xdata_store(struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct usb_hal *usb_hal = usb_intf_device_to_hal_func(dev);
	struct usb_hal_dev *usb_dev = usb_hal->private;
	const struct msdisp_hal_dev *hal_dev = usb_dev->hal_dev;
	u32 reg, data;
	u8 read_data;
	int ret;

	ret = sscanf(buf, "%x %x\n", &reg, &data);
	if (ret != 2)
		return -EINVAL;

	ret = hal_dev->funcs->xdata_write_byte(usb_dev->udev, (u16)reg, (u8)data);
	if (ret < 0)
		return ret;

	ret = hal_dev->funcs->xdata_read_byte(usb_dev->udev, reg, &read_data);
	if (ret < 0)
		return ret;

	dev_info(dev, "the reg:0x%x data:0x%02x read_data:0x%02x\n", reg, data, read_data);
	return count;
}

static ssize_t usb_hal_read_xdata_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct usb_hal *usb_hal = usb_intf_device_to_hal_func(dev);
	struct usb_hal_dev *usb_dev = usb_hal->private;
	const struct msdisp_hal_dev *hal_dev = usb_dev->hal_dev;
	u16 reg;
	int ret;
	u8 data;

	ret = kstrtou16(buf, 0, &reg);
	if (ret < 0)
		return ret;

	ret = hal_dev->funcs->xdata_read_byte(usb_dev->udev, reg, &data);
	if (ret < 0)
		return ret;

	dev_info(dev, "the reg:0x%x data:0x%02x\n", reg, data);
	return count;
}

static DEVICE_ATTR(frame, 0444, usb_hal_frame_show, NULL);
static DEVICE_ATTR(hal_dev, 0444, usb_hal_dev_show, NULL);
static DEVICE_ATTR(custom_mode, 0444, usb_hal_custom_mode_show, NULL);
static DEVICE_ATTR(reset_stats, 0220, NULL, usb_hal_reset_stats_store);
static DEVICE_ATTR(write_xdata, 0220, NULL, usb_hal_write_xdata_store);
static DEVICE_ATTR(read_xdata, 0220, NULL, usb_hal_read_xdata_store);

static struct attribute *usb_hal_attribute[] = {
	&dev_attr_frame.attr,
	&dev_attr_hal_dev.attr,
	&dev_attr_custom_mode.attr,
	&dev_attr_reset_stats.attr,
	&dev_attr_write_xdata.attr,
	&dev_attr_read_xdata.attr,
	NULL
};

static const struct attribute_group usb_hal_attr_group = {
	.attrs = usb_hal_attribute,
};

void usb_hal_sysfs_init(struct usb_interface *interface)
{
	int r;

	r = sysfs_create_group(&interface->dev.kobj, &usb_hal_attr_group);
	if (r)
		dev_err(&interface->dev, "create sysfs group fialed! ret=%d\n", r);
}

void usb_hal_sysfs_exit(struct usb_interface *interface)
{
	sysfs_remove_group(&interface->dev.kobj, &usb_hal_attr_group);
}
