# 实验 01：FreeRTOS 机制速通（POSIX 模拟器）

> 目标：不依赖板子，在 PC 上亲手运行 FreeRTOS，观察 6 个核心机制，同时精读内核源码。
> 内核版本：FreeRTOS V10.6.2 LTS（`core1/freertos-kernel`）

## 运行

```bash
cd labs/01_freertos_posix
make && ./freertos_lab        # 全流程约 30 秒，自动跑完 6 个阶段
```

完整输出示例已存：`run_output.txt`（当前版本）

## 六个阶段看什么

| 阶段 | 机制 | 观察点 |
|---|---|---|
| 1 | 任务与优先级抢占 | 高优先级就绪即抢 CPU；`vTaskDelay` 让出 |
| 2 | 队列 | 生产者/消费者；阻塞、超时（消费者 1s 超时路径） |
| 3 | 信号量 + 模拟中断 | `SIGUSR1` 信号扮演硬件中断；处理函数里只能用 `xSemaphoreGiveFromISR`，耗时活在任务里做 |
| 4 | 互斥锁 + 优先级继承 | 低任务持锁时被提升优先级，中任务无法插队饿死高任务 |
| 5 | 运行状态 | `vTaskList`：R 就绪 / B 阻塞 / S 挂起 / D 已删 + 栈高水位 |
| 6 | 栈高水位 | 递归吃栈，`uxTaskGetStackHighWaterMark` 从 24KB 一路掉到 <2KB 收手 |

## 这一路踩的三个坑（都是移植层的真知识）

1. **`pthread_attr_setstack` EINVAL**：POSIX 移植层把 FreeRTOS 任务栈直接交给 pthread 用，
   glibc 要求栈 ≥ `PTHREAD_STACK_MIN`(16KB)，而 `configMINIMAL_STACK_SIZE` 默认 2K 词=8KB
   不够 → 修复：配置提到 8K 词（`FreeRTOSConfig.h`）。这是"移植到带 OS 的环境"特有的约束，
   裸核移植（M2 目标）不存在。
2. **`-O1` 把"没用"的栈数组优化掉**：`char pad[1024]` 写了不读，编译器整个删掉，递归永远不爆栈。
   必须真实读写（`memset` + volatile 汇点）才能模拟栈压力。
3. **自删除任务（`vTaskDelete(NULL)`）竞态**：idle 释放任务栈时，对应 pthread 还在退出流程里用栈，
   glibc `free(): invalid pointer` 直接 abort。→ 实验任务一律 `vTaskSuspend(NULL)` 挂起收尾。
   （gdb 回溯 `tcache_thread_shutdown` 抓到的，值得自己复现一遍。）

## 溢出检测在模拟器里为什么"失效"（重要认知）

`configCHECK_FOR_STACK_OVERFLOW` 的检查在 `vTaskSwitchContext`（tasks.c:3112），它比较的是
`pxCurrentTCB->pxTopOfStack` 与栈边界；而 POSIX 移植层（`port.c prvSwitchThread`）**从不把真实
pthread 栈指针写回 pxTopOfStack**，所以越界检查永远不触发。栈**高水位**（基于图案扫描）仍然有效，
所以实验 6 用高水位量化。**等 M2 在 RK3568 core1 上做裸核移植时，栈指针由硬件 SP 直接驱动，
溢出检测才是真的**——这就是"每个移植层都要确认栈指针追踪语义"的经典教训。

## 源码精读作业（读 `core1/freertos-kernel/`）

带着问题读，答案写进日志：
1. `tasks.c` 的 `TCB` 结构：哪些字段是调度必需的？（提示：pxTopOfStack/uxPriority/eventList 等）
2. `vTaskDelay` 内部做了什么？任务从"运行"到"阻塞"经历了哪几步？
3. `queue.c`：`xQueueSend` 和 `xSemaphoreTake` 的本质是什么？信号量为什么是队列的一种？
4. `list.c`：就绪链表是"链表数组"还是"单链表"？`vTaskSwitchContext` 怎么选下一个任务？
5. 优先级继承在 `queue.c` 哪里实现？（搜 `pxMutexHolder`）——解释实验 4 的现象。

## 验收标准

- [ ] 能不看笔记讲出 6 个机制的行为差异
- [ ] 能回答上述 5 个源码问题（写进日志）
- [ ] 能解释实验 4 中"低任务拿锁后打印顺序"为什么是这样
