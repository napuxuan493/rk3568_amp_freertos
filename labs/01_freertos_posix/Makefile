# 实验 01：FreeRTOS POSIX 模拟器
# 把 FreeRTOS 内核编译成 Linux 用户态进程，不需要板子即可学习调度/队列/信号量
# 内核源码：../../core1/freertos-kernel（V10.6.2 LTS）

CC      ?= gcc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=gnu11
CFLAGS  += -pthread -D_GNU_SOURCE

KERNEL  := ../../core1/freertos-kernel
PORT    := $(KERNEL)/portable/ThirdParty/GCC/Posix

# 内核核心源文件（tasks.c=调度核心 queue.c=队列/信号量 list.c=链表 timers.c=软定时器）
KSRC := $(KERNEL)/tasks.c $(KERNEL)/queue.c $(KERNEL)/list.c \
        $(KERNEL)/timers.c $(KERNEL)/event_groups.c $(KERNEL)/stream_buffer.c

# POSIX 移植层：port.c（tick 用 setitimer+SIGALRM 模拟）、wait_for_event.c
PSRC := $(PORT)/port.c $(PORT)/utils/wait_for_event.c

INC := -I. -I$(KERNEL)/include -I$(PORT)

# 内存管理：heap_3 = 包一层 malloc/free（POSIX 模拟器专用；板子上一般用 heap_4）
HSRC := $(KERNEL)/portable/MemMang/heap_3.c

SRCS := main.c $(KSRC) $(PSRC) $(HSRC)

all: freertos_lab

freertos_lab: $(SRCS) FreeRTOSConfig.h
	$(CC) $(CFLAGS) $(INC) $(SRCS) -o $@

run: freertos_lab
	./freertos_lab

clean:
	rm -f freertos_lab

.PHONY: all run clean
