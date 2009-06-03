tw68-objs := tw68-core.o tw68-cards.o tw68-i2c.o tw68-video.o \
	tw68-controls.o tw68-fileops.o tw68-ioctls.o tw68-vbi.o \
	tw68-ts.o tw68-risc.o tw68-input.o tw68-tvaudio.o

obj-m += tw68.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	cp -p *.ko /lib/modules/$(shell uname -r)/extra
	depmod -a

remove:
	rm -rf /lib/modules/$(shell uname -r)/extra/tw68*
