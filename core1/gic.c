/* M2.2: GICv3 最小初始化（core1 = 物理 core3）
 *
 * RK3568 的 GIC 是 GICv3（GIC-500）：
 *   GICD @ 0xFD400000（分发器，MMIO）
 *   GICR @ 0xFD460000（重分发器，每核 128KB：RD 帧 + SGI/PPI 帧）
 *   CPU 接口 = ICC_*_EL1 系统寄存器（不是 MMIO！）
 *
 * 调试结论（2026-08-22）：
 *  - GIC 实现 4 位优先级（写 0xFF 读回 0xF0）→ configUNIQUE=16
 *  - GICD_IPRIORITYR0 只覆盖 SPI；SGI/PPI 优先级在重分发器里
 *    （vPortValidateInterruptPriority 的偏移已由 port.c 改为 0x420）
 *  - 内核跑 VHE(EL2)，其 CNTP_EL1 重定向到 CNTHP_EL2 → INTID 26；
 *    我们在 EL1 用 CNTP_EL0，实际 PPI 待实测 → 先全部使能 16-31
 */
#include <stdint.h>
#include "uart.h"

#define GICD_BASE       0xFD400000UL
#define GICR_BASE       0xFD460000UL
#define GICR_STRIDE     0x20000UL      /* 每核重分发器 128KB */

/* core3（MPIDR 0x300）→ 第 3 个重分发器 */
#define MY_GICR         (GICR_BASE + 3 * GICR_STRIDE)

#define GICD_CTLR       (GICD_BASE + 0x0000UL)

/* GICR RD 帧 */
#define GICR_WAKER      (MY_GICR + 0x0014UL)

/* GICR SGI/PPI 帧 */
#define GICR_IGROUPR0     (MY_GICR + 0x10080UL)
#define GICR_ISENABLER0   (MY_GICR + 0x10100UL)
#define GICR_ICPENDR0     (MY_GICR + 0x10280UL)
#define GICR_IPRIORITYR0  (MY_GICR + 0x10400UL)
#define GICR_ICFGR0       (MY_GICR + 0x10C00UL)
#define GICR_ICFGR1       (MY_GICR + 0x10C04UL)
static inline uint32_t gic_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void gic_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

