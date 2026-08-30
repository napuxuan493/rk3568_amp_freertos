# M2.2: core1 FreeRTOS 固件构建（ARM_CA53_64_BIT port，EL1 + GICv3 适配）
# 交叉工具链：SDK prebuilts

SDK_PREBUILT := /home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin
CROSS        ?= $(SDK_PREBUILT)/aarch64-none-linux-gnu-

CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
READELF := $(CROSS)readelf
SIZE    := $(CROSS)size

FREERTOS := freertos-kernel
PORT     := $(FREERTOS)/portable/GCC/ARM_CA53_64_BIT

CFLAGS  := -O2 -g -Wall -Wextra -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
           -I. -I$(FREERTOS)/include -I$(PORT) -DGUEST
LDFLAGS := -nostdlib -T link.ld

SRCS := startup.S uart.c main.c gic.c timer.c crash.c libc_min.c \
        rpmsg_mbox.c rpmsg_vring.c rpmsg_core.c \
        $(FREERTOS)/tasks.c \
        $(FREERTOS)/list.c \
        $(FREERTOS)/queue.c \
        $(FREERTOS)/timers.c \
        $(FREERTOS)/event_groups.c \
        $(FREERTOS)/stream_buffer.c \
        $(PORT)/port.c \
        $(PORT)/portASM.S
OBJS := $(SRCS:.S=.o)
OBJS := $(OBJS:.c=.o)

all: core1.elf core1.bin

core1.elf: $(OBJS) link.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)
	@echo "---- ELF 信息 ----"
	$(READELF) -h $@ | grep -E "Type|Entry"
	$(SIZE) $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

core1.bin: core1.elf
	$(OBJCOPY) -O binary core1.elf core1.bin
	@ls -l core1.bin
	@echo "校验: Entry 必须是 0x7000000（与 amp.its load 一致）"

# amp.img 必须用 SDK u-boot 的 mkimage 打包！
# 坑（8/24）：Ubuntu mkimage(2022.01) 不写 FIT 根节点 "totalsize" 属性，
# u-boot 的 fit_get_totalsize 查不到 → "AMP Error: No totalsize" → core1 不启动。
# SDK mkimage(2017.09) 会写 totalsize + version，u-boot 才能解析。
SDK_MKIMAGE := /home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux/u-boot/tools/mkimage

all: core1.elf core1.bin amp.img

amp.img: core1.bin amp.its
	$(SDK_MKIMAGE) -f amp.its amp.img

clean:
	rm -f $(OBJS) core1.elf core1.bin amp.img

.PHONY: all clean
