ifneq ($(KERNELRELEASE),)
obj-m := hello.o

else
# POINT THIS TO YOUR EXACT KERNEL SOURCE FOLDER
KDIR ?= /home/kyle/linux-7.0.9

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif
