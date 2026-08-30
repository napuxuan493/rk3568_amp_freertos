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
    for (;;) {
        xQueueSend(q, &xLastWake,10);  /* 发送当前 tick 给任务2 */
        uart_puts("[t1] :tick from task1：");
        uart_putdec((uint32_t)xLastWake);
        uart_puts("\r\n");
         vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(1000));   /* 绝对周期，不累积漂移 */
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

/* ── M3: rpmsg 任务（mailbox 轮询 + vring 收发 + 周期发送） ── */
static StackType_t rpmsg_task_stack[configMINIMAL_STACK_SIZE * 2];
static StaticTask_t rpmsg_task_tcb;
static uint32_t rpmsg_tx_count;

/* 声明队列对象和本地游标 */
static struct vring rx_vr;       /* 对应 VQ1 (接收队列) */
static struct vring tx_vr;       /* 对应 VQ0 (发送队列) */
static uint16_t rx_last_avail = 0;
static uint16_t tx_last_avail = 0; /* 如果需要处理 TX 回调 */

static void vRpmsgTask(void *pvParameters)
{
    (void)pvParameters;
    TickType_t xLastWake = xTaskGetTickCount();

    uart_diag_mark("R1 waiting handshake");
    if (rpmsg_init() < 0) {
        uart_diag_puts("[rpmsg] init failed, poll retry\r\n");
    } else {
        uart_diag_mark("R2 ready");
    }

    for (;;) {
        // rpmsg_poll();               /* 处理 A2B 门铃 + 收 vq1 消息（含回显） */


        /* 1. 初始化队列视图 (内存地址和布局完全对齐你的宏) */
    vring_init(&tx_vr, VQ0_BASE);
    vring_init(&rx_vr, VQ1_BASE);

    /* 2. 告诉 Linux 我们准备好了 (可选：触发一次门铃，让 Linux 知道 slave 上线了) */
    // mbox_send_doorbell();

    /* 3. 进入事件驱动的死循环 */
    while (1) {
        /* 死等 Mailbox ISR 发来的唤醒通知 (参数 pdTRUE = 唤醒后清零通知状态) */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* 
         * 醒来了！这说明 Linux 敲响了 222 号中断门铃。
         * 核心逻辑：必须用 while 循环“榨干” avail 队列，防遗漏！
         */
        uint16_t desc_idx;
        
        while (vring_get_avail(&rx_vr, &rx_last_avail, &desc_idx) == 0) {
            
            /* 拿到缓冲区的物理地址，准备读数据 */
            struct vring_desc *desc = &rx_vr.desc[desc_idx];
            void *payload = (void *)desc->addr;
            uint32_t payload_len = desc->len;
            
            /* [业务逻辑]：处理收到的 RPMsg 数据... */
            // process_rpmsg(payload, payload_len);

            /* 消费完毕，把缓冲区块归还给 used 队列 */
            vring_put_used(&rx_vr, desc_idx, payload_len);
        }

        /* 
         * 当 while 循环退出，说明 rx 队列彻底空了。
         * 此时敲响 TX 门铃，通知 Linux 去读 used 队列回收内存！
         */
        mbox_kick(); /* 往 0 号通道发门铃中断 (218) */
    }

        // /* 每秒发一条 "hello" 给 Linux（演示 core1→Linux 方向） */
        // if (rpmsg_ready) {
        //     char msg[32];
        //     int n;
        //     for (n = 0; n < 32; n++) msg[n] = 0;
        //     /* "hello from core1: N" */
        //     {
        //         static const char p[] = "hello from core1: ";
        //         int i;
        //         for (i = 0; i < (int)sizeof(p) - 1 && i < 16; i++)
        //             msg[i] = p[i];
        //         {
        //             uint32_t v = rpmsg_tx_count++;
        //             char buf[12];
        //             int j = 0;
        //             if (v == 0) { buf[j++] = '0'; }
        //             else { char tmp[12]; int k = 0;
        //                    while (v) { tmp[k++] = '0' + (v % 10); v /= 10; }
        //                    while (k) buf[j++] = tmp[--k]; }
        //             for (int i = 0; i < j && 16 + i < 31; i++)
        //                 msg[16 + i] = buf[i];
        //         }
        //     }
        //     /* 需要知道 Linux ept 地址；还没学到就跳过（握手后 Linux 会先发） */
        //     if (rpmsg_have_linux_ept()) {
        //         if (rpmsg_send(rpmsg_linux_ept(), msg, 24) > 0)
        //             uart_diag_mark("R3 tx ok");
        //     }
        // }
        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(1000));
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
    *(volatile uint32_t *)0x7002010UL = 0x4E49414DUL;

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
    xTaskCreateStatic(vRpmsgTask, "rpmsg", configMINIMAL_STACK_SIZE * 2, NULL,2,
                      rpmsg_task_stack, &rpmsg_task_tcb);
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
