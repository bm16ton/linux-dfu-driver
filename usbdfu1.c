/*
 * usbdfu1.c
 *
 * Copyright (c) 2017 Dashi Cao        <dscao999@hotmail.com, caods1@lenovo.com>
 *
 * USB Device Firmware Upgrade support for TI Tiva C series
 *
 */
#include <stdarg.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/dma-mapping.h>
#include <linux/uaccess.h>
#include "usbdfu1.h"
#include <asm/uaccess.h>

#define BLKSIZE	1024
#define DFUDEV_NAME "dfu"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dashi Cao");
MODULE_DESCRIPTION("Tiva C USB DFU Driver");

#define USB_VENDOR_LUMINARY 0x1cbe
#define USB_PRODUCT_STELLARIS_DFU 0x0ff

#define USB_VENDOR_BMP 0x1d50
#define USB_PRODUCT_BMP 0x6018

#define USB_VENDOR_BMPDFU 0x1d50
#define USB_PRODUCT_BMPDFU 0x6017

static int max_dfus = 8;
module_param(max_dfus, int, 0644);
MODULE_PARM_DESC(max_dfus, "Maximum number of USB DFU devices. "
	"Default: 8");

static int urb_timeout = 200; /* milliseconds */
module_param(urb_timeout, int, 0644);
MODULE_PARM_DESC(urb_timeout, "USB urb completion timeout. "
	"Default: 200 milliseconds.");

static const struct usb_device_id dfu_ids[] = {
	{ USB_DFU_INTERFACE_INFO(USB_VENDOR_BMP,
		USB_CLASS_APP_SPEC, USB_DFU_SUBCLASS, USB_DFU_PROTO_DFUMODE) },
	{ USB_DFU_INTERFACE_INFO(USB_VENDOR_BMPDFU,
		USB_CLASS_APP_SPEC, USB_DFU_SUBCLASS, USB_DFU_PROTO_DFUMODE) },
	{ USB_DFU_INTERFACE_INFO(USB_VENDOR_LUMINARY,
		USB_CLASS_APP_SPEC, USB_DFU_SUBCLASS, USB_DFU_PROTO_DFUMODE) },
	{ }
};
MODULE_DEVICE_TABLE(usb, dfu_ids);

static dev_t dfu_devno;
static struct class *dfu_class;

