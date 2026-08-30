# M2 设计：core1 侧 FreeRTOS 移植（RK3568 AMP）

> 目标：在 RK3568 的 3 号核（core1）上跑起 FreeRTOS——裸机启动 → 串口打印 → tick 调度 → 任务运行。
> 前置：M1 已过（FreeRTOS 概念 + POSIX 模拟器）；Linux 侧三驱动已通读（mailbox/rpmsg/amp）。
> 本设计为"参考方案"，⚠️ 标记处需板级实测确认。

## 0. 里程碑拆分

| 阶段 | 内容 | 需要什么 | 验收 |
|---|---|---|---|
| **M2.1** | core1 裸机点亮：串口打印 "hello from core1" | 启动汇编 + 链接脚本 + UART 轮询 | 串口看到打印 |
| **M2.2** | FreeRTOS 跑起来：1~2 个任务 + tick | 上述 + GICv3 最小初始化 + generic timer | 任务周期打印 + `vTaskDelay` 生效 |
| **M3**（后续） | 与 Linux 通信：A2B 中断 + vring 读写 | 上述 + mailbox 寄存器操作 | Linux ↔ core1 双向消息 |

**设计原则**：M2.1 不碰中断/GIC（纯轮询，最简单），M2.2 才引入 GIC + 定时器——每步只新增一个变量。

## 1. 硬件事实（✅ 已从 SDK dts 核实）

| 资源 | 值 | 来源 |
|---|---|---|
| core1 = CPU 3 | MPIDR 0x300（cluster0 + cpu3），link-id 0x03 | amp-cpus `id = <0x300>` |
| 控制台串口 | **UART4 @ 0xFE680000**（官方 rk3568-amp.dtsi 指定，amp 节点自带 SCLK_UART4/PCLK_UART4 + uart4m1_xfer；kickpi 40pin 头已启用 uart4） | rk3568-amp.dtsi + rk3568.dtsi L3339 |
| GIC | **GICv3**：GICD @ 0xFD400000，GICR @ 0xFD460000 | rk3568.dtsi L683 |
| arch timer | armv8-timer，PPI 14 = **非安全物理定时器**（CNTP_*），⚠️ 频率按 CNTFRQ_EL0 读（RK3568 = 24MHz） | rk3568.dtsi L475 |
| 工具链 | SDK prebuilts `aarch64-none-linux-gnu-gcc`（已验证可编裸机 ELF） | 实测 |
| FreeRTOS 移植层 | `freertos-kernel/portable/GCC/ARM_CA53_64_BIT/`（port.c / portASM.S / portmacro.h，✅ 存在） | 实测 |

**⚠️ 待核实项**：① 板子实际 console 串口（`cat /proc/device-tree/chosen` 或看 dmesg）；② 固件入口的异常级别（ATF CPU_ON 通常从 **EL1 非安全** 进入，需与 ATF 版本核对）；③ arch timer 用 CNTP 还是 CNTV（FreeRTOS port 的选择，见 §7）。

## 2. 内存布局契约（地址表 + 验证方法）

参考 rk3588-amp.dtsi 模板，rk3568 版（✅ 已按板子实测 `/proc/iomem` 修正，见下）：

| 区域 | 物理地址 | 大小 | 用途 | 谁持有 |
|---|---|---|---|---|
| core1 固件 | 0x7000000 | 1MB | FreeRTOS 镜像（**entry 也在这**） | core1 执行 |
| amp 共享 | 0x7100000 | 4MB | 通用共享内存（预留） | 双核 |
| rpmsg vring | 0x7500000 | 128KB | vring 结构体（reg） | 双核 |
| rpmsg DMA 池 | 0x7600000 | 1MB | 消息 payload（shared-dma-pool） | 双核 |

**✅ 实测结论（8-17 板子 iomem）**：
```
00200000-083fffff : System RAM      ← 内核镜像在 0x200000（Kernel code 0x200000~01e7ffff）
09400000-7fffffff : System RAM
```
- **0x200000 被内核占用，原方案作废**；新地址 0x7000000 区段全部落在第一块 System RAM 的空闲区（高于 Kernel data 0x2a9ffff，低于 0x83fffff）
- 0x8400000-0x93fffff 是 16MB 未声明间隙（疑似 u-boot/ATF 保留），**不碰**

**验证方法（必须做）**：
```bash
# 板子上跑 Linux 后检查实际内存占用，确保上述地址不被内核/其他保留区占用
cat /proc/iomem | head -40
# 确认 0x200000 / 0x7c00000 / 0x8000000 没出现在已占用列表里
```

