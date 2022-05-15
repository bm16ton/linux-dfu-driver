/*
 * usbdfu.c
 *
 * Copyright (c) 2017 Dashi Cao        <dscao999@hotmail.com, caods1@lenovo.com>
 *
 * USB Abstract Control Model driver for USB Device Firmware Upgrade
 *
 */
#include "usbdfu.h"

static void dfu_ctrlurb_done(struct urb *urb)
{
	struct dfu_control *ctrl;

	ctrl = urb->context;
	ctrl->status = urb->status;
	ctrl->nxfer = urb->actual_length;
	complete(&ctrl->urbdone);
}

static void dfu_urb_timeout(struct dfu_control *ctrl)
{
	usb_unlink_urb(ctrl->dfurb);
	wait_for_completion(&ctrl->urbdone);
	if (ctrl->req.bRequest != USB_DFU_ABORT)
		dev_err(&ctrl->intf->dev,
			"URB req type: %2.2x, req: %2.2x cancelled\n",
			(int)ctrl->req.bRequestType,
			(int)ctrl->req.bRequest);
}

int dfu_submit_urb(struct dfu_control *ctrl, int tmout)
{
	int retusb, retv;
	unsigned long jiff_wait;

	usb_fill_control_urb(ctrl->dfurb, ctrl->usbdev, ctrl->pipe,
			(__u8 *)&ctrl->req, ctrl->datbuf, ctrl->len,
			dfu_ctrlurb_done, ctrl);
	init_completion(&ctrl->urbdone);
	ctrl->status = USB_DFU_ERROR_CODE;
	ctrl->nxfer = 0;
	retusb = usb_submit_urb(ctrl->dfurb, GFP_KERNEL);
	if (retusb == 0) {
		jiff_wait = msecs_to_jiffies(tmout);
		if (!wait_for_completion_timeout(&ctrl->urbdone, jiff_wait))
			dfu_urb_timeout(ctrl);
	} else
		dev_err(&ctrl->intf->dev,
			"URB type: %2.2x, req: %2.2x submit failed: %d\n",
			(int)ctrl->req.bRequestType,
			(int)ctrl->req.bRequest, retusb);
	retv = READ_ONCE(ctrl->status);
	if (retv && ctrl->req.bRequest != USB_DFU_ABORT)
		dev_err(&ctrl->intf->dev,
			"URB type: %2.2x, req: %2.2x request failed: %d\n",
			(int)ctrl->req.bRequestType, (int)ctrl->req.bRequest,
			retv);

	return retv;
}
