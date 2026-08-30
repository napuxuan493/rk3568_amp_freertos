/*
 * FreeRTOS POSIX 模拟器 配置
 * 作用：把 FreeRTOS 内核当 Linux 用户态进程运行（pthread + setitimer 模拟 tick）
 * 对应源码：portable/ThirdParty/GCC/Posix/port.c
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdio.h>
#include <stdlib.h>

/* ---------- 基础调度 ---------- */
#define configUSE_PREEMPTION            1   /* 抢占式调度（本移植层支持） */
#define configUSE_IDLE_HOOK             1   /* idle 钩子，观察空闲任务 */
#define configUSE_TICK_HOOK             0
#define configTICK_RATE_HZ              1000 /* 1ms 一个 tick（POSIX 用 setitimer 触发） */
#define configMAX_PRIORITIES            8
#define configMINIMAL_STACK_SIZE        ( ( unsigned short ) 8192 ) /* 单位：字（4 字节）。★ 必须 ≥ (PTHREAD_STACK_MIN/4+1)≈4097：POSIX 移植层把任务栈直接给 pthread 用，glibc 要求 ≥16KB（PTHREAD_STACK_MIN），默认 2K 词=8KB 会触发 setstack EINVAL */
#define configTOTAL_HEAP_SIZE           ( ( size_t ) ( 1024 * 1024 ) )
#define configMAX_TASK_NAME_LEN         16
#define configUSE_16_BIT_TICKS          0
#define configIDLE_SHOULD_YIELD         1

/* ---------- 功能开关 ---------- */
#define configUSE_MUTEXES               1   /* 互斥锁（含优先级继承！实验 4 靠它） */
#define configUSE_RECURSIVE_MUTEXES     1
#define configUSE_COUNTING_SEMAPHORES   1
#define configUSE_TASK_NOTIFICATIONS    1
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       2
#define configTIMER_QUEUE_LENGTH        10
#define configTIMER_TASK_STACK_DEPTH    ( configMINIMAL_STACK_SIZE * 2 )
#define configUSE_TRACE_FACILITY        1   /* 支持 vTaskList */
#define configUSE_STATS_FORMATTING_FUNCTIONS 1
#define configQUEUE_REGISTRY_SIZE       10

/* ---------- 内存与错误 ---------- */
#define configSUPPORT_STATIC_ALLOCATION 0
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configCHECK_FOR_STACK_OVERFLOW  2   /* 实验 6：栈溢出检测 */
#define configUSE_MALLOC_FAILED_HOOK    1

/* ---------- 断言 ---------- */
#ifndef configASSERT
#define configASSERT( x ) do { if( ( x ) == 0 ) { fprintf( stderr, "[ASSERT] %s:%d\n", __FILE__, __LINE__ ); abort(); } } while( 0 )
#endif

/* ---------- API 裁剪（INCLUDE 系列） ---------- */
#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_vTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle      1
#define INCLUDE_eTaskGetState               1
#define INCLUDE_xEventGroupSetBitFromISR    1
#define INCLUDE_xTimerPendFunctionCall      1
#define INCLUDE_xSemaphoreGetMutexHolder    1
#define INCLUDE_xTaskGetHandle              1

#endif /* FREERTOS_CONFIG_H */
