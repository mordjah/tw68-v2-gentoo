# This makefile should allow the user to build and install the driver
# on most Linux systems.
#
# The 'make' variable 'TW68_TESTING' is only for use by driver developers,
# and will result in the inclusion of unstable and untested code being
# included.
#
# The normal usage of this Makefile is as follows:
# 	make		Compile the source and produce the tw68.ko module
#	make clean	Remove all working files, plus the tw68.ko module
#	make insmod	Compile the source and install the resulting
#			module in a running system.  Note that the module
#			will not left installed if the system is shutdown
#			and restarted.
#	make install	Compile the source, remove any previous versions of
#			the tw68 module from the running system's module
#			directory, and install the new tw68.ko into that
#			directory.  Note that the new module should be
#			automatically used any time the system is restarted.
#	make run	Do a 'make insmod', and after the new module has been
#			installed, use mplayer to display /dev/video0.  Also
#			start an instance of v4l2ucp for a "Control Panel".
#
ifneq ($(KERNELRELEASE),)
# call from kernel build system

tw68-objs := tw68-core.o tw68-cards.o tw68-video.o \
	     tw68-vbi.o tw68-ts.o tw68-risc.o tw68-tvaudio.o

ifneq ($(TW68_TESTING),)
tw68-objs += tw68-i2c.o
EXTRA_CFLAGS += -DTW68_TESTING
endif

obj-m += tw68.o

else

PWD := $(shell pwd)
KDIR ?= /lib/modules/$(shell uname -r)/build
TWMODS := /lib/modules/$(shell uname -r)
TWDEST := $(TWMODS)/kernel/drivers/media/video

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf modules.order videotest

insmod: all
	-@sudo rmmod tw68 > /dev/null 2>&1
	@sudo modprobe v4l2_common
	@sudo modprobe videobuf_dma_sg
	@sudo modprobe btcx_risc
	@sudo insmod tw68.ko core_debug=3 video_debug=3

run: insmod
	test -x /usr/bin/v4l2ucp && v4l2ucp &
	test -x /usr/bin/mplayer && mplayer tv:// -tv device=/dev/video0:outfmt=yuy2:normid=3:width=640:height=480
	killall v4l2ucp

cscope:
	find -type f -name "*.[hc]" | cscope -b -i -

install: all
	sudo find $(TWMODS) -name tw68.ko -exec rm -f {} \;
	sudo cp -p tw68.ko $(TWDEST)
	sudo depmod -a

endif