static inline int dfu_abort(struct dfu_control *ctrl)
{
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_ABORT;
	ctrl->req.wIndex = cpu_to_le16(ctrl->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = 0;
	ctrl->pipe = usb_sndctrlpipe(ctrl->usbdev, 0);
	ctrl->datbuf = NULL;
	ctrl->len = 0;
	return dfu_submit_urb(ctrl, urb_timeout);
}

static inline int dfu_get_status(struct dfu_control *ctrl)
{
	ctrl->req.bRequestType = 0xa1;
	ctrl->req.bRequest = USB_DFU_GETSTATUS;
	ctrl->req.wIndex = cpu_to_le16(ctrl->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = cpu_to_le16(6);
	ctrl->pipe = usb_rcvctrlpipe(ctrl->usbdev, 0);
	ctrl->datbuf = &ctrl->dfuStatus;
	ctrl->len = 6;
	return dfu_submit_urb(ctrl, urb_timeout);
}

static inline int dfu_get_state(struct dfu_control *ctrl)
{
	int retv;

	ctrl->req.bRequestType = 0xa1;
	ctrl->req.bRequest = USB_DFU_GETSTATE;
	ctrl->req.wIndex = cpu_to_le16(ctrl->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = cpu_to_le16(1);
	ctrl->pipe = usb_rcvctrlpipe(ctrl->usbdev, 0);
	ctrl->datbuf = &ctrl->dfuState;
	ctrl->len = 1;
	retv = dfu_submit_urb(ctrl, urb_timeout);
	if (retv == 0)
		retv = ctrl->dfuState;
	return retv;
}

static inline int dfu_clr_status(struct dfu_control *ctrl)
{
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_CLRSTATUS;
	ctrl->req.wIndex = cpu_to_le16(ctrl->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = 0;
	ctrl->pipe = usb_sndctrlpipe(ctrl->usbdev, 0);
	ctrl->datbuf = NULL;
	ctrl->len = 0;
	return dfu_submit_urb(ctrl, urb_timeout);
}

static inline int dfu_finish_dnload(struct dfu_control *ctrl)
{
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_DNLOAD;
	ctrl->req.wIndex = cpu_to_le16(ctrl->intfnum);
	ctrl->req.wValue = 0;
	ctrl->req.wLength = 0;
	ctrl->pipe = usb_sndctrlpipe(ctrl->usbdev, 0);
	ctrl->datbuf = NULL;
	ctrl->len = 0;
	return dfu_submit_urb(ctrl, urb_timeout);
}

static inline unsigned int altrim(unsigned int v, int sf)
{
	return (((v -1) >> sf) + 1) << sf;
}

static int dfu_open(struct inode *inode, struct file *filp)
{
	struct dfu1_device *dfudev;
	int state, retv, buflen;
	struct dfu_control *ctrl;
	void *datbuf;

	retv = 0;
	dfudev = container_of(inode->i_cdev, struct dfu1_device, cdev);
	filp->private_data = dfudev;
	if (mutex_lock_interruptible(&dfudev->lock))
		return -EBUSY;

	buflen = altrim(dfudev->xfersize, 4) + 2*sizeof(struct dfu_control);
	datbuf = kmalloc(buflen, GFP_KERNEL);
	if (!datbuf) {
		retv = -ENOMEM;
		goto err_10;
	}
	dfudev->datbuf = datbuf;
	dfudev->opctrl = datbuf + altrim(dfudev->xfersize, 4);
	dfudev->opctrl->datbuf = datbuf;
	dfudev->opctrl->dfurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dfudev->opctrl->dfurb) {
		retv = -ENOMEM;
		goto err_20;
	}
	dfudev->opctrl->usbdev = dfudev->usbdev;
	dfudev->opctrl->intf = dfudev->intf;
	dfudev->opctrl->intfnum = dfudev->intfnum;

	dfudev->stctrl = dfudev->opctrl + 1;
	dfudev->stctrl->datbuf = NULL;
	dfudev->stctrl->dfurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dfudev->stctrl->dfurb) {
		retv = -ENOMEM;
		goto err_25;
	}
	dfudev->stctrl->usbdev = dfudev->usbdev;
	dfudev->stctrl->intf = dfudev->intf;
	dfudev->stctrl->intfnum = dfudev->intfnum;

	ctrl = dfudev->stctrl;
	state = dfu_get_state(ctrl);
	if (state != dfuIDLE) {
		dev_err(&dfudev->intf->dev, "Bad Initial State: %d\n", state);
		retv =  -EBUSY;
		goto err_30;
	}

	return retv;

err_30:
	usb_free_urb(dfudev->stctrl->dfurb);
err_25:
	usb_free_urb(dfudev->opctrl->dfurb);
err_20:
	kfree(datbuf);
err_10:
	mutex_unlock(&dfudev->lock);
	return retv;
}

static int dfu_release(struct inode *inode, struct file *filp)
{
	struct dfu1_device *dfudev;
	struct dfu_control *stctrl;
	int retv;

	dfudev = filp->private_data;
	stctrl = dfudev->stctrl;
	retv = dfu_get_state(stctrl);
	if (retv == dfuDNLOAD_IDLE)
		dfu_finish_dnload(stctrl);
	else if (retv == dfuERROR)
		dfu_clr_status(stctrl);
	else if (retv != dfuIDLE)
		dfu_abort(stctrl);
	msleep(100);
	retv = dfu_get_state(stctrl);
	if (retv != dfuIDLE)
		dev_err(&dfudev->intf->dev, "Need Reset! Stuck in State: %d\n",
				retv);
	usb_free_urb(stctrl->dfurb);
	usb_free_urb(dfudev->opctrl->dfurb);
	kfree(dfudev->datbuf);
	mutex_unlock(&dfudev->lock);
	filp->private_data = NULL;
	return 0;
}

static ssize_t dfu_upload(struct file *filp, char __user *buff, size_t count,
			loff_t *f_pos)
{
	struct dfu1_device *dfudev;
	int blknum, numb;
	struct dfu_control *opctrl, *stctrl;
	int dfust, dma, len;
	dma_addr_t dmabuf;
	struct device *ctrler;

	if (count == 0)
		return 0;
	dfudev = filp->private_data;
	opctrl = dfudev->opctrl;
	stctrl = dfudev->stctrl;
	dfust = dfu_get_state(stctrl);
	if (dfust != dfuIDLE && dfust != dfuUPLOAD_IDLE) {
		dev_err(&dfudev->intf->dev, "Inconsistent State: %d\n", dfust);
		return -EINVAL;
	}
	if (*f_pos != 0 && dfust == dfuIDLE)
		return 0;

	if (!access_ok(buff, count))
		return -EFAULT;

	ctrler = dfudev->usbdev->bus->controller;
	dmabuf = ~0;
	dma = 0;
	opctrl->req.bRequestType = 0xa1;
	opctrl->req.bRequest = USB_DFU_UPLOAD;
	opctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	opctrl->req.wLength = cpu_to_le16(dfudev->xfersize);
	opctrl->pipe = usb_rcvctrlpipe(dfudev->usbdev, 0);
	opctrl->len = dfudev->xfersize;
	if (dfudev->dma) {
		dmabuf = dma_map_single(ctrler, opctrl->datbuf,
				dfudev->xfersize, DMA_FROM_DEVICE);
		if (!dma_mapping_error(ctrler, dmabuf)) {
			dma = 1;
			opctrl->dfurb->transfer_flags |=
						URB_NO_TRANSFER_DMA_MAP;
			opctrl->dfurb->transfer_dma = dmabuf;
		} else
			dev_warn(&dfudev->intf->dev,
					"Cannot map DMA address\n");
	}

	blknum = *f_pos / BLKSIZE;
	numb = 0;
	do {
		opctrl->req.wValue = cpu_to_le16(blknum);
		if (dfu_submit_urb(opctrl, urb_timeout) ||
				dfu_get_status(stctrl))
			break;
		dfust = stctrl->dfuStatus.bState;
		if (dfust != dfuUPLOAD_IDLE && dfust != dfuIDLE) {
			dev_err(&dfudev->intf->dev,
				"Uploading failed. DFU State: %d\n", dfust);
			break;
		}
		len = READ_ONCE(opctrl->nxfer);
		if (len == 0)
			break;
		*f_pos += len;
		blknum = *f_pos / BLKSIZE;
		dma_sync_single_for_cpu(ctrler, dmabuf, len, DMA_FROM_DEVICE);
		if (copy_to_user(buff+numb, opctrl->datbuf, len)) {
			dev_err(&dfudev->intf->dev,
				"Failed to copy data into user space!\n");
			break;
		}
		numb += len;
	} while (numb < count && dfust == dfuUPLOAD_IDLE);

	if (dma)
		dma_unmap_single(ctrler, dmabuf, dfudev->xfersize,
				DMA_FROM_DEVICE);

	return numb;
}

static ssize_t dfu_dnload(struct file *filp, const char __user *buff,
			size_t count, loff_t *f_pos)
{
	struct dfu1_device *dfudev;
	int blknum, numb, fpos;
	struct dfu_control *opctrl, *stctrl;
	int dfust, dma, len, lenrem, tmout;
	dma_addr_t dmabuf;
	struct device *ctrler;

	if (count == 0)
		return 0;
	dfudev = filp->private_data;
	opctrl = dfudev->opctrl;
	stctrl = dfudev->stctrl;
	dfust = dfu_get_state(stctrl);
	if (dfust != dfuIDLE && dfust != dfuDNLOAD_IDLE) {
		dev_err(&dfudev->intf->dev, "Inconsistent State: %d\n", dfust);
		return -EINVAL;
	}
	if (!access_ok(buff, count))
		return -EFAULT;

	ctrler = dfudev->usbdev->bus->controller;
	dmabuf = ~0;
	dma = 0;
	opctrl->req.bRequestType = 0x21;
	opctrl->req.bRequest = USB_DFU_DNLOAD;
	opctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	opctrl->pipe = usb_sndctrlpipe(dfudev->usbdev, 0);
	if (dfudev->dma) {
		dmabuf = dma_map_single(ctrler, opctrl->datbuf, dfudev->xfersize,
					DMA_TO_DEVICE);
		if (!dma_mapping_error(ctrler, dmabuf)) {
			dma = 1;
			opctrl->dfurb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
			opctrl->dfurb->transfer_dma = dmabuf;
		} else
			dev_warn(&dfudev->intf->dev,
					"Cannot map DMA address\n");
	}

	fpos = *f_pos;
	blknum = fpos / BLKSIZE;
	lenrem = count;
	numb = 0;
	do {
		opctrl->len = dfudev->xfersize > lenrem ? lenrem :
							dfudev->xfersize;
		if (copy_from_user(dfudev->datbuf, buff+numb, opctrl->len))
			break;
		opctrl->req.wValue = cpu_to_le16(blknum);
		opctrl->req.wLength = cpu_to_le16(opctrl->len);
		dma_sync_single_for_device(ctrler, dmabuf, opctrl->len,
						DMA_TO_DEVICE);
		if (dfu_submit_urb(opctrl, urb_timeout) ||
				dfu_get_status(stctrl))
			break;
		len = READ_ONCE(opctrl->nxfer);
		if (len == 0)
			break;
		numb += len;
		lenrem -= len;
		fpos += len;
		blknum = fpos / BLKSIZE;
		while (stctrl->dfuStatus.bState == dfuDNLOAD_BUSY) {
			tmout = stctrl->dfuStatus.wmsec[0] |
					(stctrl->dfuStatus.wmsec[1] << 8) |
					(stctrl->dfuStatus.wmsec[2] << 16);
			msleep(tmout);
			if (dfu_get_status(stctrl))
				break;
		}
		if (stctrl->dfuStatus.bState != dfuDNLOAD_IDLE &&
		    stctrl->dfuStatus.bState != dfuIDLE) {
			dev_err(&dfudev->intf->dev,
				"Downloading failed. DFU State: %d\n", dfust);
			break;
		}
	} while (lenrem > 0);
	if (*f_pos != 0 || numb > 32)
		*f_pos += numb;

	if (dma)
		dma_unmap_single(ctrler, dmabuf, dfudev->xfersize,
				DMA_FROM_DEVICE);

	return numb;
}

static const struct file_operations dfu_fops = {
	.owner		= THIS_MODULE,
	.open		= dfu_open,
	.release	= dfu_release,
	.read		= dfu_upload,
	.write		= dfu_dnload
};

static ssize_t dfu_sndcmd(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct dfu1_device *dfudev;
	struct dfu_control *ctrl;

	if (count < 1 || count > 32)
		return count;

	dfudev = container_of(attr, struct dfu1_device, tachattr);
	if (!mutex_trylock(&dfudev->lock)) {
		dev_err(&dfudev->intf->dev,
				"Cannot send command, device busy\n");
		return count;
	}
	ctrl = kmalloc(sizeof(struct dfu_control) + count, GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dfurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ctrl->dfurb) {
		kfree(ctrl);
		return -ENOMEM;
	}

	ctrl->usbdev = dfudev->usbdev;
	ctrl->intf = dfudev->intf;
	ctrl->intfnum = dfudev->intfnum;
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_DNLOAD;
	ctrl->req.wValue = 0;
	ctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	ctrl->req.wLength = cpu_to_le16(count);
	ctrl->pipe = usb_sndctrlpipe(dfudev->usbdev, 0);
	ctrl->datbuf = ctrl + 1;
	memcpy(ctrl->datbuf, buf, count);
	ctrl->len = count;
	if (dfu_submit_urb(ctrl, urb_timeout) ||
			dfu_get_status(ctrl))
		dev_err(&dfudev->intf->dev,
				"DFU command failed: %d, State: %d\n",
				(int)ctrl->dfuStatus.bStatus,
				(int)ctrl->dfuStatus.bState);
	else
		dev_info(&dfudev->intf->dev,
				"DFU commmand status: %d, State: %d\n",
				(int)ctrl->dfuStatus.bStatus,
				(int)ctrl->dfuStatus.bState);
	usb_free_urb(ctrl->dfurb);
	kfree(ctrl);
	mutex_unlock(&dfudev->lock);
	return count;
}

static ssize_t dfu_attr_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct dfu1_device *dfudev;

	dfudev = container_of(attr, struct dfu1_device, attrattr);
	return sprintf(buf, "Download:%d Upload:%d Manifest:%d Detach:%d\n",
		dfudev->download, dfudev->upload, dfudev->manifest,
		dfudev->detach);
}

static ssize_t dfu_timeout_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct dfu1_device *dfudev;

	dfudev = container_of(attr, struct dfu1_device, tmoutattr);
	return sprintf(buf, "%d\n", dfudev->dettmout);
}

