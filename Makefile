ifneq ($(KERNELRELEASE),)
# call from kernel build system

tw68-objs := tw68-core.o tw68-cards.o tw68-i2c.o tw68-video.o \
	tw68-controls.o tw68-fileops.o tw68-ioctls.o tw68-vbi.o \
	tw68-ts.o tw68-risc.o tw68-input.o tw68-tvaudio.o

obj-m += tw68.o

else

PWD := $(shell pwd)
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf modules.order videotest

run: all
	-sudo rmmod tw68
	sudo insmod tw68.ko

cscope:
	find -type f -name "*.[hc]" | cscope -b -i -

endif
