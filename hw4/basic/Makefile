ifneq ($(KERNELRELEASE),)

obj-m := myChrDevice.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRA_CFLAGS = -I../include
PWD := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
    
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
    
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
    

.PHONY: modules modules_install clean

endif