static ssize_t dfu_xfersize_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct dfu1_device *dfudev;

	dfudev = container_of(attr, struct dfu1_device, xsizeattr);
	return sprintf(buf, "%d\n", dfudev->xfersize);
}

static ssize_t dfu_state_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct dfu1_device *dfudev;
	struct dfu_control *ctrl;
	int dfstat;

	ctrl = kmalloc(sizeof(struct dfu_control), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	dfudev = container_of(attr, struct dfu1_device, statattr);
	ctrl->dfurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ctrl->dfurb) {
		kfree(ctrl);
		return -ENOMEM;
	}
	ctrl->usbdev = dfudev->usbdev;
	ctrl->intf = dfudev->intf;
	ctrl->intfnum = dfudev->intfnum;
	dfstat = dfu_get_state(ctrl);
	usb_free_urb(ctrl->dfurb);
	kfree(ctrl);
	return sprintf(buf, "%d\n", dfstat);
}

static ssize_t stellaris_show(struct dfu1_device *dfudev,
			struct dfu_control *ctrl, char *buf)
{
	struct tidfu {
		unsigned short usMarker;
		unsigned short usVersion;
	} *stellaris;
	unsigned short usMarker, usVersion;
	int num = 0;

	ctrl->req.bRequest = 0x42; /* Request Stellaris */
	ctrl->req.bRequestType = 0xa1;
	ctrl->req.wValue = 0x23;
	ctrl->req.wIndex = cpu_to_le16(dfudev->intfnum);
	ctrl->req.wLength = 4;
	ctrl->pipe = usb_rcvctrlpipe(dfudev->usbdev, 0);
	ctrl->datbuf = ctrl->ocupy;
	ctrl->len = sizeof(struct tidfu);
	stellaris = ctrl->datbuf;
	if (dfu_submit_urb(ctrl, urb_timeout) == 0) {
		usMarker = le16_to_cpu(stellaris->usMarker);
		usVersion = le16_to_cpu(stellaris->usVersion);
		num = sprintf(buf, "Stellaris Marker: %4.4X, Version: %4.4X\n",
			usMarker, usVersion);
	}
	return num;
}

