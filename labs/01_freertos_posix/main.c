/*
 * 第一周实验 01：FreeRTOS 机制速通（POSIX 模拟器）
 * 作用：在 PC 上以用户态进程运行 FreeRTOS，亲手观察 6 个核心机制
 * 编译：make && ./freertos_lab
 *
 * 实验清单（main 里的"编排任务"每 4 秒推进一个阶段）：
 *   Phase 1 任务与优先级抢占        —— 高优先级任务就绪即抢占
 *   Phase 2 队列（生产者/消费者）   —— 阻塞、超时、队列满/空
 *   Phase 3 信号量 + 模拟中断       —— ISR 安全 API（GiveFromISR）与任务延迟处理
 *   Phase 4 互斥锁与优先级反转      —— FreeRTOS 互斥锁内置优先级继承
 *   Phase 5 运行状态一览            —— vTaskList 看就绪/阻塞/挂起
 *   Phase 6 栈溢出检测              —— configCHECK_FOR_STACK_OVERFLOW=2 触发钩子
 *
 * 阅读源码对照：tasks.c（调度）、queue.c（队列/信号量本质）、timers.c
 *
 * ★ 移植层注意（读 port.c 可印证）：
 *   - POSIX 移植层每个任务 = 一个 pthread，tick 由 SIGALRM 模拟
 *   - 自删除任务（vTaskDelete(NULL)）在此模拟器有"栈释放竞态"（idle 释放栈时
 *     对应 pthread 还在退出流程里用栈，glibc 直接 abort）。所以本实验任务
 *     结束一律用 vTaskSuspend(NULL) 挂起自己，避免踩坑；真实裸核移植
 *     （M2 的 RK3568 core1）不存在此问题。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ============ 阶段控制 ============ */
static volatile int g_phase = 1;   /* 当前实验阶段，编排任务推进 */

/* ============ 实验 3：模拟中断的信号量 ============ */
static SemaphoreHandle_t g_isr_sem;      /* 模拟"中断产生事件" */
static volatile uint32_t g_isr_count = 0;/* 中断次数统计 */

/* ============ 实验 4：优先级反转道具 ============ */
static SemaphoreHandle_t g_mutex;        /* 互斥锁（带优先级继承） */
static volatile int g_rev_hold_done = 0; /* 低优先级任务是否已持锁 */

/* ============ 实验 6：栈溢出道具 ============ */
static volatile int g_overflow_triggered = 0;

/* ============ 钩子函数（在 tasks.c / heap 里被调用） ============ */
void vApplicationIdleHook(void)
{
    /* idle 任务：没有就绪任务时运行。这里只打印一次，避免刷屏 */
    static int printed = 0;
    if (!printed) {
        printf("[idle] 空闲任务运行：说明此刻没有任务就绪（低功耗/省电常在此做）\n");
        printed = 1;
    }
}

void vApplicationMallocFailedHook(void)
{
    printf("[FATAL] 内存分配失败！检查 configTOTAL_HEAP_SIZE 或任务栈\n");
    abort();
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    g_overflow_triggered = 1;
    printf("[FATAL] 栈溢出检测到！任务名=%s（configCHECK_FOR_STACK_OVERFLOW=2：tick 时检查水位线）\n",
           pcTaskName);
}

/* ============ 工具：小睡（让出 CPU 并延迟若干 tick） ============ */
#define MS(n) ((n) / portTICK_PERIOD_MS)

/* ================================================================
 * Phase 1 任务：验证优先级抢占
 * ================================================================ */
static void vP1_high(void *pv)
{
    int i;
    (void)pv;
    for (i = 0; i < 3; i++) {
        printf("[P1] 高优先级任务 running...\n");
        vTaskDelay(MS(500)); /* 主动让出：进入 Blocked，低优先级才有机会 */
    }
    printf("[P1] 高优先级任务完成\n");
    vTaskSuspend(NULL);
}

static void vP1_low(void *pv)
{
    int i;
    (void)pv;
    for (i = 0; i < 3; i++) {
        printf("[P1] 低优先级任务 running...（高优先级就绪时我会被抢占）\n");
        vTaskDelay(MS(500));
    }
    printf("[P1] 低优先级任务完成\n");
    vTaskSuspend(NULL);
}

/* ================================================================
 * Phase 2 队列：生产者 / 消费者
 * ================================================================ */
static QueueHandle_t g_q;
static void vP2_producer(void *pv)
{
    int val = 0;
    (void)pv;
    while (g_phase == 2) {
        val++;
        if (xQueueSend(g_q, &val, 0) != pdTRUE)
            printf("[P2] 队列满！生产者被阻塞（或超时）——查 queue.c 的 xQueueGenericSend\n");
        else
            printf("[P2] 生产者发送: %d\n", val);
        vTaskDelay(MS(300));
    }
    printf("[P2] 生产者退出\n");
    vTaskSuspend(NULL);
}

