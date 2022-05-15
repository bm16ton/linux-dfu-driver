#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dashi Cao");
MODULE_DESCRIPTION("USB DFU Class Driver");

#define USB_DFU_DETACH		0
#define USB_DFU_GETSTATUS	3

#define USB_DFU_SUBCLASS	0x01
#define USB_DFU_PROTO_RUNTIME	0x01
#define USB_DFU_PROTO_DFUMODE	0x02
#define USB_DFU_FUNC_DSCLEN	0x09
#define USB_DFU_FUNC_DSCTYP	0x21

static const struct usb_device_id dfu_ids[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_APP_SPEC, USB_DFU_SUBCLASS,
		USB_DFU_PROTO_DFUMODE) },
	{ }
};
MODULE_DEVICE_TABLE(usb, dfu_ids);

typedef unsigned char u8;

struct __attribute__((packed)) dfufdsc {
	u8 len;
	u8 dsctyp;
	u8 attr;
	u16 tmout;
	u16 xfersize;
	u16 ver;
};

struct dfu_device {
	struct mutex devlock;
	struct urb *ctrlurb;
	struct usb_device *usbdev;
	struct usb_interface *intf;
	struct device_attribute devattr;
	struct usb_ctrlrequest ctrlreq;
	struct completion urbdone;
	int attr;
	int dettmout;
	int xfersize;
	int runtime;
	int intfnum;
	union {
		int detach;
		int status;
	} st;
};

static void detach_done(struct urb *urb)
{
	struct dfu_device *dfudev;

	dfudev = urb->context;
	dfudev->st.detach = urb->status;
	complete(&dfudev->urbdone);
}

static inline int dfudev_reset(struct dfu_device *dfudev)
{
	int retusb;

	retusb = usb_lock_device_for_reset(dfudev->usbdev, dfudev->intf);
	if (retusb == 0)
		retusb = usb_reset_device(dfudev->usbdev);
	else
		dev_err(&dfudev->intf->dev, "Device busy\n");

	return retusb;
}

static void submit_detach(struct dfu_device *dfudev)
{
	int tmout, pipe, retusb;
	unsigned long jiff_wait;

	tmout = dfudev->dettmout > 500? 500: dfudev->dettmout;
	dfudev->ctrlreq.bRequestType = 0b00100001;
	dfudev->ctrlreq.bRequest = USB_DFU_DETACH;
	dfudev->ctrlreq.wValue = cpu_to_le16(tmout);
	dfudev->ctrlreq.wIndex = cpu_to_le16(dfudev->intfnum);
	dfudev->ctrlreq.wLength = 0;
	pipe = usb_sndctrlpipe(dfudev->usbdev, 0);
	usb_fill_control_urb(dfudev->ctrlurb, dfudev->usbdev, pipe,
			(u8 *)&dfudev->ctrlreq, NULL, 0, detach_done, dfudev);
	init_completion(&dfudev->urbdone);
	dfudev->st.detach = -65535;
	retusb = usb_submit_urb(dfudev->ctrlurb, GFP_KERNEL);
	if (retusb == 0) {
		jiff_wait = msecs_to_jiffies(tmout/2);
		if (!wait_for_completion_timeout(&dfudev->urbdone, jiff_wait)) {
			usb_unlink_urb(dfudev->ctrlurb);
			wait_for_completion(&dfudev->urbdone);
		} else if ((dfudev->attr & 0x08) == 0)
			retusb = dfudev_reset(dfudev);
	} else
		dev_err(&dfudev->intf->dev, "Cannot submit URB: %d\n", retusb);

}

struct __attribute__((packed)) dfu_status {
	u8 bStatus;
	u8 ptmout[3];
	u8 bState;
	u8 istr;
};

static void submit_attach(struct dfu_device *dfudev)
{
	int tmout, pipe, retusb;
	unsigned long jiff_wait;
	struct dfu_status dfust;

	tmout = dfudev->dettmout > 500? 500: dfudev->dettmout;
	dfudev->ctrlreq.bRequestType = 0b10100001;
	dfudev->ctrlreq.bRequest = USB_DFU_GETSTATUS;
	dfudev->ctrlreq.wValue = 0;
	dfudev->ctrlreq.wIndex = cpu_to_le16(dfudev->intfnum);
	dfudev->ctrlreq.wLength = cpu_to_le16(6);
	pipe = usb_rcvctrlpipe(dfudev->usbdev, 0);
	usb_fill_control_urb(dfudev->ctrlurb, dfudev->usbdev, pipe,
			(u8 *)&dfudev->ctrlreq, &dfust, sizeof(dfust),
				detach_done, dfudev);
	init_completion(&dfudev->urbdone);
	dfudev->st.status = -65535;
	retusb = usb_submit_urb(dfudev->ctrlurb, GFP_KERNEL);
	if (retusb == 0) {
		jiff_wait = msecs_to_jiffies(tmout);
		if (!wait_for_completion_timeout(&dfudev->urbdone, jiff_wait)) {
			usb_unlink_urb(dfudev->ctrlurb);
			wait_for_completion(&dfudev->urbdone);
		}
	} else
		dev_err(&dfudev->intf->dev, "Cannot submit URB: %d\n", retusb);
	if (dfudev->st.status == 0)
		dev_info(&dfudev->intf->dev, "DFU State: %d\n",
				(int)dfust.bState);
	else
		dev_err(&dfudev->intf->dev, "Cannot get DFU Status: %d\n",
				dfudev->st.status);
}