void gic_init(void)
{
    uint32_t v, ppi;

    /* 1. GICD：使能 Group1（非安全组）。EL1 非安全视角只有 bit1 可写 */
    v = gic_read32(GICD_CTLR);
    v |= (1u << 1);                     /* EnableGrp1NS */
    gic_write32(GICD_CTLR, v);

    /* 2. GICR：唤醒本核重分发器（清 ProcessorSleep，等 ChildrenAsleep 清） */
    v = gic_read32(GICR_WAKER);
    v &= ~(1u << 1);
    gic_write32(GICR_WAKER, v);
    {
        uint32_t spins = 0;
        while ((gic_read32(GICR_WAKER) & (1u << 2)) && spins < 100000u)
            spins++;
    }

    /* 3. 定时器 PPI = INTID 30（= dts GIC_PPI 14 = CNTP 非安全物理定时器，
     *    2026-08-22 实测：ppi对应的中断号位30。 */
    #define TIMER_PPI   30
    gic_write32(GICR_ICPENDR0, (1u << TIMER_PPI));        /* 清 pending */

    v = gic_read32(GICR_IGROUPR0);
    v |= (1u << TIMER_PPI);                               /* Group1 */
    gic_write32(GICR_IGROUPR0, v);

    {
        uint32_t pri_reg = GICR_IPRIORITYR0 + (TIMER_PPI / 4) * 4;
        uint32_t lane   = (TIMER_PPI % 4) * 8;
        v = gic_read32(pri_reg);
        v = (v & ~(0xFFu << lane)) | (0xE0u << lane);
        gic_write32(pri_reg, v);
    }

    // 然后对着 ICFGR1 进行读写：
    v = gic_read32(GICR_ICFGR1);
    v &= ~(0x3u << ((TIMER_PPI % 16) * 2));  /* 彻底清零第 28 和 29 位 */
    gic_write32(GICR_ICFGR1, v);







    /* ==========================================
     * 二. 配置 Mailbox A2B 通道 3 (INTID 222, SPI)
     * ========================================== */
    #define MBOX_INTID 222
    uint32_t mbox_idx = MBOX_INTID / 32;       /* = 6 */
    uint32_t mbox_bit = MBOX_INTID % 32;       /* = 30 */

    /* (1) 跨核心路由 (IROUTER) - 将 222 路由给 Core 3 (MPIDR = 0x300) 
     * 注意：IROUTER 是 64 位寄存器，如果你的 gic_write64 没实现，可以直接用指针强转 */
    *(volatile uint64_t *)(GICD_BASE + 0x6000 + MBOX_INTID * 8) = 0x300ULL;

    /* (2) 分配到非安全 Group 1 (GICD_IGROUPR6) */
    v = gic_read32(GICD_BASE + 0x0080 + mbox_idx * 4);
    v |= (1u << mbox_bit);
    gic_write32(GICD_BASE + 0x0080 + mbox_idx * 4, v);

    /* (3) 配置为电平触发 (GICD_ICFGR13) */
    uint32_t cfg_reg = GICD_BASE + 0x0C00 + (MBOX_INTID / 16) * 4;  /* 222/16 = 13 */
    uint32_t cfg_bit = (MBOX_INTID % 16) * 2;                       /* 14*2 = 28 */
    v = gic_read32(cfg_reg);
    v &= ~(0x3u << cfg_bit); 
    gic_write32(cfg_reg, v);

    /* (4) 设置优先级为 0xE0 (GICD_IPRIORITYR55) */
    uint32_t pri_reg = GICD_BASE + 0x0400 + (MBOX_INTID / 4) * 4;   /* 222/4 = 55 */
    uint32_t pri_lane = (MBOX_INTID % 4) * 8;                       /* 2*8 = 16 */
    v = gic_read32(pri_reg);
    v = (v & ~(0xFFu << pri_lane)) | (0xE0u << pri_lane);
    gic_write32(pri_reg, v);

    /* (5) 使能 Mailbox 门铃中断 (GICD_ISENABLER6) */
    v = gic_read32(GICD_BASE + 0x0100 + mbox_idx * 4);
    v |= (1u << mbox_bit);
    gic_write32(GICD_BASE + 0x0100 + mbox_idx * 4, v);

    gic_write32(GICR_ISENABLER0, (1u << TIMER_PPI));      /* 使能 */







    /*修复rtos中断优先级有效位检测 */
    /* 4. SPI 优先级：vPortValidateInterruptPriority（port.c）读 GICD_IPRIORITYR8
     *    的置位位来校验优先级位数；必须非 0 否则死循环。 */
    gic_write32(GICD_BASE + 0x0420, 0xFFFFFFFFu);

    /* 5. CPU 接口（GICv3 = 系统寄存器） */
    __asm volatile("msr s3_0_c12_c12_5, %0" :: "r"(0x07UL));   /* ICC_SRE_EL1: SRE=1,DFB=1,DIB=1 */
    __asm volatile("isb");
    __asm volatile("msr s3_0_c4_c6_0, %0"  :: "r"(0xFFUL));    /* ICC_PMR_EL1: 全放行 */
    __asm volatile("msr s3_0_c12_c12_3, %0" :: "r"(0UL));      /* ICC_BPR1_EL1: 0 */
    __asm volatile("msr s3_0_c12_c12_7, %0" :: "r"(1UL));      /* ICC_IGRPEN1_EL1: 使能 Group1 */
    __asm volatile("isb");

    /* M2.2 修复(8/24)：GIC settle。实测 gic_init 完成后立即启动调度器，
     * 首次 tick 竞态导致 core1 静默卡死（banner 后无输出）。~3ms 等待让
     * GICD/GICR/CPU 接口完全就绪后再 arm tick/开 IRQ。 */
    for (volatile uint32_t d = 0; d < 3000000u; d++) ;
}