static ssize_t dfu_query_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct dfu1_device *dfudev;
	struct dfu_control *ctrl;
	unsigned short idVendor, idProduct;
	ssize_t numbytes;

	dfudev = container_of(attr, struct dfu1_device, queryattr);
	idVendor = le16_to_cpu(dfudev->usbdev->descriptor.idVendor);
	idProduct = le16_to_cpu(dfudev->usbdev->descriptor.idProduct);
	ctrl = kmalloc(sizeof(struct dfu_control), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dfurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ctrl->dfurb) {
		kfree(ctrl);
		return -ENOMEM;
	}
	ctrl->usbdev = dfudev->usbdev;
	ctrl->intf = dfudev->intf;
	ctrl->intfnum = dfudev->intfnum;
	numbytes = 0;
	if (idVendor == USB_VENDOR_LUMINARY &&
	    idProduct == USB_PRODUCT_STELLARIS_DFU)
		numbytes = stellaris_show(dfudev, ctrl, buf);

	usb_free_urb(ctrl->dfurb);
	kfree(ctrl);
	return numbytes;
}

static ssize_t dfu_clear_cmd(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct dfu1_device *dfudev;
	struct dfu_control *ctrl;
	int dfust;

	dfudev = container_of(attr, struct dfu1_device, abortattr);
	if (count < 1 || *buf != '1') {
		dev_warn(&dfudev->intf->dev, "Invalid command: %c\n", *buf);
		return count;
	}

	ctrl = kmalloc(sizeof(struct dfu_control), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->dfurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ctrl->dfurb) {
		kfree(ctrl);
		return -ENOMEM;
	}

	ctrl->usbdev = dfudev->usbdev;
	ctrl->intf = dfudev->intf;
	ctrl->intfnum = dfudev->intfnum;
	dfust = dfu_get_state(ctrl);
	switch (dfust) {
	case dfuDNLOAD_IDLE:
	case dfuUPLOAD_IDLE:
		dfu_abort(ctrl);
		break;
	case dfuERROR:
		dfu_clr_status(ctrl);
		break;
	default:
		dev_warn(&dfudev->intf->dev, "Cannot clear, in state: %d\n",
				dfust);
		break;
	}
	usb_free_urb(ctrl->dfurb);
	kfree(ctrl);
	return count;
}

