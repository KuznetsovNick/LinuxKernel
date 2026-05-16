obj-m := simplefs.o
simplefs-objs := simplefs_main.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

.PHONY: all user clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

user:
	$(MAKE) -C user

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C user clean