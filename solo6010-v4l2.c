/*
 * Copyright (C) 2010 Ben Collins <bcollins@bluecherry.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>

#include "solo6010-v4l2.h"
#include "solo6010-p2m.h"

#define SOLO_H_SIZE_FDMA	2048
#define SOLO_PAGE_SIZE		4
#define SOLO_DISP_PIX_FORMAT	V4L2_PIX_FMT_UYVY
#define SOLO_DISP_PIX_FIELD	V4L2_FIELD_INTERLACED_TB
#define SOLO_DEFAULT_CHAN	0
#define CAPTURE_MAX_BANDWIDTH	32
#define VI_PROG_HSIZE		(1280-16)
#define VI_PROG_VSIZE		(1024-16)

//#define COPY_WHOLE_LINE

/* Image size is two fields, SOLO_H_SIZE_FDMA is one horizontal line */
#ifdef COPY_WHOLE_LINE
#define solo_image_size(__solo) (SOLO_H_SIZE_FDMA * __solo->video_vsize * 2)
#else
#define solo_image_size(__solo)	(__solo->video_hsize * __solo->video_vsize * 4)
#endif

static unsigned video_nr = -1;
module_param(video_nr, uint, 0644);
MODULE_PARM_DESC(video_nr, "videoX start number, -1 is autodetect (default)");

static void erase_on(struct solo6010_dev *solo_dev)
{
	solo_reg_write(solo_dev, SOLO_VO_DISP_ERASE, SOLO_VO_DISP_ERASE_ON);
	solo_dev->erasing = 1;
}

static int erase_off(struct solo6010_dev *solo_dev)
{
	if (solo_dev->erasing)
		return 0;

	solo_reg_write(solo_dev, SOLO_VO_DISP_ERASE, 0);
	solo_dev->erasing = 0;

	return 1;
}

