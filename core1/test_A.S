/* M2.1 终极诊断固件：core3 一执行就在调试串口（UART2 fe660000）刷 'A'
 * 设计：全部用立即数构造（无 literal 依赖、无 EL 敏感指令），
 *      每写必刷 cache——只要 core3 执行了哪怕第一条指令，调试口必见 'A' */
    .section .text.start, "ax"
    .globl  _start
_start:
    /* 探针 @0x7002000（顺带） */
    movz    x0, #0x2000
    movk    x0, #0x700, lsl #16       /* x0 = 0x7002000 */
    movz    x1, #0x4F43
    movk    x1, #0x4552, lsl #16      /* "CORE" */
    str     w1, [x0]
    dc      civac, x0
    dsb     sy

    /* 循环往 UART2 THR（0xFE660000）写 'A' */
    movz    x0, #0x6600
    movk    x0, #0xFE00, lsl #16      /* x0 = 0xFE660000 */
    mov     w1, #0x41                 /* 'A' */
1:
    str     w1, [x0]
    dc      civac, x0
    dsb     sy
    b       1b
