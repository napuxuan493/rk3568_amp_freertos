/* M2.2: core1 FreeRTOS 入口 —— 2 个任务 + tick 调度
 * M3: 新增 rpmsg_task —— 双向 rpmsg 通信（mailbox + vring）
 */
#include "FreeRTOS.h"
#include "task.h"
#include "uart.h"
#include <stdint.h>
#include <queue.h>
#include "rpmsg_core.h"
#include "rpmsg_mbox.h"
#include "rpmsg_vring.h"

void gic_init(void);

static inline uint64_t read_currentel(void)
{
    uint64_t v;
    asm volatile("mrs %0, currentel" : "=r"(v));
    return v;
}
static uint8_t q_storage[4 * sizeof(uint32_t)];
static StaticQueue_t q_mem;
QueueHandle_t q;

/* ── 任务 1：每 500ms 打印 ── */
static StackType_t task1_stack[configMINIMAL_STACK_SIZE * 2];
static StaticTask_t task1_tcb;

static void vTask1(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWake = xTaskGetTickCount();
    uint32_t active_tx_count = 0;
    for (;;) {
        /* 原本的存活打印，证明任务在跑 */
        uart_puts("[t1] :tick from task1\r\n");

        /* ── 新增：主动发送逻辑 ── */
        /* 必须确保已经握手成功，且获取到了 Linux 端的端点地址 */
        if (rpmsg_ready && rpmsg_have_linux_ept()) {
            char msg[64];
            int i;
            const char prefix[] = "Active msg from FreeRTOS: ";
            
            /* 简单组装一个字符串，不使用耗时的 sprintf */
            for (i = 0; i < (int)(sizeof(prefix) - 1); i++) {
                msg[i] = prefix[i];
            }
            msg[i++] = 'A' + (active_tx_count % 26); /* 附加一个变化字母 */
            msg[i++] = '\0';

            /* 向 Linux 端发送 */
            if (rpmsg_send(rpmsg_linux_ept(), msg, i) > 0) {
                uart_diag_mark("Task1 TX OK");
            }
            active_tx_count++;
        }

        /* 每 2 秒主动发一次 */
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(2000));   
    }
    // (void)pvParameters;
    // for (;;) {
    //     uart_puts("[t1] hello from task1 (500ms)\r\n");
    //     vTaskDelay(pdMS_TO_TICKS(500));
    // }
}

/* ── 任务 2：每 1000ms 打印 tick 计数（证明 tick 在走） ── */
static StackType_t task2_stack[configMINIMAL_STACK_SIZE * 2];
static StaticTask_t task2_tcb;

