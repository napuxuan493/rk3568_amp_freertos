/* 最小测试固件：什么都不做，只在 0x7000000 死循环
 * 用途：隔离"ATF 启动 core3 的机制" vs "我们的固件行为"
 *   - 如果连它都会导致 SSH 断 → 问题在启动机制（ATF/电源/时钟）
 *   - 如果它没事 → 问题在我们的固件（MMU/串口/异常级别） */
    .section .text.start, "ax"
    .globl  _start
_start:
    b       .
