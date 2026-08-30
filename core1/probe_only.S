/* 万能探针固件（M2.1 诊断版）：
 * 只做：写两个探针 → 刷 D-Cache → 死循环
 * 设计目标：对 MMU / cache / 异常级别 全部免疫——
 *   - 只用 EL0 也安全的指令（ldr/movz/movk/str/dc/dsb）
 *   - 每写必刷 cache（无论 cache 是否开启，数据必达 DRAM）
 *   - 不碰 sctlr/vbar（避免 EL 相关 fault）
 * 如果这个都不产生探针 → core3 没执行我们的代码（入口/安全世界问题）
 */
    .section .text.start, "ax"
    .globl  _start
_start:
    /* 探针1 @0x7002000 = "CORE" */
    ldr     x0, =0x7002000
    movz    x1, #0x4F43
    movk    x1, #0x4552, lsl #16
    str     w1, [x0]
    dc      civac, x0          /* 刷这条 cache line 到 DRAM（EL0 可用） */
    dsb     sy

    /* 探针2 @0x7002010 = "PROB" */
    ldr     x0, =0x7002010
    movz    x1, #0x5042         /* 先写低半 "PB"...
    movk    x1, #0x4F52, lsl #16  /* ...再补高半 "OR" → "PROB" */
    str     w1, [x0]
    dc      civac, x0
    dsb     sy

    b       .                   /* 死循环 */
