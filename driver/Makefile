obj-m += pac1954.o

# DKMS sets PAC1954_VERSION to the package version.
ifneq ($(PAC1954_VERSION),)
ccflags-y += -DPAC1954_VERSION=\"$(PAC1954_VERSION)\"
endif

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