static int dfu_create_attrs(struct dfu1_device *dfudev)
{
	int retv = 0;

	dfudev->tachattr.attr.name = "dfucmd";
	dfudev->tachattr.attr.mode = 0200;
	dfudev->tachattr.show = NULL;
	dfudev->tachattr.store = dfu_sndcmd;
	retv = device_create_file(&dfudev->intf->dev, &dfudev->tachattr);
	if (retv != 0) {
		dev_err(&dfudev->intf->dev, "Cannot create sysfs file %d\n",
				retv);
		return retv;
	}
	dfudev->attrattr.attr.name = "attr";
	dfudev->attrattr.attr.mode = 0444;
	dfudev->attrattr.show = dfu_attr_show;
	dfudev->attrattr.store = NULL;
	retv = device_create_file(&dfudev->intf->dev, &dfudev->attrattr);
	if (retv != 0) {
		dev_err(&dfudev->intf->dev, "Cannot create sysfs file %d\n",
				retv);
		goto err_10;
	}
	dfudev->tmoutattr.attr.name = "timeout";
	dfudev->tmoutattr.attr.mode = 0444;
	dfudev->tmoutattr.show = dfu_timeout_show;
	dfudev->tmoutattr.store = NULL;
	retv = device_create_file(&dfudev->intf->dev, &dfudev->tmoutattr);
	if (retv != 0) {
		dev_err(&dfudev->intf->dev, "Cannot create sysfs file %d\n",
				retv);
		goto err_20;
	}
	dfudev->xsizeattr.attr.name = "xfersize";
	dfudev->xsizeattr.attr.mode = 0444;
	dfudev->xsizeattr.show = dfu_xfersize_show;
	dfudev->xsizeattr.store = NULL;
	retv = device_create_file(&dfudev->intf->dev, &dfudev->xsizeattr);
	if (retv != 0) {
		dev_err(&dfudev->intf->dev, "Cannot create sysfs file %d\n",
				retv);
		goto err_30;
	}
	dfudev->statattr.attr.name = "state";
	dfudev->statattr.attr.mode = 0444;
	dfudev->statattr.show = dfu_state_show;
	dfudev->statattr.store = NULL;
	retv = device_create_file(&dfudev->intf->dev, &dfudev->statattr);
	if (retv != 0) {
		dev_err(&dfudev->intf->dev, "Cannot create sysfs file %d\n",
				retv);
		goto err_40;
	}
	dfudev->abortattr.attr.name = "clear";
	dfudev->abortattr.attr.mode = 0200;
	dfudev->abortattr.store = dfu_clear_cmd;
	dfudev->abortattr.show = NULL;
	retv = device_create_file(&dfudev->intf->dev, &dfudev->abortattr);
	if (retv != 0) {
		dev_err(&dfudev->intf->dev, "Cannot create sysfs file %d\n",
				retv);
		goto err_50;
	}
	dfudev->queryattr.attr.name = "query";
	dfudev->queryattr.attr.mode =  0444;
	dfudev->queryattr.show = dfu_query_show;
	dfudev->queryattr.store = NULL;
	retv = device_create_file(&dfudev->intf->dev, &dfudev->queryattr);
	if (retv != 0) {
		dev_err(&dfudev->intf->dev, "Cannot create sysfs file %d\n",
				retv);
		goto err_60;
	}

	return retv;

err_60:
	device_remove_file(&dfudev->intf->dev, &dfudev->abortattr);
err_50:
	device_remove_file(&dfudev->intf->dev, &dfudev->statattr);
err_40:
	device_remove_file(&dfudev->intf->dev, &dfudev->xsizeattr);
err_30:
	device_remove_file(&dfudev->intf->dev, &dfudev->tmoutattr);
err_20:
	device_remove_file(&dfudev->intf->dev, &dfudev->attrattr);
err_10:
	device_remove_file(&dfudev->intf->dev, &dfudev->tachattr);
	return retv;
}