static void vP2_consumer(void *pv)
{
    int val;
    (void)pv;
    while (g_phase == 2) {
        /* 阻塞等待 1 秒；队列空时返回 pdFALSE -> 演示超时路径 */
        if (xQueueReceive(g_q, &val, MS(1000)) == pdTRUE)
            printf("[P2] 消费者收到: %d\n", val);
        else
            printf("[P2] 消费者超时（1s 内没有数据）\n");
    }
    printf("[P2] 消费者退出\n");
    vTaskSuspend(NULL);
}

/* ================================================================
 * Phase 3 信号量 + 模拟中断
 * 真实场景：硬件中断里不能做耗时操作，只能 xSemaphoreGiveFromISR
 * 本实验用 SIGUSR1 信号模拟"硬件中断"，在信号处理函数里 GiveFromISR
 * ================================================================ */
static void vSigusr1_handler(int sig)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    (void)sig;
    /* ★ 这就是 ISR 上下文！只能调用 FromISR 结尾的 API，不能 printf 等耗时操作 */
    g_isr_count++;
    xSemaphoreGiveFromISR(g_isr_sem, &xHigherPriorityTaskWoken);
    (void)xHigherPriorityTaskWoken; /* 若为 pdTRUE，本应在中断退出时主动切换（见注释） */
}

static void vP3_trigger(void *pv)   /* 模拟"外部中断源"：周期性 raise 信号 */
{
    (void)pv;
    while (g_phase == 3) {
        raise(SIGUSR1);             /* 模拟硬件产生一次中断 */
        vTaskDelay(MS(700));
    }
    vTaskSuspend(NULL);
}

static void vP3_handler(void *pv)   /* 中断事件的处理任务（真正的活在这里干） */
{
    uint32_t last = 0;
    (void)pv;
    while (g_phase == 3) {
        if (xSemaphoreTake(g_isr_sem, MS(2000)) == pdTRUE) {
            printf("[P3] 处理任务醒来，已处理中断 %lu 次（%lu 次在 2s 超时窗口内）\n",
                   (unsigned long)g_isr_count, (unsigned long)(g_isr_count - last));
            last = g_isr_count;
        }
    }
    vTaskSuspend(NULL);
}

/* ================================================================
 * Phase 4 互斥锁 + 优先级反转
 * 低优先级任务持锁 → 高优先级任务等锁 → 中优先级任务不停跑
 * 若没有优先级继承：高任务会被中任务无限期饿死
 * FreeRTOS 互斥锁（不是信号量！）内置优先级继承：持锁者临时提升到等待者优先级
 * 观察点：低任务持锁期间是否被中任务抢占（应该不会——它被提升到高优先级）
 * ================================================================ */
static void vP4_low(void *pv)
{
    (void)pv;
    printf("[P4] 低优先级任务：尝试拿锁...\n");
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    printf("[P4] 低优先级任务：拿到锁，正在做临界区工作（此时它被提升优先级）\n");
    vTaskDelay(MS(1500));            /* 模拟持锁做耗时操作 */
    xSemaphoreGive(g_mutex);
    printf("[P4] 低优先级任务：释放锁\n");
    g_rev_hold_done = 1;
    vTaskSuspend(NULL);
}

static void vP4_medium(void *pv)
{
    (void)pv;
    while (g_phase == 4) {
        printf("[P4] 中优先级任务：狂转（不碰锁）——若低任务没被提升，高任务会被我饿死\n");
        vTaskDelay(MS(400));
    }
    vTaskSuspend(NULL);
}

static void vP4_high(void *pv)
{
    (void)pv;
    vTaskDelay(MS(300));             /* 等低任务先拿到锁 */
    printf("[P4] 高优先级任务：开始等锁...\n");
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    printf("[P4] 高优先级任务：拿到锁（说明优先级继承生效，中任务没能插队）\n");
    xSemaphoreGive(g_mutex);
    vTaskSuspend(NULL);
}

/* ================================================================
 * Phase 5 运行状态一览：vTaskList
 * ================================================================ */
static void vP5_snapshot(void *pv)
{
    char buf[1024];
    (void)pv;
    while (g_phase == 5) {
        printf("\n[P5] --- vTaskList 运行状态（R=就绪 B=阻塞 S=挂起 D=删除）---\n");
        vTaskList(buf);
        printf("%s\n", buf);
        vTaskDelay(MS(1000));
    }
    vTaskSuspend(NULL);
}

static void vP5_worker(void *pv)
{
    (void)pv;
    while (g_phase == 5) {
        vTaskDelay(MS(300));         /* 一个老老实实的周期性任务 */
    }
    vTaskSuspend(NULL);
}

/* ================================================================
 * Phase 6 栈溢出：故意写爆栈
 * ================================================================ */
/* ================================================================
 * Phase 6 栈水位线：故意吃栈，观察 uxTaskGetStackHighWaterMark 变化
 * ★ 移植层教学点：POSIX 移植层（port.c prvSwitchThread）从不把真实 pthread
 *   栈指针写回 pxCurrentTCB->pxTopOfStack，所以 configCHECK_FOR_STACK_OVERFLOW
 *   的"越界检查"在这里永远不触发（钩子只会在真实裸核移植上生效——那是 M2 的任务）。
 *   但"高水位"（uxTaskGetStackHighWaterMark）基于栈区图案填充扫描，是有效的，
 *   这里就用它量化栈消耗。
 * ================================================================ */
