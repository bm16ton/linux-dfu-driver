ifneq ($(KERNELRELEASE),)
	obj-m := dfusb0.o
	dfusb0-objs := usbdfu0.o usbdfu.o
	obj-m += dfusb1.o
	dfusb1-objs := usbdfu1.o usbdfu.o

usbdfu0.o: usbdfu0.h usbdfu.h
usbdfu1.o: usbdfu1.h usbdfu.h

else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	WDIR := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(WDIR) modules

clean:
	@rm -f modules.order Module.symvers
	@rm -rf .tmp_versions
	@rm -f *.o *.ko *.mod.c
	@rm -f .*.ko.cmd .*.o.cmd *.mod
	@rm -f core
endif