static int solo_disp_ch(struct solo6010_dev *solo_dev, u8 ch, int on)
{
	if (ch >= solo_dev->nr_chans)
		return -EINVAL;

	/* Here, we just keep window/channel the same */
	solo_reg_write(solo_dev, SOLO_VI_WIN_CTRL0(ch),
		       SOLO_VI_WIN_CHANNEL(ch) |
		       SOLO_VI_WIN_SX(on ? 0: solo_dev->video_hsize) |
		       SOLO_VI_WIN_EX(solo_dev->video_hsize) |
		       SOLO_VI_WIN_SCALE(on ? 1 : 0));

	solo_reg_write(solo_dev, SOLO_VI_WIN_CTRL1(ch),
		       SOLO_VI_WIN_SY(on ? 0 : solo_dev->video_vsize) |
		       SOLO_VI_WIN_EY(solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VI_WIN_ON(ch), 0x00000001);

	/* Expansion modes */
	solo_reg_write(solo_dev, SOLO_VO_EXP(0), SOLO_VO_EXP_ON |
		       SOLO_VO_EXP_SIZE(0));
	solo_reg_write(solo_dev, SOLO_VO_EXP(1), SOLO_VO_EXP_ON |
		       SOLO_VO_EXP_SIZE(2));
	solo_reg_write(solo_dev, SOLO_VO_EXP(2), SOLO_VO_EXP_ON |
		       SOLO_VO_EXP_SIZE(698));
	solo_reg_write(solo_dev, SOLO_VO_EXP(2), SOLO_VO_EXP_ON |
		       SOLO_VO_EXP_SIZE(700));

	return 0;
}

static int solo_disp_set_ch(struct solo6010_dev *solo_dev, unsigned int ch)
{
	int i;

	if (ch >= solo_dev->nr_chans)
		return -EINVAL;

	erase_on(solo_dev);

	for (i = 0; i < solo_dev->nr_chans; i++)
		solo_disp_ch(solo_dev, ch, i == ch ? 1 : 0);

	solo_dev->cur_ch = ch;

	return 0;
}

static void solo_cap_config(struct solo6010_dev *solo_dev)
{
	unsigned long height;
	unsigned long width;

	solo_reg_write(solo_dev, SOLO_CAP_BASE,
		       SOLO_CAP_MAX_PAGE(SOLO_CAP_EXT_MAX_PAGE *
					 solo_dev->nr_chans) |
		       SOLO_CAP_BASE_ADDR((SOLO_CAP_EXT_ADDR(solo_dev) >> 16) &
					  0xffff));
	solo_reg_write(solo_dev, SOLO_CAP_BTW,
		       (1 << 17) | SOLO_CAP_PROG_BANDWIDTH(2) |
		       SOLO_CAP_MAX_BANDWIDTH(CAPTURE_MAX_BANDWIDTH));

	/* Set scale 1, 9 dimension */
	width = solo_dev->video_hsize;
	height = solo_dev->video_vsize;
	solo_reg_write(solo_dev, SOLO_DIM_SCALE1,
		       SOLO_DIM_H_MB_NUM(width / 16) |
		       SOLO_DIM_V_MB_NUM_FRAME(height / 8) |
		       SOLO_DIM_V_MB_NUM_FIELD(height / 16));

	/* Set scale 2, 10 dimension */
	width = solo_dev->video_hsize / 2;
	height = solo_dev->video_vsize / 1;
	solo_reg_write(solo_dev, SOLO_DIM_SCALE2,
		       SOLO_DIM_H_MB_NUM(width / 16) |
		       SOLO_DIM_V_MB_NUM_FRAME(height / 8) |
		       SOLO_DIM_V_MB_NUM_FIELD(height / 16));

	/* Set scale 3, 11 dimension */
	width = solo_dev->video_hsize / 2;
	height = solo_dev->video_vsize / 2;
	solo_reg_write(solo_dev, SOLO_DIM_SCALE3,
		       SOLO_DIM_H_MB_NUM(width / 16) |
		       SOLO_DIM_V_MB_NUM_FRAME(height / 8) |
		       SOLO_DIM_V_MB_NUM_FIELD(height / 16));

	/* Set scale 4, 12 dimension */
	width = solo_dev->video_hsize / 3;
	height = solo_dev->video_vsize / 3;
	solo_reg_write(solo_dev, SOLO_DIM_SCALE4,
		       SOLO_DIM_H_MB_NUM(width / 16) |
		       SOLO_DIM_V_MB_NUM_FRAME(height / 8) |
		       SOLO_DIM_V_MB_NUM_FIELD(height / 16));

	/* Set scale 5, 13 dimension */
	width = solo_dev->video_hsize / 4;
	height = solo_dev->video_vsize / 2;
	solo_reg_write(solo_dev, SOLO_DIM_SCALE5,
		       SOLO_DIM_H_MB_NUM(width / 16) |
		       SOLO_DIM_V_MB_NUM_FRAME(height / 8) |
		       SOLO_DIM_V_MB_NUM_FIELD(height / 16));

	/* Progressive */
	width = VI_PROG_HSIZE;
	height = VI_PROG_VSIZE;
	solo_reg_write(solo_dev, SOLO_DIM_PROG,
		       SOLO_DIM_H_MB_NUM(width / 16) |
		       SOLO_DIM_V_MB_NUM_FRAME(height / 16) |
		       SOLO_DIM_V_MB_NUM_FIELD(height / 16));
}

static void solo_vin_config(struct solo6010_dev *solo_dev)
{
	solo_dev->vin_hstart = 8;
	solo_dev->vin_vstart = 2;

	solo_reg_write(solo_dev, SOLO_SYS_VCLK,
		       SOLO_VCLK_SELECT(2) |
		       SOLO_VCLK_VIN1415_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN1213_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN1011_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0809_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0607_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0405_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0203_DELAY(SOLO_VCLK_DELAY) |
		       SOLO_VCLK_VIN0001_DELAY(SOLO_VCLK_DELAY));

	solo_reg_write(solo_dev, SOLO_VI_ACT_I_P,
		       SOLO_VI_H_START(solo_dev->vin_hstart) |
		       SOLO_VI_V_START(solo_dev->vin_vstart) |
		       SOLO_VI_V_STOP(solo_dev->vin_vstart +
				      solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VI_ACT_I_S,
		       SOLO_VI_H_START(solo_dev->vout_hstart) |
		       SOLO_VI_V_START(solo_dev->vout_vstart) |
		       SOLO_VI_V_STOP(solo_dev->vout_vstart +
				      solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VI_ACT_P,
		       SOLO_VI_H_START(0) |
		       SOLO_VI_V_START(1) |
		       SOLO_VI_V_STOP(SOLO_PROGRESSIVE_VSIZE));

	solo_reg_write(solo_dev, SOLO_VI_CH_FORMAT,
		       SOLO_VI_FD_SEL_MASK(0) | SOLO_VI_PROG_MASK(0));

	/* XXX: Use this for stable check? */
	solo_reg_write(solo_dev, SOLO_VI_FMT_CFG, 0);
	solo_reg_write(solo_dev, SOLO_VI_CH_ENA, 0xffff);
	solo_reg_write(solo_dev, SOLO_VI_PAGE_SW, 2);

	if (solo_dev->video_type == 0) {
		solo_reg_write(solo_dev, SOLO_VI_PB_CONFIG,
			       SOLO_VI_PB_USER_MODE);
		solo_reg_write(solo_dev, SOLO_VI_PB_RANGE_HV,
			       SOLO_VI_PB_HSIZE(858) | SOLO_VI_PB_VSIZE(246));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_H,
			       SOLO_VI_PB_HSTART(16) |
			       SOLO_VI_PB_HSTOP(16 + 720));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_V,
			       SOLO_VI_PB_VSTART(4) |
			       SOLO_VI_PB_VSTOP(4 + 240));
	} else {
		solo_reg_write(solo_dev, SOLO_VI_PB_CONFIG,
			       SOLO_VI_PB_USER_MODE | SOLO_VI_PB_PAL);
		solo_reg_write(solo_dev, SOLO_VI_PB_RANGE_HV,
			       SOLO_VI_PB_HSIZE(864) | SOLO_VI_PB_VSIZE(294));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_H,
			       SOLO_VI_PB_HSTART(16) |
			       SOLO_VI_PB_HSTOP(16 + 720));
		solo_reg_write(solo_dev, SOLO_VI_PB_ACT_V,
			       SOLO_VI_PB_VSTART(4) |
			       SOLO_VI_PB_VSTOP(4 + 288));
	}

}