static ssize_t dfu_switch(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct dfu_device *dfudev;
	int retv;

	retv = 0;
	dfudev = container_of(attr, struct dfu_device, devattr);
	dfudev->ctrlurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dfudev->ctrlurb) {
		dev_err(&dfudev->intf->dev,
				"Cannot allocate USB URB\n");
		return count;
	}

	if (count > 0 && *buf == '-' && (*(buf+1) == '\n' || *(buf+1) == 0)) {
		if (mutex_trylock(&dfudev->devlock)) {
			if (dfudev->runtime)
				submit_detach(dfudev);
			else
				submit_attach(dfudev);
			mutex_unlock(&dfudev->devlock);
		} else
			dev_err(dev, "Device busy\n");
	} else
		dev_err(dev, "Invalid Command: %c\n", *buf);

	usb_free_urb(dfudev->ctrlurb);
	dfudev->ctrlurb = NULL;
	return count;
}

static int dfu_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	int retv, dfufdsc_len;
	struct usb_interface_descriptor *intfdsc;
	struct dfu_device *dfudev;
	struct dfufdsc *dfufdsc;

	dev_info(&intf->dev, "usbprobe Probing now...\n");
	intfdsc = &intf->cur_altsetting->desc;
	dfufdsc = (struct dfufdsc *)intf->cur_altsetting->extra;
	dfufdsc_len = intf->cur_altsetting->extralen;
	if (!dfufdsc || dfufdsc_len != USB_DFU_FUNC_DSCLEN ||
			dfufdsc->dsctyp != USB_DFU_FUNC_DSCTYP ||
			intfdsc->bInterfaceClass != USB_CLASS_APP_SPEC ||
			intfdsc->bInterfaceSubClass != USB_DFU_SUBCLASS)
		return -ENODEV;

	dfudev = kmalloc(sizeof(struct dfu_device), GFP_KERNEL);
	if (!dfudev)
		return -ENOMEM;

	dfudev->ctrlurb = NULL;
	dfudev->attr = dfufdsc->attr;
	dfudev->dettmout = le16_to_cpu(dfufdsc->tmout);
	dfudev->xfersize = le16_to_cpu(dfufdsc->xfersize);
	dfudev->intf = intf;
	dfudev->usbdev = interface_to_usbdev(intf);
	dfudev->intfnum = intf->cur_altsetting->desc.bInterfaceNumber;
	mutex_init(&dfudev->devlock);

	retv = 0;
	if (intfdsc->bInterfaceProtocol == USB_DFU_PROTO_RUNTIME) {
		dfudev->devattr.attr.name = "detach";
		dfudev->runtime = 1;
	} else {
		dfudev->devattr.attr.name = "attach";
		dfudev->runtime = 0;
	}
	dfudev->devattr.attr.mode = S_IWUSR;
	dfudev->devattr.show = NULL;
	dfudev->devattr.store = dfu_switch;
	retv = device_create_file(&intf->dev, &dfudev->devattr);
	if (retv == 0)
		usb_set_intfdata(intf, dfudev);
	else {
		dev_err(&intf->dev, "Cannot create sysfs file \"detach\"\n");
		kfree(dfudev);
	}
	dev_info(&intf->dev, "usbprobe Probing done\n");

	return retv;
}

static void dfu_disconnect(struct usb_interface *intf)
{
	struct dfu_device *dev;

	dev = usb_get_intfdata(intf);
	device_remove_file(&intf->dev, &dev->devattr);
	usb_set_intfdata(intf, NULL);
	while (mutex_lock_interruptible(&dev->devlock) != 0)
		dev_err(&intf->dev, "Unable to get DFU lock\n");
	kfree(dev);
}

static struct usb_driver dfu_driver = {
	.name = "usbprobe",
	.probe = dfu_probe,
	.disconnect = dfu_disconnect,
	.id_table = dfu_ids,
};

static int __init usbdfu_init(void)
{
	int retv;

	retv = usb_register(&dfu_driver);
	if (retv)
		printk(KERN_ERR "Cannot register USB DFU driver\n");
	return retv;
}

static void __exit usbdfu_exit(void)
{
	usb_deregister(&dfu_driver);
}

module_init(usbdfu_init);
module_exit(usbdfu_exit);
