/* M2.1: core1 裸机启动（ATF CPU_ON 进入，假定 EL1 非安全、MMU 关）
 * 链接基址 0x7000000 = dts amp-cpus entry（实测定址，避开内核 0x200000）
 */
    .section .text.start, "ax"
    .globl  _start
    .type   _start, %function
_start:
    /* 1. 先关 MMU/D-Cache/I-Cache。
     *    ★ isb 之后所有访问不再走 cache——探针写、UART 写才能直达
     *      DRAM/外设（否则 core3 的写可能滞留在 L1，Linux 的 ioremap
     *      读和 UART 硬件都看不到——本次调试的教训） */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)      /* M:   MMU */
    bic     x0, x0, #(1 << 2)      /* C:   D-Cache */
    bic     x0, x0, #(1 << 12)     /* I:   I-Cache */
    msr     sctlr_el1, x0
    isb

    /* ★ 探针1：_start 到达标记（此时 cache 已关，写直达 DRAM，Linux 可读回） */
    ldr     x0, =0x7002000
    movz    x1, #0x4F43
    movk    x1, #0x4552, lsl #16   /* x1 = 0x45524F43 → 内存字节 "CORE" */
    str     w1, [x0]

    /* 2. 初始化栈（链接脚本末尾 _stack_top，32KB） */
    ldr     x0, =_stack_top
    mov     sp, x0

    /* 3. 异常向量表（M2.1 空表：任何异常死循环，不使能中断就不会进） */
    ldr     x0, =_vectors
    msr     vbar_el1, x0
    isb

    /* 4. 清 BSS（.data 由加载器拷入，.bss 需清零） */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
    sub     x2, x1, x0
    cbz     x2, 2f
1:  str     xzr, [x0], #8
    subs    x2, x2, #8
    bne     1b
2:
    /* 5. 跳 C 世界 */
    bl      main

    /* main 不应返回 */
1:  b       1b
    .size   _start, . - _start

/* 异常向量表：AArch64 每个向量槽 128 字节，共 16 槽，2KB 对齐
 * M2.2：IRQ 槽跳 FreeRTOS_IRQ_Handler（portASM.S 提供，做上下文切换），
 * 其余槽死循环。符号 _freertos_vector_table 是 port 要求的名字
 * （vPortRestoreTaskContext 会 MSR VBAR_EL1 = 这个表）。 */
    .section .text.vectors, "ax"
    .balign  2048
    .globl  _vectors
    .globl  _freertos_vector_table
_vectors:
_freertos_vector_table:
    .macro  vector_slot handler
    b       \handler
    .balign 128
    .endm
    vector_slot FreeRTOS_SWI_Handler        /* 0:  EL1t 同步（SVC yield 走这） */
    vector_slot FreeRTOS_IRQ_Handler        /* 1:  EL1t IRQ   ← 任务在 EL1t 跑，IRQ 走这 */
    vector_slot ignore_async                /* 2:  EL1t FIQ（M3：门铃噪声，忽略返回） */
    vector_slot ignore_async                /* 3:  EL1t SError */
    vector_slot FreeRTOS_SWI_Handler        /* 4:  EL1h 同步 */
    vector_slot FreeRTOS_IRQ_Handler        /* 5:  EL1h IRQ（保险） */
    vector_slot ignore_async                /* 6:  EL1h FIQ */
    vector_slot ignore_async                /* 7:  EL1h SError */
    vector_slot FreeRTOS_SWI_Handler        /* 8:  EL0t(a64) 同步 */
    vector_slot FreeRTOS_IRQ_Handler        /* 9:  EL0t(a64) IRQ */
    vector_slot ignore_async                /* 10: EL0t(a64) FIQ */
    vector_slot ignore_async                /* 11: EL0t(a64) SError */
    vector_slot FreeRTOS_SWI_Handler        /* 12: EL0t(a32) 同步 */
    vector_slot FreeRTOS_IRQ_Handler        /* 13: EL0t(a32) IRQ */
    vector_slot ignore_async                /* 14: EL0t(a32) FIQ */
    vector_slot ignore_async                /* 15: EL0t(a32) SError */
    .size   _vectors, . - _vectors

/* M3(8/26)：FIQ/SError 忽略返回。mailbox A2B 门铃的中断在 core3 上表现为
 * 异步异常（FIQ/SError），core1 用轮询收门铃不需要它；直接 ERET 继续执行。
 * 注意：异步异常被取后 SPSR/ELR 已由硬件保存，不碰栈直接 eret 即可。 */
.align 4
.global ignore_async
.type ignore_async, %function
ignore_async:
    /* M3(8/26)：门铃后的首个内存访问触发 EC=0 异常，若 eret 回同一指令会
     * 无限重触发。把 ELR 前进 4 字节跳过该指令再返回。 */
    /*mrs     x0, elr_el1
    add     x0, x0, #4
    msr     elr_el1, x0
    eret*/
    b   abort_handler
