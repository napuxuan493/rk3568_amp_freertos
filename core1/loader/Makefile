# 内核模块构建：板子上 insmod 用
obj-m += amp_loader.o
obj-m += psci_on.o

KDIR := /home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux/kernel

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
