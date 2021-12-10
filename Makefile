# SPDX-License-Identifier: GPL-2.0-only
snd-usb-zoom-objs := driver.o pcm.o
obj-$(CONFIG_SND_USB_AUDIO) += snd-usb-zoom.o

.PHONY: build
build:
	make -C /usr/src/linux/ M=`pwd` modules
	@rm -f snd-usb-zoom.ko.zst
	@zstd snd-usb-zoom.ko
clean:
	make -C /usr/src/linux/ M=`pwd` clean