static void solo_disp_config(struct solo6010_dev *solo_dev)
{
	int i;

	solo_dev->vout_hstart = 6;
	solo_dev->vout_vstart = 8;

	solo_reg_write(solo_dev, SOLO_VO_BORDER_LINE_COLOR,
		       (0xa0 << 24) | (0x88 << 16) | (0xa0 << 8) | 0x88);
	solo_reg_write(solo_dev, SOLO_VO_BORDER_FILL_COLOR,
		       (0x10 << 24) | (0x8f << 16) | (0x10 << 8) | 0x8f);
	solo_reg_write(solo_dev, SOLO_VO_BKG_COLOR,
		       (16 << 24) | (128 << 16) | (16 << 8) | 128);
		       //SOLO_VO_BG_YUV(16, 128, 128));

	solo_reg_write(solo_dev, SOLO_VO_FMT_ENC,
		       solo_dev->video_type |
		       SOLO_VO_USER_COLOR_SET_NAV |
		       SOLO_VO_NA_COLOR_Y(0) |
		       SOLO_VO_NA_COLOR_CB(0) |
		       SOLO_VO_NA_COLOR_CR(0));

	solo_reg_write(solo_dev, SOLO_VO_ACT_H,
		       SOLO_VO_H_START(solo_dev->vout_hstart) |
		       SOLO_VO_H_STOP(solo_dev->vout_hstart +
				      solo_dev->video_hsize));

	solo_reg_write(solo_dev, SOLO_VO_ACT_V,
		       SOLO_VO_V_START(solo_dev->vout_vstart) |
		       SOLO_VO_V_STOP(solo_dev->vout_vstart +
				      solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VO_RANGE_HV,
		       SOLO_VO_H_LEN(solo_dev->video_hsize) |
		       SOLO_VO_V_LEN(solo_dev->video_vsize));

	solo_reg_write(solo_dev, SOLO_VI_WIN_SW, 5);

	solo_reg_write(solo_dev, SOLO_VO_DISP_CTRL, SOLO_VO_DISP_ON |
		       SOLO_VO_DISP_ERASE_COUNT(8) |
		       SOLO_VO_DISP_BASE(SOLO_DISP_EXT_ADDR(solo_dev)));

	erase_on(solo_dev);

	/* Mute channels we aren't supporting */
	for (i = solo_dev->nr_chans; i < 16; i++) {
		int val = ((~(1 << i) & 0xffff) &
			solo_reg_read(solo_dev, SOLO_VI_CH_ENA));
		solo_reg_write(solo_dev, SOLO_VI_CH_ENA, val);
	}

	/* Disable the watchdog */
	solo_reg_write(solo_dev, SOLO_WATCHDOG, 0);

	/* Test signal - BENC */
	//solo_reg_write(solo_dev, SOLO_VI_FMT_CFG, SOLO_VI_FMT_TEST_SIGNAL);
}

static int solo_disp_open(struct file *file)
{
	struct solo6010_dev *solo_dev = video_drvdata(file);
	struct solo_filehandle *fh;

	if ((fh = kzalloc(sizeof(*fh), GFP_KERNEL)) == NULL)
		return -ENOMEM;

	fh->solo_dev = solo_dev;
	file->private_data = fh;

	return 0;
}

/* Try to obtain and/or verify that a fh can read the display device. Only
 * one file descriptor can do this at a time and it retains exclusivity
 * until the file descriptor is closed. */
static int solo_disp_can_read(struct solo_filehandle *fh)
{
	struct solo6010_dev *solo_dev = fh->solo_dev;

	mutex_lock(&solo_dev->v4l2_mutex);
	if (solo_dev->v4l2_reader == NULL) 
		solo_dev->v4l2_reader = fh;
	mutex_unlock(&solo_dev->v4l2_mutex);

	if (solo_dev->v4l2_reader != fh)
		return 0;

	return 1;
}

/* If this file handle has exclusivity on read rights, release them */
static void solo_disp_free_read(struct solo_filehandle *fh)
{
	struct solo6010_dev *solo_dev = fh->solo_dev;

	mutex_lock(&solo_dev->v4l2_mutex);
	if (solo_dev->v4l2_reader == fh)
		solo_dev->v4l2_reader = NULL;
	mutex_unlock(&solo_dev->v4l2_mutex);
}

static ssize_t solo_disp_read(struct file *file, char __user *data,
			      size_t count, loff_t *ppos)
{
	struct solo_filehandle *fh = file->private_data;
	struct solo6010_dev *solo_dev = fh->solo_dev;
	unsigned int fdma_addr;
	int cur_write;
	int frame_size;
	int image_size = solo_image_size(solo_dev);
	int i, j;
	static int frames = 0;
	static int ch = 0;

	if (!solo_disp_can_read(fh))
		return -EBUSY;
	
	if (count < image_size)
		return -EINVAL;

	/* XXX: Is this really a good idea? */
	do {
		unsigned int status = solo_reg_read(solo_dev, SOLO_VI_STATUS0);
		cur_write = SOLO_VI_STATUS0_PAGE(status);
		if (cur_write != solo_dev->old_write)
			break;
		msleep_interruptible(1);
	} while(1);

	solo_dev->old_write = cur_write;

	if (erase_off(solo_dev)) {
		for (i = 0; i < image_size; i += 2) {
			u8 buf[2] = { 0x80, 0x80 };
			copy_to_user(data + i, buf, 2);
		}
		return image_size;
	}

	if (frames++ >= 150) {
		solo_disp_set_ch(solo_dev, ch);
		ch = ch ? 0 : 1;
		frames = 0;
	}

	frame_size = SOLO_H_SIZE_FDMA * solo_dev->video_vsize * 2;
	fdma_addr = SOLO_DISP_EXT_ADDR(solo_dev) + (cur_write * frame_size);

	for (i = 0; i < frame_size / SOLO_DISP_BUF_SIZE; i++) {
		if (solo_p2m_dma(solo_dev, SOLO_P2M_DMA_ID_DISP, 0,
				 solo_dev->vout_buf,
				 fdma_addr + (i * SOLO_DISP_BUF_SIZE),
				 SOLO_DISP_BUF_SIZE) < 0)
			return -EFAULT;
#ifdef COPY_WHOLE_LINE
		if (copy_to_user(data + (i * SOLO_DISP_BUF_SIZE),
				 solo_dev->vout_buf, SOLO_DISP_BUF_SIZE))
			return -EFAULT;
#else
		for (j = 0; j < (SOLO_DISP_BUF_SIZE / SOLO_H_SIZE_FDMA); j++) {
			int off = 2 * solo_dev->video_hsize *
			      ((i * SOLO_DISP_BUF_SIZE / SOLO_H_SIZE_FDMA) + j);
			if (copy_to_user(data + off, solo_dev->vout_buf +
					 (j * SOLO_H_SIZE_FDMA),
					 2 * solo_dev->video_hsize))
				return -EFAULT;
		}
#endif
	}

	return image_size;
}

static int solo_disp_release(struct file *file)
{
	struct solo_filehandle *fh = file->private_data;

	solo_disp_free_read(fh);
	kfree(fh);

	return 0;
}

static int solo_querycap(struct file *file, void  *priv,
			 struct v4l2_capability *cap)
{
	struct solo_filehandle  *fh  = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	strcpy(cap->driver, SOLO6010_NAME);
	strcpy(cap->card, "Softlogic 6010");
	snprintf(cap->bus_info, sizeof(cap->bus_info), "%s %s",
		 SOLO6010_NAME, pci_name(solo_dev->pdev));
	cap->version = SOLO6010_VER_NUM;
	cap->capabilities =     V4L2_CAP_VIDEO_CAPTURE |
				V4L2_CAP_READWRITE;
	return 0;
}

static int solo_enum_input(struct file *file, void *priv,
			   struct v4l2_input *input)
{
	struct solo_filehandle *fh  = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	if (input->index >= solo_dev->nr_chans)
		return -EINVAL;

	snprintf(input->name, sizeof(input->name), "Camera %d",
		 input->index + 1);
	input->type = V4L2_INPUT_TYPE_CAMERA;
	input->std = V4L2_STD_525_60 | V4L2_STD_625_50;
	/* XXX Should check for signal status on this camera */
	input->status = 0;

	return 0;
}

static int solo_set_input(struct file *file, void *priv, unsigned int index)
{
	struct solo_filehandle *fh = priv;

	return solo_disp_set_ch(fh->solo_dev, index);
}

static int solo_get_input(struct file *file, void *priv, unsigned int *index)
{
	struct solo_filehandle *fh = priv;

	*index = fh->solo_dev->cur_ch;

	return 0;
}

static int solo_enum_fmt_cap(struct file *file, void *priv,
			     struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = SOLO_DISP_PIX_FORMAT;
	snprintf(f->description, sizeof(f->description),
		 "%s", "YUV 4:2:2 Packed");

	return 0;
}

static int solo_try_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct solo_filehandle *fh = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;
	int image_size = solo_image_size(solo_dev);

	/* Check supported sizes */
	if (pix->width > solo_dev->video_hsize)
		pix->width = solo_dev->video_hsize;
	if (pix->height > solo_dev->video_vsize * 2)
		pix->width = solo_dev->video_vsize * 2;
	if (pix->sizeimage > image_size)
		pix->sizeimage = image_size;

	if (pix->width     != solo_dev->video_hsize ||
	    pix->height    != solo_dev->video_vsize * 2 ||
	    pix->sizeimage != image_size)
		return -EINVAL;

	/* Check formats */
	if (pix->field == V4L2_FIELD_ANY)
		pix->field = SOLO_DISP_PIX_FIELD;

	if (pix->pixelformat != SOLO_DISP_PIX_FORMAT ||
	    pix->field       != SOLO_DISP_PIX_FIELD ||
	    pix->colorspace  != V4L2_COLORSPACE_SMPTE170M)
		return -EINVAL;

	return 0;
}