**链接地址铁律**：固件链接地址（`.lds` 的 `. = 0x200000`）= dts `amp-cpus` 的 `entry = <0x200000>` = 加载器放置地址。三者不一致 = core1 跑飞。

## 3. 工程骨架（core1/ 目录）

```
core1/
├── freertos-kernel/        # 已克隆（V10.6.2）
├── build/
├── Makefile                # 交叉编译：aarch64-none-linux-gnu-gcc -nostdlib
├── link.ld                 # 链接脚本（. = 0x200000）
├── startup.S               # 异常向量 + 栈初始化 + 跳 main（M2.1 手写，M2.2 换 port 的）
├── main.c                  # M2.1: 串口打印；M2.2: 创建任务 + 启动调度器
├── uart.c / uart.h         # UART4 轮询收发（打印用）
├── timer.c                 # generic timer 初始化（CNTFRQ + CNTP_CTL）
├── gic.c                   # GICv3 最小初始化（M2.2）
├── FreeRTOSConfig.h        # AArch64 port 专用配置（§7）
└── loader/                 # Linux 侧固件加载器（§8）
```

## 4. 启动代码 startup.S（M2.1 骨架）

core1 被 ATF 从 entry 拉起时的状态：EL1、MMU 关、可能 SCTLR 未初始化。最小启动要做：

```asm
.section .text.start
.global _start
_start:
    /* 1. 关 MMU/对齐检查（保险起见） */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)      /* 清 M 位关 MMU */
    msr     sctlr_el1, x0
    isb

    /* 2. 初始化栈（core1 独占一块 RAM，比如固件区尾部） */
    ldr     x0, =_stack_top
    mov     sp, x0

    /* 3. 设异常向量表（M2.1 先放空表，M2.2 用 FreeRTOS port 的） */
    ldr     x0, =_vectors
    msr     vbar_el1, x0
    isb

    /* 4. 跳 C 世界 */
    bl      main
1:  b       1b                    /* main 不应返回 */
```

要点：
- **`_stack_top` 在链接脚本里定义**（放在固件镜像末尾，栈向下生长）；
- M2.1 的异常向量表可以是"全死循环"的空表（不碰中断就不会进）；
- ⚠️ 如果 ATF 进入时在 EL2 或别的级别，`sctlr_el1` 会异常——先加 `mrs x0, currentel` 打印验证当前 EL（调试第一步）。

## 5. 链接脚本 link.ld（骨架）

```lds
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
    . = 0x7000000;                    /* ★ 链接地址 = dts entry（实测后定，0x200000 被内核占用） */
    .text : {
        KEEP(*(.text.start))         /* _start 必须放最前 */
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }

    . = ALIGN(16);
    _stack_bottom = .;
    . += 0x8000;                     /* core1 栈：32KB，够 printf 深度 */
    _stack_top = .;

    _end = .;
}
```

验证：
```bash
aarch64-none-linux-gnu-readelf -h build/core1.elf | grep Entry   # 必须 = 0x200000
aarch64-none-linux-gnu-objcopy -O binary build/core1.elf build/core1.bin
```

## 6. 最小外设初始化

### 6.1 UART4 轮询打印（uart.c）

dw-apb-uart 的轮询写（core1 控制台 = UART4 @ 0xFE680000，官方 dtsi 指定）：

```c
#define UART4_BASE   0xFE680000UL
#define UART_THR     (UART4_BASE + 0x00)   /* 发送保持寄存器 */
#define UART_LSR     (UART4_BASE + 0x14)   /* 线路状态寄存器 */
#define LSR_THRE     (1 << 5)              /* 发送保持寄存器空 */

void uart_putc(char c)
{
    while (!(readl(UART_LSR) & LSR_THRE))  /* 等发送缓冲空 */
        ;
    writel(c, UART_THR);
}
```

⚠️ **时钟问题（M2 第一大坑）**：UART2 的时钟由 CRU 提供，core1 裸机不管 CRU 的话，串口可能根本没时钟/没使能。Linux 已经把 UART2 初始化过（console 在用），**它的寄存器配置会保持**——所以 M2.1 里 UART 直接就能用（复用 Linux 的配置）。如果换别的串口/或 Linux 没初始化过，就需要在 core1 侧配 CRU——**这是 M2 最耗时的潜在坑，先确认板子 console 就是 UART2 且 Linux 在用**。

### 6.2 Generic timer（timer.c，M2.2）

