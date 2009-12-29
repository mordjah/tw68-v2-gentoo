ifneq ($(KERNELRELEASE),)
# call from kernel build system

tw68-objs := tw68-core.o tw68-cards.o tw68-video.o tw68-ioctls.o \
	     tw68-vbi.o tw68-ts.o tw68-risc.o tw68-tvaudio.o

obj-m += tw68.o

else

PWD := $(shell pwd)
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf modules.order videotest

insmod: all
	-sudo rmmod tw68 2>&1 > /dev/null
	sudo modprobe v4l2_common
	sudo modprobe videobuf_dma_sg
	sudo modprobe btcx_risc
	sudo insmod tw68.ko core_debug=10 video_debug=10

run: insmod
	test -x /usr/bin/v4l2ucp && v4l2ucp &
	test -x /usr/bin/mplayer && mplayer tv:// -tv device=/dev/video0:outfmt=yuy2:normid=3:width=640:height=480
	killall v4l2ucp

cscope:
	find -type f -name "*.[hc]" | cscope -b -i -

endif