static int solo_set_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct solo_filehandle *fh = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;

	/* If there is currently a reader, we do not change the format */
	if (solo_dev->v4l2_reader != NULL)
		return -EBUSY;

	/* For right now, if it doesn't match our running config,
	 * then fail */
	return solo_try_fmt_cap(file, priv, f);
}

static int solo_get_fmt_cap(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct solo_filehandle *fh = priv;
	struct solo6010_dev *solo_dev = fh->solo_dev;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->width = solo_dev->video_hsize;
	pix->height = solo_dev->video_vsize * 2;
	pix->pixelformat = SOLO_DISP_PIX_FORMAT;
	pix->field = SOLO_DISP_PIX_FIELD;
	pix->sizeimage = solo_image_size(solo_dev);
#ifdef COPY_WHOLE_LINE
	pix->bytesperline = SOLO_H_SIZE_FDMA;
#else
	pix->bytesperline = solo_dev->video_hsize * 2;
#endif
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static const struct v4l2_file_operations solo_disp_fops = {
	.owner			= THIS_MODULE,
	.open			= solo_disp_open,
	.release		= solo_disp_release,
	.read			= solo_disp_read,
	.ioctl			= video_ioctl2,
};

static const struct v4l2_ioctl_ops solo_disp_ioctl_ops = {
	.vidioc_querycap		= solo_querycap,
	/* Input callbacks */
	.vidioc_enum_input		= solo_enum_input,
	.vidioc_s_input			= solo_set_input,
	.vidioc_g_input			= solo_get_input,
	/* Video capture format callbacks */
	.vidioc_enum_fmt_vid_cap	= solo_enum_fmt_cap,
	.vidioc_try_fmt_vid_cap		= solo_try_fmt_cap,
	.vidioc_s_fmt_vid_cap		= solo_set_fmt_cap,
	.vidioc_g_fmt_vid_cap		= solo_get_fmt_cap,
};

static struct video_device solo_disp_template = {
	.name			= SOLO6010_NAME,
	.fops			= &solo_disp_fops,
	.ioctl_ops		= &solo_disp_ioctl_ops,
	.minor			= -1,
	.release		= video_device_release,

	.tvnorms		= V4L2_STD_525_60 | V4L2_STD_625_50,
	.current_norm		= V4L2_STD_NTSC_M,
};

static void solo_v4l2_remove(struct solo6010_dev *solo_dev)
{
	video_unregister_device(solo_dev->vfd);
	solo_dev->vfd = NULL;
}

int solo_v4l2_init(struct solo6010_dev *solo_dev)
{
	int ret;

	mutex_init(&solo_dev->v4l2_mutex);

	solo_dev->vfd = video_device_alloc();
	if (!solo_dev->vfd)
		return -ENOMEM;

	*solo_dev->vfd = solo_disp_template;
	solo_dev->vfd->parent = &solo_dev->pdev->dev;

	ret = video_register_device(solo_dev->vfd, VFL_TYPE_GRABBER, video_nr);
	if (ret < 0) {
		video_device_release(solo_dev->vfd);
		solo_dev->vfd = NULL;
		return ret;
	}

	video_set_drvdata(solo_dev->vfd, solo_dev);

	snprintf(solo_dev->vfd->name, sizeof(solo_dev->vfd->name), "%s (%i)",
		 solo_disp_template.name, solo_dev->vfd->num);

	if (video_nr >= 0)
		video_nr++;

	dev_info(&solo_dev->pdev->dev, "Registered as /dev/video%d with "
		 "%d inputs\n", solo_dev->vfd->num, solo_dev->nr_chans);

	/* Start out with NTSC */
	solo_dev->video_type = SOLO_VO_FMT_TYPE_NTSC;
	solo_dev->video_hsize = 704;
	solo_dev->video_vsize = 240;

	solo_vin_config(solo_dev);
	solo_disp_config(solo_dev);
	solo_cap_config(solo_dev);

	/* Set the default display channel */
	solo_disp_set_ch(solo_dev, SOLO_DEFAULT_CHAN);

	return 0;
}

void solo_v4l2_exit(struct solo6010_dev *solo_dev)
{
	solo_v4l2_remove(solo_dev);
}
