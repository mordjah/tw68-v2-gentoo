tw6800-objs	:= tw68-core.o tw68-video.o tw68-cards.o

obj-m += tw6800.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

install:
	cp -p *.ko /lib/modules/$(shell uname -r)/extra
	depmod -a

remove:
	rm -rf /lib/modules/$(shell uname -r)/extra/tw68*
