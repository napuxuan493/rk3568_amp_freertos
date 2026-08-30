/* M2.2: FreeRTOS AArch64 (ARM_CA53_64_BIT port) 配置
 * - core1 在 EL1 非安全运行 → 编译 port 时必须定义 GUEST（Makefile -DGUEST）
 * - tick 用非安全物理定时器 CNTP_EL0（PPI 14，level 触发，24MHz）
 * - GICv3：CPU 接口走 ICC_*_EL1 系统寄存器（port 已适配，见 port.c/portASM.S）
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
#include "uart.h"

/* 告诉 FreeRTOS 底层 port.c：我们使用的是 GICv3 硬件中断控制器 */
#define GICv3_PRESENT    1
/*-----------------------------------------------------------
 * 时钟与 tick
 *-----------------------------------------------------------*/
#define configCPU_CLOCK_HZ                  24000000UL  /* 已实测 cntfrq */
#define configTICK_RATE_HZ                  1000        /* 1ms tick */
#define configUSE_16_BIT_TICKS              0

/*-----------------------------------------------------------
 * 调度
 *-----------------------------------------------------------*/
#define configUSE_PREEMPTION                1
#define configUSE_TIME_SLICING              1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1   /* AArch64 有 clz，位图选任务 */
#define configMAX_PRIORITIES                5
#define configIDLE_SHOULD_YIELD             1
#define configUSE_TASK_NOTIFICATIONS        1
#define configUSE_MUTEXES                   0
#define configUSE_RECURSIVE_MUTEXES         0
#define configUSE_COUNTING_SEMAPHORES       0
#define configUSE_QUEUE_SETS                0
#define configUSE_EVENT_GROUPS              0
#define configUSE_TIMERS                    0       /* M2.2 不引入软件定时器 */
#define configUSE_CO_ROUTINES               0
#define configUSE_POSIX_ERRNO               0

/*-----------------------------------------------------------
 * 内存分配：全静态（裸机无 malloc）
 *-----------------------------------------------------------*/
#define configSUPPORT_STATIC_ALLOCATION     1
#define configSUPPORT_DYNAMIC_ALLOCATION    0
#define configTOTAL_HEAP_SIZE               0
#define configMINIMAL_STACK_SIZE            512     /* 单位：uint64_t 字（=4KB） */
#define configMAX_TASK_NAME_LEN             12

/*-----------------------------------------------------------
 * 钩子与检查
 *-----------------------------------------------------------*/
#define configUSE_IDLE_HOOK                 0
#define configUSE_TICK_HOOK                 0
#define configCHECK_FOR_STACK_OVERFLOW      2
/* configASSERT 打 UART2 调试口（uart_diag_puts），避免 UART4 上看不到 */
/* 替换 FreeRTOSConfig.h 中的 configASSERT */
#define configASSERT(x) \
    do { \
        if (!(x)) { \
            /* 禁用中断，防止被打断 */ \
            __asm volatile("msr daifset, #3" ::: "memory"); \
            uart_puts("\r\n[FATAL] ASSERT FAILED!\r\n"); \
            uart_puts("File: "); \
            uart_puts(__FILE__); \
            uart_puts("\r\nLine: "); \
            uart_putdec(__LINE__); \
            uart_puts("\r\n"); \
            /* 死循环锁死，方便排查 */ \
            for (;;) ; \
        } \
    } while (0)

/*-----------------------------------------------------------
 * GIC（portmacro.h 需要这些；CPU 接口已适配为 ICC_* 系统寄存器）
 *-----------------------------------------------------------*/
#define configINTERRUPT_CONTROLLER_BASE_ADDRESS          0xFD400000UL  /* GICD */
#define configINTERRUPT_CONTROLLER_CPU_INTERFACE_OFFSET  0x10000UL
/* M2.2 实测（2026-08-22）：写 0xFF 到 GICD_IPRIORITYR8 只留下 0xF0——
 * RK3568 的 GIC 实现 4 位优先级（16 级），不是 8 位！ */
#define configUNIQUE_INTERRUPT_PRIORITIES                16      /* GIC 4 位优先级 */
#define configMAX_API_CALL_INTERRUPT_PRIORITY            12      /* >16/2，编译期检查 */

/*-----------------------------------------------------------
 * tick 中断（port.c 在 xPortStartScheduler 里调用 SETUP，
 * 每个 tick 在 FreeRTOS_Tick_Handler 里调用 CLEAR）
 *-----------------------------------------------------------*/
void vPortSetupTimerInterrupt(void);    /* timer.c：使能 CNTP + 装填 */
void vPortClearTickInterrupt(void);     /* timer.c：重新装填 CNTP */
#define configSETUP_TICK_INTERRUPT()    vPortSetupTimerInterrupt()
#define configCLEAR_TICK_INTERRUPT()    vPortClearTickInterrupt()

/*-----------------------------------------------------------
 * IRQ 入口：portASM.S 保存上下文后调用，参数 = ack 的 INTID
 *-----------------------------------------------------------*/
void vApplicationIRQHandler(uint32_t ulICCIAR);

/*-----------------------------------------------------------
 * INCLUDE 开关（V10 仍然按函数门控）
 *-----------------------------------------------------------*/
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    0
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     0
#define INCLUDE_xTaskGetIdleTaskHandle          0
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTaskGetTickCount               1

/* 兼容旧宏 */
#define configKERNEL_INTERRUPT_PRIORITY         15
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    12


#endif /* FREERTOS_CONFIG_H */
