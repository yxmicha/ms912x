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
 * msdisp_drm_sysfs.c -- Drm driver for MacroSilicon chip 913x and 912x
 */

#include <linux/version.h>
#include <linux/device.h>
#include <linux/fs.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "msdisp_drm_drv.h"

#define to_pipeline(d) container_of(d, struct msdisp_drm_pipeline, dev)

static ssize_t msdisp_drm_frame_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct msdisp_drm_pipeline *pipeline = to_pipeline(dev);
	struct msdisp_drm_frame_stat *stat = &pipeline->frame_stat;
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "total:%lld\n", stat->total);
	len += snprintf(buf + len, PAGE_SIZE - len, "no usb hal:%lld\n", stat->no_usb_hal);
	len += snprintf(buf + len, PAGE_SIZE - len, "no old state:%lld\n", stat->no_old_state);
	len += snprintf(buf + len, PAGE_SIZE - len, "no fb:%lld\n", stat->no_fb);
	len += snprintf(buf + len, PAGE_SIZE - len, "vmap fail:%lld\n", stat->vmap_fail);
	len += snprintf(buf + len, PAGE_SIZE - len, "vmap null:%lld\n", stat->vmap_null);
	len += snprintf(buf + len, PAGE_SIZE - len, "cpu access fail:%lld\n",
			stat->cpu_access_fail);
	len += snprintf(buf + len, PAGE_SIZE - len, "aquire buffer fail:%lld\n",
			stat->acquire_buf_fail);
	len += snprintf(buf + len, PAGE_SIZE - len, "handle fail:%lld\n", stat->handle_fail);
	len += snprintf(buf + len, PAGE_SIZE - len, "full needs refresh:%lld\n",
			stat->full_needs_refresh);
	len += snprintf(buf + len, PAGE_SIZE - len, "full no old state:%lld\n",
			stat->full_no_old_state);
	len += snprintf(buf + len, PAGE_SIZE - len, "full no old fb:%lld\n", stat->full_no_old_fb);
	len += snprintf(buf + len, PAGE_SIZE - len, "full no damage:%lld\n", stat->full_no_damage);

	return len;
}

static ssize_t msdisp_drm_pipeline_info_show(struct device *dev,
					     struct device_attribute *attr,
					     char *buf)
{
	struct msdisp_drm_pipeline *pipeline = to_pipeline(dev);
	int len = 0;

	len += snprintf(buf + len, PAGE_SIZE - len, "reg hal:%d\n", pipeline->reg_flag);
	len += snprintf(buf + len, PAGE_SIZE - len, "status :%d\n", pipeline->drm_status);
	len += snprintf(buf + len, PAGE_SIZE - len, "width:%d\n", pipeline->drm_width);
	len += snprintf(buf + len, PAGE_SIZE - len, "height:%d\n", pipeline->drm_height);
	len += snprintf(buf + len, PAGE_SIZE - len, "rate:%d\n", pipeline->drm_rate);
	len += snprintf(buf + len, PAGE_SIZE - len, "fb format:%x\n", pipeline->drm_fb_format);

	return len;
}

static ssize_t msdisp_drm_dump_fb_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct msdisp_drm_pipeline *pipeline = to_pipeline(dev);

	if (count > 255) {
		dev_err(dev, "file path is too long\n");
		goto out;
	}
	memcpy(pipeline->dump_fb_filename, buf, count);
	if ('\n' == pipeline->dump_fb_filename[count - 1])
		pipeline->dump_fb_filename[count - 1] = '\0';

	atomic_set(&pipeline->dump_fb_flag, 1);

out:
	dev_info(dev, "the dump fb filename:%s len:%zu count:%zu\n",
		 pipeline->dump_fb_filename, strlen(pipeline->dump_fb_filename), count);
	return count;
}

static DEVICE_ATTR(dump_fb, 0220, NULL, msdisp_drm_dump_fb_store);
static DEVICE_ATTR(frame, 0444, msdisp_drm_frame_show, NULL);
static DEVICE_ATTR(info, 0444, msdisp_drm_pipeline_info_show, NULL);

static struct attribute *msdisp_drm_attribute[] = {
	&dev_attr_dump_fb.attr,
	&dev_attr_frame.attr,
	&dev_attr_info.attr,
	NULL
};

static const struct attribute_group msdisp_drm_attr_group = {
	.attrs = msdisp_drm_attribute,
};

static const struct attribute_group *attr_groups[] = {
	&msdisp_drm_attr_group,
	NULL
};

static int do_init_device(struct device *dev, struct device *parent, int index)
{
	int ret;

	dev->driver = NULL;
	dev->bus = NULL;
	dev->type = NULL;
	dev->groups = attr_groups;
	dev->parent = parent;
	device_initialize(dev);
	dev_set_name(dev, "pipeline%d", index);

	ret = device_add(dev);
	if (ret)
		dev_err(parent, "add pipeline%d's device failed! ret=%d\n", index, ret);

	return ret;
}

void msdisp_drm_sysfs_init(struct msdisp_drm_device *msdisp_drm)
{
	int i, ret;
	struct device *dev;

	for (i = 0; i < msdisp_drm->pipeline_cnt; i++) {
		dev = &msdisp_drm->pipeline[i].dev;
		ret = do_init_device(dev, msdisp_drm->drm.dev, i);
		msdisp_drm->pipeline[i].dev_init = ((ret == 0) ? 1 : 0);
	}
}

void msdisp_drm_sysfs_exit(struct msdisp_drm_device *msdisp_drm)
{
	int i;
	struct device *dev;

	for (i = 0; i < msdisp_drm->pipeline_cnt; i++) {
		dev = &msdisp_drm->pipeline[i].dev;
		if (msdisp_drm->pipeline[i].dev_init)
			device_del(dev);
		else
			put_device(dev);
	}
}
