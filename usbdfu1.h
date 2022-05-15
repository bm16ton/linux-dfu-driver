#ifndef LINUX_USB_DFU_1_DSCAO__
#define LINUX_USB_DFU_1_DSCAO__
/*
 * usbdfu0.h
 *
 * Copyright (c) 2017 Dashi Cao        <dscao999@hotmail.com, caods1@lenovo.com>
 *
 * USB Abstract Control Model driver for USB Device Firmware Upgrade, for Protocol 0
 *
*/
#include <linux/cdev.h>
#include "usbdfu.h"

struct dfu1_device {
	struct mutex lock;
	struct usb_device *usbdev;
	struct usb_interface *intf;
	struct device *sysdev;
	struct device_attribute tachattr;
	struct device_attribute attrattr;
	struct device_attribute tmoutattr;
	struct device_attribute xsizeattr;
	struct device_attribute statattr;
	struct device_attribute abortattr;
	struct device_attribute queryattr;
	struct {
		unsigned int download:1;
		unsigned int upload:1;
		unsigned int manifest:1;
		unsigned int detach:1;
	};
	struct dfu_control *opctrl, *stctrl;
	void *datbuf;
	dev_t devno;
	int dettmout;
	int xfersize;
	int proto;
	int intfnum;
	int dma;
	struct cdev cdev;
};
#endif /* LINUX_USB_DFU_1_DSCAO__ */