```c
/* 非安全物理定时器：CNTP_* 系统寄存器，PPI 14 */
#define CNTFRQ_EL0    S3_3_C14_C0_0
#define CNTP_CTL_EL0  S3_3_C14_C2_1
#define CNTP_TVAL_EL0 S3_3_C14_C2_0

static inline uint64_t read_cntfrq(void) { uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }

void timer_init(uint32_t tick_hz)
{
    uint64_t freq = read_cntfrq();      /* ⚠️ 若返回 0，硬编码 24MHz 并写回 CNTFRQ_EL0 */
    writel(freq / tick_hz, CNTP_TVAL_EL0);   /* 设置下次触发间隔 */
    asm volatile("msr cntp_ctl_el0, %0" :: "r"(1));  /* 使能，非屏蔽 */
}
```

### 6.3 GICv3 最小初始化（gic.c，M2.2）

core1 要收到 PPI 14（定时器中断），GIC 必须：使能分发器 + 使能本核的重分发器 + 配置并使能 PPI。

```c
#define GICD_BASE  0xFD400000UL
#define GICR_BASE  0xFD460000UL
/* GICv3 关键寄存器（详细位域按 ARM GICv3 手册 + RK3568 TRM 核对）：
   GICD_CTLR: 使能 Group0/Group1
   GICR_WAKER: 清除 ProcessorSleep，等 ChildrenAsleep 清
   GICR_IGROUPRn / GICR_IPRIORITYRn / GICR_ISPENDRn / GICR_ISENABLERn: PPI 配置
   PPI 14 → 每核私有，重分发器按本核 MPIDR 寻址 */
void gic_init(void)
{
    /* 1. 使能 GICD（分发器） */
    /* 2. 定位本核 GICR（按 MPIDR 的 Affinity 找到对应重分发器页） */
    /* 3. GICR_WAKER 唤醒本核 */
    /* 4. PPI 14：清 pending → 设优先级 → 使能 */
    /* 5. ICC_IGRPEN1_EL1 使能 CPU 接口（GICv3 用系统寄存器！） */
}
```

⚠️ GICv3 重分发器初始化和 CPU 接口（`ICC_*` 系统寄存器）是 **M2.2 最硬的部分**——优先级、安全分组、亲和路由都在这。建议：先只服务于定时器 PPI，跑通 tick 后再扩展。

## 7. FreeRTOS 接入（M2.2）

### 7.1 用官方的 AArch64 移植层

`portable/GCC/ARM_CA53_64_BIT/` 已存在，含：`port.c`（含 `vPortSetupTimerInterrupt`，L334 附近"Start the timer that generates the tick ISR"）、`portASM.S`（EL1 异常入口、首任务启动）、`portmacro.h`。

⚠️ **待核实**：该 port 的定时器用的是 CNTP 还是 CNTV、异常向量在 portASM.S 里的形态——决定了我们 `startup.S` 的向量表是"用自己的"还是"跳转进 port 的"（推荐后者：让 port 接管 IRQ）。

### 7.2 FreeRTOSConfig.h 关键项（AArch64 专属）

```c
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1   /* AArch64 有 clz，可用位图快速选任务 */
#define configUNIQUE_INTERRUPT_PRIORITIES       256  /* GIC 8 位优先级 */
#define configMAX_API_CALL_INTERRUPT_PRIORITY   128  /* 高于此优先级的中断不能调 API（port.c 编译期检查） */
#define configTICK_RATE_HZ                      1000
#define configMINIMAL_STACK_SIZE                512   /* 词（4B）；AArch64 port 栈要求见其文档 */
#define configUSE_TIMERS                        1
#define configSUPPORT_STATIC_ALLOCATION         1     /* 用静态分配，避开裸机堆实现 */
```

### 7.3 内存分配

裸核没有 malloc——`heap_4.c`（`portable/MemMang/`）+ 静态 `ucHeap[configTOTAL_HEAP_SIZE]`，或用 `configSUPPORT_STATIC_ALLOCATION=1` 全静态。**M2 推荐静态分配**（少一个变量）。

## 8. Linux 侧配合（dts + 加载器）

### 8.1 dts 修改（在 kickpi 板级 dts 上加 amp 节点）