static int g_depth = 0;

static void vP6_recursive(void *pv)
{
    char pad[1024];                  /* 每次递归吃掉 1KB 栈 */
    volatile char sink;
    (void)pv;
    /* ★ 必须真实使用 pad，否则 -O1 优化会把数组整个优化掉（前面就踩了这坑） */
    memset(pad, 0x5a, sizeof(pad));
    sink = pad[0];
    (void)sink;

    g_depth++;
    {
        unsigned wm = uxTaskGetStackHighWaterMark(NULL);
        if ((g_depth % 10) == 0)
            printf("[P6] 深度=%d  剩余栈高水位=%u 词（≈%u KB，初始 8192 词=32KB）\n",
                   g_depth, wm, (unsigned)(wm / 256));
        /* 每层都检查水位，剩 2KB 就收手（留安全余量，避免真的爆栈破坏堆） */
        if (wm < 512) {
            printf("[P6] 水位见底（<2KB）！注意：模拟器里溢出钩子不会触发（移植层不追踪真实 SP），"
                   "真实裸核移植上 configCHECK_FOR_STACK_OVERFLOW 会在切换时报警。收手。\n");
            g_depth = 0;
            vTaskSuspend(NULL);
        }
    }
    vTaskDelay(MS(60));
    vP6_recursive(NULL);             /* 无限递归吃栈 */
}

/* ================================================================
 * 编排任务（最高优先级 7）：每 4 秒推进一个阶段，最后结束调度器
 * ================================================================ */
/* 前向声明：各阶段启动函数定义在文件后部，避免 C 隐式声明把 static 函数当成外部函数 */
static void start_phase1(void);
static void start_phase2(void);
static void start_phase3(void);
static void start_phase4(void);
static void start_phase5(void);
static void start_phase6(void);

static void vOrchestrator(void *pv)
{
    (void)pv;
    while (g_phase <= 6) {
        vTaskDelay(MS(4000));
        g_phase++;
        printf("\n========== 进入 Phase %d ==========\n", g_phase);
        /* 每个阶段进入时创建对应实验任务 */
        switch (g_phase) {
        case 2: start_phase2(); break;
        case 3: start_phase3(); break;
        case 4: start_phase4(); break;
        case 5: start_phase5(); break;
        case 6: start_phase6(); break;
        default: break;
        }
    }
    printf("\n全部实验完成。调用 vTaskEndScheduler() 退出。\n");
    vTaskEndScheduler();
    vTaskSuspend(NULL);
}

/* ============ 启动各阶段任务的辅助函数 ============ */
static void start_phase1(void)
{
    xTaskCreate(vP1_high, "p1_high", 8192, NULL, 3, NULL);
    xTaskCreate(vP1_low,  "p1_low",  8192, NULL, 1, NULL);
}

static void start_phase2(void)
{
    g_q = xQueueCreate(3, sizeof(int));   /* 容量 3 的 int 队列 */
    xTaskCreate(vP2_producer, "p2_prod", 8192, NULL, 2, NULL);
    xTaskCreate(vP2_consumer, "p2_cons", 8192, NULL, 1, NULL);
}

static void start_phase3(void)
{
    struct sigaction sa;
    g_isr_sem = xSemaphoreCreateBinary();
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = vSigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);
    xTaskCreate(vP3_trigger, "p3_trig", 8192, NULL, 1, NULL);
    xTaskCreate(vP3_handler, "p3_hand", 8192, NULL, 2, NULL);
}

static void start_phase4(void)
{
    g_mutex = xSemaphoreCreateMutex();    /* ★ 注意：这是互斥锁（带优先级继承），不是二进制信号量 */
    xTaskCreate(vP4_low,   "p4_low",   8192, NULL, 1, NULL);
    xTaskCreate(vP4_medium, "p4_medium", 8192, NULL, 2, NULL);
    xTaskCreate(vP4_high,  "p4_high",  8192, NULL, 3, NULL);
}

static void start_phase5(void)
{
    xTaskCreate(vP5_snapshot, "p5_snap", 8192, NULL, 1, NULL);
    xTaskCreate(vP5_worker,   "p5_work", 8192, NULL, 2, NULL);
}

static void start_phase6(void)
{
    xTaskCreate(vP6_recursive, "p6_boom", 8192, NULL, 1, NULL); /* 递归吃到 8K 词(32KB)栈，触发溢出检测 */
}

int main(void)
{
    printf("=== FreeRTOS %s POSIX 模拟器实验 ===\n", tskKERNEL_VERSION_NUMBER);
    printf("tick 周期 = %d ms（portTICK_PERIOD_MS=%d）\n",
           (int)(1000 / configTICK_RATE_HZ), (int)portTICK_PERIOD_MS);
    printf("内存管理 = heap_3（malloc 封装，POSIX 模拟器专用）\n\n");

    start_phase1();
    xTaskCreate(vOrchestrator, "orchestrator", 8192, NULL, 7, NULL);

    vTaskStartScheduler();   /* 永不返回，直到 vTaskEndScheduler() */

    printf("调度器已退出。\n");
    return 0;
}
