#ifndef LINUX_USB_DFU_0_DSCAO__
#define LINUX_USB_DFU_0_DSCAO__
/*
 * usbdfu0.h
 *
 * Copyright (c) 2017 Dashi Cao        <dscao999@hotmail.com, caods1@lenovo.com>
 *
 * USB Abstract Control Model driver for USB Device Firmware Upgrade, for Protocol 0
 *
*/
#include "usbdfu.h"

struct dfu0_device {
	struct usb_device *usbdev;
	struct usb_interface *intf;
	struct device_attribute tachattr;
	struct device_attribute attrattr;
	struct device_attribute tmoutattr;
	struct device_attribute xsizeattr;
	struct {
		unsigned int download:1;
		unsigned int upload:1;
		unsigned int manifest:1;
		unsigned int detach:1;
	};
	int dettmout;
	int xfersize;
	int proto;
	int intfnum;
};
#endif /* LINUX_USB_DFU_0_DSCAO__ */
