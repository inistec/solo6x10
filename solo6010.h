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

#ifndef __SOLO6010_H
#define __SOLO6010_H

#include <linux/pci.h>
#include <linux/i2c.h>
#include <linux/semaphore.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <asm/io.h>
#include <linux/videodev2.h>

#include "solo6010-registers.h"

#define PCI_VENDOR_ID_SOFTLOGIC		0x9413
#define PCI_DEVICE_ID_SOLO6010		0x6010

#define SOLO6010_NAME			"solo6010"

/* Make sure these two match */
#define SOLO6010_VERSION		"0.1.0"
#define SOLO6010_VER_MAJOR		0
#define SOLO6010_VER_MINOR		1
#define SOLO6010_VER_SUB		0
#define SOLO6010_VER_NUM \
    KERNEL_VERSION(SOLO6010_VER_MAJOR, SOLO6010_VER_MINOR, SOLO6010_VER_SUB)

/* Stock runtime parameters */
#define SOLO_CLOCK_MHZ			108

/*
 * The SOLO6010 actually has 8 i2c channels, but we only use 2.
 * 0 - Techwell chip(s)
 * 1 - SAA7128 (only if we don't have tw2865)
 */
#define SOLO_I2C_ADAPTERS		2
#define SOLO_I2C_TW			0
#define SOLO_I2C_SAA			1

/* DMA Engine setup */
#define SOLO_NR_P2M			4
#define SOLO_NR_P2M_DESC		256
#define SOLO_P2M_DESC_SIZE		(SOLO_NR_P2M_DESC * 16)
/* MPEG and JPEG share the same interrupt and locks so they must be together
 * in the same dma channel. */
#define SOLO_P2M_DMA_ID_MP4E		0
#define SOLO_P2M_DMA_ID_JPEG		0
#define SOLO_P2M_DMA_ID_MP4D		1
#define SOLO_P2M_DMA_ID_G723D		1
#define SOLO_P2M_DMA_ID_DISP		2
#define SOLO_P2M_DMA_ID_OSG		2
#define SOLO_P2M_DMA_ID_G723E		3
#define SOLO_P2M_DMA_ID_VIN		3

#define SOLO_DISP_BUF_SIZE		(64 * 1024) // 64k
#define SOLO_VCLK_DELAY			3
#define SOLO_PROGRESSIVE_VSIZE		1024

enum SOLO_I2C_STATE {
	IIC_STATE_IDLE,
	IIC_STATE_START,
	IIC_STATE_READ,
	IIC_STATE_WRITE,
	IIC_STATE_STOP
};

struct solo_p2m_dev {
	struct semaphore	sem;
	struct completion	completion;
	int			error;
	u8			desc[SOLO_P2M_DESC_SIZE];
};

/* The SOLO6010 PCI Device */
struct solo6010_dev {
	/* General stuff */
	struct pci_dev		*pdev;
	u8 __iomem		*reg_base;
	int			nr_chans;
	u32			irq_mask;
	spinlock_t		reg_io_lock;

	/* tw28xx accounting */
	u8			tw2864, tw2865, tw2815;
	u8			tw28_cnt;

	/* i2c related items */
	struct i2c_adapter	i2c_adap[SOLO_I2C_ADAPTERS];
	enum SOLO_I2C_STATE	i2c_state;
	struct semaphore	i2c_sem;
	int			i2c_id;
	wait_queue_head_t	i2c_wait;
	struct i2c_msg		*i2c_msg;
	unsigned int		i2c_msg_num;
	unsigned int		i2c_msg_ptr;

	/* P2M DMA Engine */
	struct solo_p2m_dev	p2m_dev[SOLO_NR_P2M];

	/* V4L2 items */
	struct video_device	*vfd;
	unsigned int		erasing;
	unsigned int		frame_blank;

	/* Current video settings */
	u8 			video_type;
	u16			video_hsize, video_vsize;
	u16			vout_hstart, vout_vstart;
	u16			vin_hstart, vin_vstart;
	u8			cur_ch;
};

static inline u32 solo_reg_read(struct solo6010_dev *solo_dev, int reg)
{
	unsigned long flags;
	u32 ret;
	u16 val;

	spin_lock_irqsave(&solo_dev->reg_io_lock, flags);

	ret = readl(solo_dev->reg_base + reg);
	rmb();
	pci_read_config_word(solo_dev->pdev, PCI_STATUS, &val);
	rmb();

	spin_unlock_irqrestore(&solo_dev->reg_io_lock, flags);

	return ret;
}

static inline void solo_reg_write(struct solo6010_dev *solo_dev, int reg,
				      u32 data)
{
	unsigned long flags;
	u16 val;

	spin_lock_irqsave(&solo_dev->reg_io_lock, flags);

	writel(data, solo_dev->reg_base + reg);
	wmb();
	pci_read_config_word(solo_dev->pdev, PCI_STATUS, &val);
	rmb();

	spin_unlock_irqrestore(&solo_dev->reg_io_lock, flags);
}

void solo6010_irq_on(struct solo6010_dev *solo_dev, u32 mask);
void solo6010_irq_off(struct solo6010_dev *solo_dev, u32 mask);

/* Init/exit routeines for subsystems */
int solo_disp_init(struct solo6010_dev *solo_dev);
void solo_disp_exit(struct solo6010_dev *solo_dev);

int solo_gpio_init(struct solo6010_dev *solo_dev);
void solo_gpio_exit(struct solo6010_dev *solo_dev);

int solo_i2c_init(struct solo6010_dev *solo_dev);
void solo_i2c_exit(struct solo6010_dev *solo_dev);

int solo_p2m_init(struct solo6010_dev *solo_dev);
void solo_p2m_exit(struct solo6010_dev *solo_dev);

int solo_v4l2_init(struct solo6010_dev *solo_dev);
void solo_v4l2_exit(struct solo6010_dev *solo_dev);

/* i2c and p2m(dma) routines */
int solo_i2c_isr(struct solo6010_dev *solo_dev);

u8 solo_i2c_readbyte(struct solo6010_dev *solo_dev, int id, u8 addr, u8 off);
void solo_i2c_writebyte(struct solo6010_dev *solo_dev, int id, u8 addr, u8 off,
			u8 data);

void solo_p2m_isr(struct solo6010_dev *solo_dev, int id);
void solo_p2m_error_isr(struct solo6010_dev *solo_dev, u32 status);

int solo_p2m_dma_t(struct solo6010_dev *solo_dev, int id, int wr,
		   dma_addr_t dma_addr, u32 ext_addr, u32 size);
int solo_p2m_dma(struct solo6010_dev *solo_dev, int id, int wr,
		 void *sys_addr, u32 ext_addr, u32 size);

#endif /* __SOLO6010_H */