static void dfu_remove_attrs(struct dfu1_device *dfudev)
{
	device_remove_file(&dfudev->intf->dev, &dfudev->queryattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->abortattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->statattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->xsizeattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->tmoutattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->attrattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->tachattr);
}

static atomic_t dfu_index = ATOMIC_INIT(-1);
static atomic_t *dev_minors;

static int dfu_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	int retv, dfufdsc_len, index;
	struct dfu1_device *dfudev;
	struct dfufdsc *dfufdsc;
	int i;

	retv = 0;
	dfufdsc = (struct dfufdsc *)intf->cur_altsetting->extra;
	dfufdsc_len = intf->cur_altsetting->extralen;
	if (!dfufdsc || dfufdsc_len != USB_DFU_FUNC_DSCLEN ||
			dfufdsc->dsctyp != USB_DFU_FUNC_DSCTYP) {
		dev_err(&intf->dev, "Invalid DFU functional descriptor\n");
		return -ENODEV;
	}
	index = atomic_inc_return(&dfu_index);
	if (!(index < max_dfus)) {
		dev_err(&intf->dev, "Maximum supported USB DFU reached: %d\n",
				max_dfus);
		retv = -ENODEV;
		goto err_05;
	}
	dfudev = kmalloc(sizeof(struct dfu1_device), GFP_KERNEL);
	if (!dfudev) {
		retv = -ENOMEM;
		goto err_05;
	}
	dfudev->download = (dfufdsc->attr & 0x01) ? 1 : 0;
	dfudev->upload = (dfufdsc->attr & 0x02) ? 1 : 0;
	dfudev->manifest = (dfufdsc->attr & 0x04) ? 1 : 0;
	dfudev->detach = (dfufdsc->attr & 0x08) ? 1 : 0;
	dfudev->dettmout = le16_to_cpu(dfufdsc->tmout);
	dfudev->xfersize = le16_to_cpu(dfufdsc->xfersize);
	dfudev->intf = intf;
	dfudev->usbdev = interface_to_usbdev(intf);
	dfudev->intfnum = intf->cur_altsetting->desc.bInterfaceNumber;
	dfudev->proto = 2;
	if (dfudev->usbdev->bus->controller->dma_mask)
		dfudev->dma = 1;
	else
		dfudev->dma = 0;
	mutex_init(&dfudev->lock);

	retv = dfu_create_attrs(dfudev);
	if (retv)
		goto err_10;

        for (i = 0; i < max_dfus; i++)
                if (!atomic_xchg(dev_minors+i, 1))
                        break;
	if (i == max_dfus) {
		retv = -EINVAL;
		dev_err(&dfudev->intf->dev, "No minor usable, Logic Error\n");
		goto err_15;
	}
        dfudev->devno = MKDEV(MAJOR(dfu_devno), i);

	cdev_init(&dfudev->cdev, &dfu_fops);
	dfudev->cdev.owner = THIS_MODULE;
	retv = cdev_add(&dfudev->cdev, dfudev->devno, 1);
	if (retv) {
		dev_err(&dfudev->intf->dev, "Cannot add device: %d\n", retv);
		goto err_20;
	}
	dfudev->sysdev = device_create(dfu_class, &intf->dev, dfudev->devno,
				dfudev, DFUDEV_NAME"%d", MINOR(dfudev->devno));
	if (IS_ERR(dfudev->sysdev)) {
		retv = (int)PTR_ERR(dfudev->sysdev);
		dev_err(&dfudev->intf->dev, "Cannot create device file: %d\n",
				retv);
		goto err_30;
	}

        usb_set_intfdata(intf, dfudev);
	return retv;

