/*
 * usbdfu.c
 *
 * Copyright (c) 2017 Dashi Cao        <dscao999@hotmail.com, caods1@lenovo.com>
 *
 * USB Abstract Control Model driver for USB Device Firmware Upgrade
 *
 */
#include <stdarg.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include "usbdfu0.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dashi Cao");
MODULE_DESCRIPTION("USB DFU Class Driver for Protocol 0");

static int urb_timeout = 200; /* milliseconds */
module_param(urb_timeout, int, 0644);
MODULE_PARM_DESC(urb_timeout, "USB urb completion timeout value. "
	"Default 200 milliseconds.");

static int detach_timeout = 2000; /* 2 seconds */
module_param(detach_timeout, int, 0644);
MODULE_PARM_DESC(detach_timeout, "Time window for reset after detaching DFU. "
	"Default 2 seconds.");

static const struct usb_device_id dfu_ids[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_APP_SPEC, USB_DFU_SUBCLASS,
		USB_DFU_PROTO_RUNTIME) },
	{ }
};
MODULE_DEVICE_TABLE(usb, dfu_ids);

static int dfu_do_switch(struct dfu0_device *dfudev, struct dfu_control *ctrl)
{
	int tmout, retusb;

	tmout = dfudev->dettmout > detach_timeout ?
				detach_timeout : dfudev->dettmout;
	ctrl->req.bRequestType = 0x21;
	ctrl->req.bRequest = USB_DFU_DETACH;
	ctrl->req.wIndex = cpu_to_le16(ctrl->intfnum);
	ctrl->req.wValue = cpu_to_le16(tmout);
	ctrl->req.wLength = 0;
	ctrl->pipe = usb_sndctrlpipe(ctrl->usbdev, 0);
	ctrl->len = 0;
	ctrl->datbuf = NULL;
	retusb = dfu_submit_urb(ctrl, urb_timeout);
	if (retusb == 0 && dfudev->detach == 0)
		dev_info(&dfudev->intf->dev, "Need reset to switch to DFU\n");
	return retusb;
}

static ssize_t dfu_switch(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct dfu0_device *dfudev;
	struct dfu_control *ctrl;

	dfudev = container_of(attr, struct dfu0_device, tachattr);
	ctrl = kmalloc(sizeof(struct dfu_control), GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;
	ctrl->usbdev = dfudev->usbdev;
	ctrl->intf = dfudev->intf;
	ctrl->intfnum = dfudev->intfnum;
	ctrl->dfurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ctrl->dfurb) {
		kfree(ctrl);
		return -ENOMEM;
	}

	if (count > 0 && *buf == '-' && (*(buf+1) == '\n' || *(buf+1) == 0))
		dfu_do_switch(dfudev, ctrl);
	else
		dev_err(dev, "Invalid Command: %c\n", *buf);

	usb_free_urb(ctrl->dfurb);
	kfree(ctrl);
	return count;
}

static ssize_t dfu_attr_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct dfu0_device *dfudev;

	dfudev = container_of(attr, struct dfu0_device, attrattr);
	return sprintf(buf, "Download:%d Upload:%d Manifest:%d Detach:%d\n",
		dfudev->download, dfudev->upload, dfudev->manifest,
		dfudev->detach);
}

static ssize_t dfu_timeout_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dfu0_device *dfudev;

	dfudev = container_of(attr, struct dfu0_device, tmoutattr);
	return sprintf(buf, "%d\n", dfudev->dettmout);
}

static ssize_t dfu_xfersize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dfu0_device *dfudev;

	dfudev = container_of(attr, struct dfu0_device, xsizeattr);
	return sprintf(buf, "%d\n", dfudev->xfersize);
}

static int dfu_create_attrs(struct dfu0_device *dfudev)
{
	int retv = 0;

	dfudev->tachattr.attr.name = "detach";
	dfudev->tachattr.attr.mode = 0200;
	dfudev->tachattr.show = NULL;
	dfudev->tachattr.store = dfu_switch;
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

	return retv;

err_30:
	device_remove_file(&dfudev->intf->dev, &dfudev->tmoutattr);
err_20:
	device_remove_file(&dfudev->intf->dev, &dfudev->attrattr);
err_10:
	device_remove_file(&dfudev->intf->dev, &dfudev->tachattr);
	return retv;
}

static void dfu_remove_attrs(struct dfu0_device *dfudev)
{
	device_remove_file(&dfudev->intf->dev, &dfudev->xsizeattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->tmoutattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->attrattr);
	device_remove_file(&dfudev->intf->dev, &dfudev->tachattr);
}

static int dfu_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	int retv;
	struct dfu0_device *dfudev;
	struct dfufdsc *dfufdsc;
	int dfufdsc_len;

	dfufdsc = (struct dfufdsc *)intf->cur_altsetting->extra;
	dfufdsc_len = intf->cur_altsetting->extralen;
	if (!dfufdsc || dfufdsc_len != USB_DFU_FUNC_DSCLEN ||
			dfufdsc->dsctyp != USB_DFU_FUNC_DSCTYP) {
		dev_err(&intf->dev, "Invalid DFU functional descriptor\n");
		return -ENODEV;
	}

	dfudev = kmalloc(sizeof(struct dfu0_device), GFP_KERNEL);
	if (!dfudev)
		return -ENOMEM;
	dfudev->download = (dfufdsc->attr & 0x01) ? 1 : 0;
	dfudev->upload = (dfufdsc->attr & 0x02) ? 1 : 0;
	dfudev->manifest = (dfufdsc->attr & 0x04) ? 1 : 0;
	dfudev->detach = (dfufdsc->attr & 0x08) ? 1 : 0;
	dfudev->dettmout = le16_to_cpu(dfufdsc->tmout);
	dfudev->xfersize = le16_to_cpu(dfufdsc->xfersize);
	dfudev->intf = intf;
	dfudev->usbdev = interface_to_usbdev(intf);
	dfudev->intfnum = intf->cur_altsetting->desc.bInterfaceNumber;
	dfudev->proto = 1;

	retv = dfu_create_attrs(dfudev);
	if (retv)
		kfree(dfudev);
	else
		usb_set_intfdata(intf, dfudev);
	return retv;
}

static void dfu_disconnect(struct usb_interface *intf)
{
	struct dfu0_device *dfudev;

	dfudev = usb_get_intfdata(intf);
	dfu_remove_attrs(dfudev);
	usb_set_intfdata(dfudev->intf, NULL);
	kfree(dfudev);
}

static struct usb_driver dfu_driver = {
	.name = "dfusb0",
	.probe = dfu_probe,
	.disconnect = dfu_disconnect,
	.id_table = dfu_ids,
};

static int __init usbdfu_init(void)
{
	return usb_register(&dfu_driver);
}

static void __exit usbdfu_exit(void)
{
	usb_deregister(&dfu_driver);
}

module_init(usbdfu_init);
module_exit(usbdfu_exit);
