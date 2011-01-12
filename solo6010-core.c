/*
 * Copyright (C) 2010 Bluecherry, LLC www.bluecherrydvr.com
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
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/videodev2.h>

#include "solo6010.h"
#include "solo6010-tw28.h"

MODULE_DESCRIPTION("Softlogic 6010 MP4 Encoder/Decoder V4L2/ALSA Driver");
MODULE_AUTHOR("Ben Collins <bcollins@bluecherry.net>");
MODULE_VERSION(SOLO6010_VERSION);
MODULE_LICENSE("GPL");

void solo6010_irq_on(struct solo6010_dev *solo_dev, u32 mask)
{
	solo_dev->irq_mask |= mask;
	solo_reg_write(solo_dev, SOLO_IRQ_ENABLE, solo_dev->irq_mask);
}

void solo6010_irq_off(struct solo6010_dev *solo_dev, u32 mask)
{
	solo_dev->irq_mask &= ~mask;
	solo_reg_write(solo_dev, SOLO_IRQ_ENABLE, solo_dev->irq_mask);
}

static irqreturn_t solo6010_isr(int irq, void *data)
{
	struct solo6010_dev *solo_dev = data;
	u32 status;
	int i;

	status = solo_reg_read(solo_dev, SOLO_IRQ_STAT);
	if (!status)
		return IRQ_NONE;

	if (status & ~solo_dev->irq_mask) {
		solo_reg_write(solo_dev, SOLO_IRQ_STAT,
			       status & ~solo_dev->irq_mask);
		status &= solo_dev->irq_mask;
	}

	if (status & SOLO_IRQ_PCI_ERR) {
		u32 err = solo_reg_read(solo_dev, SOLO_PCI_ERR);
		solo_p2m_error_isr(solo_dev, err);
	}

	for (i = 0; i < SOLO_NR_P2M; i++)
		if (status & SOLO_IRQ_P2M(i))
			solo_p2m_isr(solo_dev, i);

	if (status & SOLO_IRQ_IIC)
		solo_i2c_isr(solo_dev);

	if (status & SOLO_IRQ_VIDEO_IN)
		solo_video_in_isr(solo_dev);

	if (status & SOLO_IRQ_ENCODER)
		solo_enc_v4l2_isr(solo_dev);

	if (status & SOLO_IRQ_G723)
		solo_g723_isr(solo_dev);

	/* Clear all interrupts handled */
	solo_reg_write(solo_dev, SOLO_IRQ_STAT, status);

	return IRQ_HANDLED;
}

static void free_solo_dev(struct solo6010_dev *solo_dev)
{
	struct pci_dev *pdev;

	if (!solo_dev)
		return;

	device_unregister(&solo_dev->dev);

	pdev = solo_dev->pdev;

	/* If we never initialized the PCI device, then nothing else
	 * below here needs cleanup */
	if (!pdev) {
		kfree(solo_dev);
		return;
	}

	/* Bring down the sub-devices first */
	solo_g723_exit(solo_dev);
	solo_enc_v4l2_exit(solo_dev);
	solo_enc_exit(solo_dev);
	solo_v4l2_exit(solo_dev);
	solo_disp_exit(solo_dev);
	solo_gpio_exit(solo_dev);
	solo_p2m_exit(solo_dev);
	solo_i2c_exit(solo_dev);

	/* Now cleanup the PCI device */
	if (solo_dev->reg_base) {
		solo6010_irq_off(solo_dev, ~0);
		pci_iounmap(pdev, solo_dev->reg_base);
		free_irq(pdev->irq, solo_dev);
	}

	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);

	kfree(solo_dev);
}

static ssize_t solo_set_eeprom(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct solo6010_dev *solo_dev =
		container_of(dev, struct solo6010_dev, dev);
	unsigned short *p = (unsigned short *)buf;
	int i;

	if (count != 128)
		return -EINVAL;

	solo_eeprom_ewen(solo_dev, 1);

	for (i = 0; i < 64; i++)
		solo_eeprom_write(solo_dev, i, p[i]);

	solo_eeprom_ewen(solo_dev, 0);

        return count;
}
static ssize_t solo_get_eeprom(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct solo6010_dev *solo_dev =
		container_of(dev, struct solo6010_dev, dev);
	unsigned short *p = (unsigned short *)buf;
	int i;

	for (i = 0; i < 64; i++)
		p[i] = solo_eeprom_read(solo_dev, i);

	return 128;
}
static DEVICE_ATTR(eeprom, S_IWUSR | S_IRUGO, solo_get_eeprom, solo_set_eeprom);

static struct device_attribute *const solo_dev_attrs[] = {
	&dev_attr_eeprom,
};

static void solo_device_release(struct device *dev)
{
	/* Do nothing */
}

static int __devinit solo_sysfs_init(struct solo6010_dev *solo_dev)
{
	struct device *dev = &solo_dev->dev;
	int i;

	dev->release = solo_device_release;
	dev->parent = &solo_dev->pdev->dev;
	set_dev_node(dev, dev_to_node(&solo_dev->pdev->dev));
	dev_set_name(dev, "solo6x10");

	if (device_register(dev))
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(solo_dev_attrs); i++) {
		if (device_create_file(dev, solo_dev_attrs[i])) {
			device_unregister(dev);
			return -ENOMEM;
		}
	}

	return 0;
}