err_30:
	cdev_del(&dfudev->cdev);
err_20:
	atomic_set(dev_minors+MINOR(dfudev->devno), 0);
err_15:
	dfu_remove_attrs(dfudev);
err_10:
	kfree(dfudev);
err_05:
	atomic_dec(&dfu_index);
	return retv;
}

static void dfu_disconnect(struct usb_interface *intf)
{
	struct dfu1_device *dfudev;

	dfudev = usb_get_intfdata(intf);
	usb_set_intfdata(intf, NULL);
	device_destroy(dfu_class, dfudev->devno);
	cdev_del(&dfudev->cdev);
	atomic_set(dev_minors+MINOR(dfudev->devno), 0);
	dfu_remove_attrs(dfudev);
	kfree(dfudev);
	atomic_dec(&dfu_index);
}

static struct usb_driver dfu_driver = {
	.name = "dfusb1",
	.probe = dfu_probe,
	.disconnect = dfu_disconnect,
	.id_table = dfu_ids,
};

static int __init usbdfu_init(void)
{
	int i, retv;

	retv = alloc_chrdev_region(&dfu_devno, 0, max_dfus, DFUDEV_NAME);
	if (retv != 0) {
		pr_err("Cannot allocate a char major number: %d\n", retv);
		return retv;
	}
	dfu_class = class_create(THIS_MODULE, DFUDEV_NAME);
	if (IS_ERR(dfu_class)) {
		retv = (int)PTR_ERR(dfu_class);
		pr_err("Cannot create DFU class, Out of Memory!\n");
		goto err_10;
	}
	dev_minors = kmalloc_array(max_dfus, sizeof(atomic_t), GFP_KERNEL);
	if (!dev_minors) {
		retv = -ENOMEM;
		pr_err("Cannot allocate minor talbe, Out of Memory\n");
		goto err_20;
	}
	for (i = 0; i < max_dfus; i++)
		atomic_set(dev_minors+i, 0);
        retv = usb_register(&dfu_driver);
	if (retv) {
		pr_err("Cannot register USB DFU driver: %d\n", retv);
		goto err_30;
	}

        return 0;

err_30:
	kfree(dev_minors);
err_20:
	class_destroy(dfu_class);
err_10:
	unregister_chrdev_region(dfu_devno, max_dfus);
	return retv;
}

static void __exit usbdfu_exit(void)
{
	usb_deregister(&dfu_driver);
	kfree(dev_minors);
	class_destroy(dfu_class);
	unregister_chrdev_region(dfu_devno, max_dfus);
}

module_init(usbdfu_init);
module_exit(usbdfu_exit);
