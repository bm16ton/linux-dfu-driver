A USB DFU class driver for Linux kernel. USB DFU is a specification for
downloading firmware to devices. The official spec is at
http://www.usb.org/developers/docs/devclass_docs/DFU_1.1.pdf. This
project is to implement a general Linux USB class driver for USB DFU.
An attribute file "detach" will be present under /sys at the DFU interface of
a USB capable device. The attributes of transfer size, download/upload/auto
detach/manifestation capabilities, and detach timeout can be read from the file. 
Writing a - to the file will start a detach-attach process to switch the device
into DFU mode, so that device firmware can be read and/or written. A device file
/dev/dfu? will be present if there is a device in DFU mode. Reading/Writing this
file will initiate uploading/downloading of the device firmware.