static void vTask2(void *pvParameters)
{
    (void)pvParameters;
    for (;;) {
        uint32_t tick;
        if (xQueueReceive(q, &tick, 10) == pdTRUE) {
            uart_puts("[t2] tick = ");
            uart_putdec(tick);
            uart_puts("\r\n");
        }
        // vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ── 空闲任务内存（静态分配必需） ── */
static StackType_t idle_task_stack[configMINIMAL_STACK_SIZE];
static StaticTask_t idle_task_tcb;


// /* 声明队列对象和本地游标 */
// static struct vring rx_vr;       /* 对应 VQ1 (接收队列) */
// static struct vring tx_vr;       /* 对应 VQ0 (发送队列) */
// static uint16_t rx_last_avail = 0;
// static uint16_t tx_last_avail = 0; /* 如果需要处理 TX 回调 */

static void vRpmsgTask(void *pvParameters)
{
    (void)pvParameters;

    /* 改用 UART4 打印，让你能在主终端直接看到进度！ */
    uart_puts("[RPMsg] Waiting Linux handshake...\r\n"); 
    
    if (rpmsg_init() < 0) {
        uart_puts("[RPMsg] init failed\r\n");
    } else {
        uart_puts("[RPMsg] Handshake OK, entering loop\r\n");
    }

    for (;;) {
        /* 1. 死等 222 号中断 (Mailbox A2B 通道 3) 唤醒 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* 2. 醒来后，直接调用 rpmsg_core.c 里的协议层轮询函数。
         * 它内部会自动榨干 vq1，剥离 RPMsg 协议头，调用 rpmsg_on_recv 回显，并踢 TX 门铃 */
        rpmsg_poll(); 
    }
}

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &idle_task_tcb;
    *ppxIdleTaskStackBuffer = idle_task_stack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* ── 栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW=2 需要） ── */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    uart_puts("\r\n[FATAL] STACK OVERFLOW in task: ");
    uart_puts(pcTaskName);
    uart_puts("\r\n");
    for (;;)
        ;
}

/* ── IRQ 入口：portASM.S 保存上下文后调用（W0 = ack 的 INTID） ── */
static uint32_t irq_log_count;
/* 定义任务句柄，供 ISR 唤醒时使用 */
TaskHandle_t xRpmsgTaskHandle = NULL;

void vApplicationIRQHandler(uint32_t ulICCIAR)
{
    uint32_t intid = ulICCIAR & 0x3FFUL;

    /* 定时器 PPI = INTID 30（CNTP，实测）。打印 1 次便于确认 */
    if (intid == 30) {
        if (irq_log_count == 0) {
            uart_puts("[irq] intid=30\r\n");
            irq_log_count = 1;
        }
        FreeRTOS_Tick_Handler();    /* port.c 里：清 tick + xTaskIncrementTick */
    }





    /* ── 2. 处理 Linux 发来的 Mailbox 门铃 ── */
    else if (intid == 222) { /* Mailbox A2B_CH3 */
        /* 1. 必须先清硬件挂起标志 (撤销高电平) */
        *(volatile uint32_t *)(0xFE780004) = (1u << MBOX_A2B_CHAN); 

        /* 2. 唤醒 RPMsg 处理任务 */
        if (xRpmsgTaskHandle != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            
            /* 给任务发送通知，相当于给二值信号量做 Give 操作 */
            vTaskNotifyGiveFromISR(xRpmsgTaskHandle, &xHigherPriorityTaskWoken);
            
            /* 如果唤醒的任务优先级比当前被打断的任务高，触发上下文切换 */
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }
}



/* 1. 静态分配任务的栈 (注意单位是字 StackType_t，在 64 位系统上通常是 8 字节) */
#define RPMSG_STACK_SIZE 1024
static StackType_t xRpmsgTaskStack[RPMSG_STACK_SIZE];

/* 2. 静态分配任务控制块 (TCB) */
static StaticTask_t xRpmsgTaskTCB;
/* ── 入口（startup.S 关 MMU/缓存后跳到这里） ── */
void main(void)
{
    /* 探针2：main 到达标记（0x7002010，"MAIN"） */
    // *(volatile uint32_t *)0x7002010UL = 0x4E49414DUL;

    uart_init();
    uart_diag();                    /* M2.2 诊断：UART2 打印 UART4 配置 */
    uart_puts("\r\n=== core1 FreeRTOS (M2.2) ===\r\n");
    uart_puts("el = EL");
    uart_putdec((uint32_t)(read_currentel() >> 2));
    uart_puts("\r\n");
    gic_init();                         /* GICv3：GICD + GICR + PPI14 + ICC_* */
    q= xQueueCreateStatic(4, sizeof(uint32_t),q_storage,&q_mem);  /* 让编译器生成 queue.c 的静态内存 */
    xTaskCreateStatic(vTask1, "t1", configMINIMAL_STACK_SIZE * 2, NULL,2,
                      task1_stack, &task1_tcb);
    xTaskCreateStatic(vTask2, "t2", configMINIMAL_STACK_SIZE * 2, NULL,1,
                      task2_stack, &task2_tcb);
    // xTaskCreateStatic(vRpmsgTask, "rpmsg", configMINIMAL_STACK_SIZE * 2, NULL,2,
    //                   rpmsg_task_stack, &rpmsg_task_tcb);
    xRpmsgTaskHandle = xTaskCreateStatic(
        vRpmsgTask,           /* 任务执行的函数入口 */
        "RPMsgTask",          /* 任务名字 */
        RPMSG_STACK_SIZE,     /* 栈深度 (以字为单位) */
        NULL,                 /* 传递给任务的参数 */
        3,                    /* 任务优先级 */
        xRpmsgTaskStack,      /* 任务栈的起始地址 */
        &xRpmsgTaskTCB        /* 任务控制块的地址 */
    );
    
    // 创建成功的话，xRpmsgTaskHandle 将不再是 NULL，它直接指向 xRpmsgTaskTCB


    /* 内部流程：装向量表(VBAR) → configSETUP_TICK_INTERRUPT() → 首任务 ERET
     * 首任务的 PSTATE(0x04=EL1t) 会清掉 DAIF.I → IRQ 使能 → tick 开始 */
    vTaskStartScheduler();

    /* 不应到达 */
    for (;;)
        ;
}
