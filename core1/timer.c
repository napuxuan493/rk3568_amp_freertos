/* M2.2: Generic timer（非安全物理定时器 CNTP_EL0，PPI 14）
 *
 * FreeRTOS port 通过 configSETUP_TICK_INTERRUPT / configCLEAR_TICK_INTERRUPT
 * 调用这里（声明在 FreeRTOSConfig.h）。
 */
#include <stdint.h>
#include "FreeRTOS.h"

#define CNTP_TVAL_EL0   S3_3_C14_C2_0   /* 计数值（倒数） */
#define CNTP_CTL_EL0    S3_3_C14_C2_1   /* 控制：bit0 EN, bit1 IMASK, bit2 ISTATUS */
#define CNTFRQ_EL0      S3_3_C14_C0_0

static uint64_t tick_period;    /* 每个 tick 的计数器增量 */

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* configSETUP_TICK_INTERRUPT()：xPortStartScheduler 里、中断关闭时调用 */
void vPortSetupTimerInterrupt(void)
{
    uint64_t freq = read_cntfrq();
    if (freq == 0)
        freq = 24000000UL;          /* 实测 cntfrq=24MHz，兜底 */

    tick_period = freq / configTICK_RATE_HZ;

    __asm volatile("msr cntp_tval_el0, %0" :: "r"(tick_period));   /* 装填 */
    __asm volatile("msr cntp_ctl_el0, %0"  :: "r"(1UL));           /* EN=1, IMASK=0 */
    __asm volatile("isb");
}

/* configCLEAR_TICK_INTERRUPT()：FreeRTOS_Tick_Handler 里、进 tick 前调用
 * 重新装填 TVAL → 定时器条件解除（ISTATUS 清 0）→ GIC 电平不再有效 */
void vPortClearTickInterrupt(void)
{
    __asm volatile("msr cntp_tval_el0, %0" :: "r"(tick_period));
    __asm volatile("msr cntp_ctl_el0, %0"  :: "r"(1UL));
    __asm volatile("isb");
}