```dts
/ {
    reserved-memory {
        #address-cells = <2>; #size-cells = <2>; ranges;
        amp_shmem: amp-shmem@7100000 { reg = <0x0 0x7100000 0x0 0x400000>; no-map; };
        rpmsg_reserved: rpmsg@7500000  { reg = <0x0 0x7500000 0x0 0x20000>;  no-map; };
        rpmsg_dma_reserved: rpmsg-dma@7600000 { compatible = "shared-dma-pool";
                                                reg = <0x0 0x7600000 0x0 0x100000>; no-map; };
    };
    rockchip-amp {
        compatible = "rockchip,rk3568-amp";
        status = "okay";
        amp-cpus {
            cpu@3 {
                id = <0x300>;
                entry = <0x7000000>;     /* ★ = 固件链接地址（实测后定） */
                mode = <1>;              /* ⚠️ 与 ATF 约定的模式值，先查 SIP 协议 */
                boot-on = <0>;           /* M2 阶段手动开核，不要自动启动 */
            };
        };
    };
};
```

### 8.2 固件加载器（loader/，把 core1.bin 送到 0x200000）

Linux 侧用 `/dev/mem` 把固件拷进保留区（no-map 区 /dev/mem 仍可 mmap）：

```c
int fd = open("/dev/mem", O_RDWR | O_SYNC);
void *dst = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x200000);
memcpy(dst, core1_bin, size);     /* 固件就位 */
munmap(dst, size);
```

### 8.3 开核

```bash
echo on 3 > /sys/rk_amp/boot_cpu     # → boot_cpu_store → SMC → ATF CPU_ON(3, 0x200000)
# 然后看 UART2 串口输出
```

## 9. 构建 / 运行 / 验收流程

```bash
# 1. 构建 core1 固件
make -C core1                        # 产出 build/core1.bin
aarch64-none-linux-gnu-readelf -h build/core1.elf | grep Entry   # 必须 0x7000000

# 2. 板子上：编译加载器 + 拷固件 + 开核
#    （Loader 先跑，再 echo on 3）

# 3. 串口期望输出（M2.1）
#    hello from core1 (cpu=3, el=1, freq=24000000)

# 4. 串口期望输出（M2.2）
#    taskA: count=1
#    taskB: count=1
#    taskA: count=2   ... 周期稳定 = tick 正常
```

**验收判据**：① 串口有输出；② 任务按 `vTaskDelay` 周期打印且顺序符合优先级；③（M2.2 加分）`uxTaskGetStackHighWaterMark` 打印不异常。

## 10. 调试工具箱（跑飞了怎么查）

| 症状 | 排查顺序 |
|---|---|
| 串口无输出 | ① console 串口对不对（UART2?）② 固件真加载到 0x200000 了？（loader 后 `devmem 0x200000` 读前 8 字节对比）③ entry 与链接地址一致？ |
| 打印乱码 | 波特率/时钟不对——UART 复用 Linux 配置应没问题，检查是不是换过串口 |
| 进 main 就崩 | 栈没设对/栈太小；先 `mrs x0, currentel` 打印当前 EL |
| 任务不跑/卡死 | tick 没来——GIC PPI14 没使能，或 CNTP_CTL 没开；读回 GICD/GICR 寄存器确认 |
| 偶发数据错 | ⚠️ 又是内存屏障：core1 写共享内存 + 写 mailbox 之间必须有 `dmb`（Linux 侧学到的那课） |

**核心排查手段**：`devmem`（Linux 侧）读任意物理地址 + `cat /proc/iomem` 看占用 + 串口打印当作"裸机 printf"用。

## 11. 风险与待核实项清单

- [ ] 板子实际 console 串口地址（UART2 @ FE660000 是默认，待确认）
- [x] 保留区地址与 `/proc/iomem` 实测避让 ✅ 8-17：内核占 0x200000，固件改址 0x7000000
- [ ] ATF 拉起 core1 的异常级别与状态（假定 EL1 非安全、MMU 关）
- [ ] `mode = <1>` 的语义（Rockchip SIP 协议，查 ATF 源码或文档）
- [ ] FreeRTOS ARM_CA53_64_BIT port 的定时器选择（CNTP/CNTV）与向量表形态
- [ ] RK3568 system counter 频率（假定 24MHz，读 CNTFRQ_EL0 验证）

## 12. 行动顺序（建议节奏）

1. **半天**：做 §2 的 `/proc/iomem` 实测 + 确认 console 串口 → 敲定地址契约
2. **1 天**：M2.1 裸机点亮（startup + lds + uart + loader + 开核）→ 串口出 "hello"
3. **2 天**：M2.2（GIC + timer + FreeRTOS port 接入）→ 任务周期打印
4. **后续**：M3（A2B 中断 + vring，把 Linux 侧学到的东西镜像到 core1）

---

*本文档为活文档：每核实一项就在 §11 打勾，每踩一个坑按日志模板记录。*