static int __devinit solo6010_pci_probe(struct pci_dev *pdev,
					const struct pci_device_id *id)
{
	struct solo6010_dev *solo_dev;
	int ret;
	int sdram;
	u8 chip_id;

	if ((solo_dev = kzalloc(sizeof(*solo_dev), GFP_KERNEL)) == NULL)
		return -ENOMEM;

	solo_dev->pdev = pdev;
	rwlock_init(&solo_dev->reg_io_lock);
	pci_set_drvdata(pdev, solo_dev);

	if ((ret = pci_enable_device(pdev)))
		goto fail_probe;

	pci_set_master(pdev);

	/* RETRY/TRDY Timeout disabled */
	pci_write_config_byte(pdev, 0x40, 0x00);
	pci_write_config_byte(pdev, 0x41, 0x00);

	if ((ret = pci_request_regions(pdev, SOLO6010_NAME)))
		goto fail_probe;

	if ((solo_dev->reg_base = pci_ioremap_bar(pdev, 0)) == NULL) {
		ret = -ENOMEM;
		goto fail_probe;
	}

	chip_id = solo_reg_read(solo_dev, SOLO_CHIP_OPTION) &
				SOLO_CHIP_ID_MASK;
	switch (chip_id) {
		case 7:
			solo_dev->nr_chans = 16;
			solo_dev->nr_ext = 5;
			break;
		case 6:
			solo_dev->nr_chans = 8;
			solo_dev->nr_ext = 2;
			break;
		default:
			dev_warn(&pdev->dev, "Invalid chip_id 0x%02x, "
				 "defaulting to 4 channels\n",
				 chip_id);
		case 5:
			solo_dev->nr_chans = 4;
			solo_dev->nr_ext = 1;
	}

	/* Disable all interrupts to start */
	solo6010_irq_off(solo_dev, ~0);

	/* Initial global settings */
	solo_reg_write(solo_dev, SOLO_SYS_CFG, SOLO_SYS_CFG_SDRAM64BIT |
		       SOLO_SYS_CFG_INPUTDIV(25) |
		       SOLO_SYS_CFG_FEEDBACKDIV((SOLO_CLOCK_MHZ * 2) - 2) |
		       SOLO_SYS_CFG_OUTDIV(3));
	solo_reg_write(solo_dev, SOLO_TIMER_CLOCK_NUM, SOLO_CLOCK_MHZ - 1);

	/* PLL locking time of 1ms */
	mdelay(1);

	ret = request_irq(pdev->irq, solo6010_isr, IRQF_SHARED, SOLO6010_NAME,
			  solo_dev);
	if (ret)
		goto fail_probe;

	/* Handle this from the start */
	solo6010_irq_on(solo_dev, SOLO_IRQ_PCI_ERR);

	if ((ret = solo_i2c_init(solo_dev)))
		goto fail_probe;

	/* Setup the DMA engine */
	sdram = (solo_dev->nr_chans >= 8) ? 2 : 1;
	solo_reg_write(solo_dev, SOLO_DMA_CTRL,
		       SOLO_DMA_CTRL_REFRESH_CYCLE(1) |
		       SOLO_DMA_CTRL_SDRAM_SIZE(sdram) |
		       SOLO_DMA_CTRL_SDRAM_CLK_INVERT |
		       SOLO_DMA_CTRL_READ_CLK_SELECT |
		       SOLO_DMA_CTRL_LATENCY(1));

	if ((ret = solo_sysfs_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_p2m_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_disp_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_gpio_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_tw28_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_v4l2_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_enc_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_enc_v4l2_init(solo_dev)))
		goto fail_probe;

	if ((ret = solo_g723_init(solo_dev)))
		goto fail_probe;

	return 0;

fail_probe:
	free_solo_dev(solo_dev);
	return ret;
}

static void __devexit solo6010_pci_remove(struct pci_dev *pdev)
{
	struct solo6010_dev *solo_dev = pci_get_drvdata(pdev);

	free_solo_dev(solo_dev);
}

static struct pci_device_id solo6010_id_table[] = {
	/* 6010 based cards */
	{PCI_DEVICE(PCI_VENDOR_ID_SOFTLOGIC, PCI_DEVICE_ID_SOLO6010)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_NEUSOLO_4)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_NEUSOLO_9)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_NEUSOLO_16)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_BC_SOLO_4)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_BC_SOLO_9)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_BC_SOLO_16)},
	/* 6110 based cards */
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_BC_6110_4)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_BC_6110_8)},
	{PCI_DEVICE(PCI_VENDOR_ID_BLUECHERRY, PCI_DEVICE_ID_BC_6110_16)},
	{0,}
};

MODULE_DEVICE_TABLE(pci, solo6010_id_table);

static struct pci_driver solo6010_pci_driver = {
	.name = SOLO6010_NAME,
	.id_table = solo6010_id_table,
	.probe = solo6010_pci_probe,
	.remove = solo6010_pci_remove,
};

static int __init solo6010_module_init(void)
{
	return pci_register_driver(&solo6010_pci_driver);
}

static void __exit solo6010_module_exit(void)
{
	pci_unregister_driver(&solo6010_pci_driver);
}

module_init(solo6010_module_init);
module_exit(solo6010_module_exit);
