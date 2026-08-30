/* M2.2: 异常处理 —— 打印 ELR/SP/ESR/FAR 后死循环（调试用） */
#include "uart.h"
#include <stdint.h>

/* [EXC] 打 UART2 调试口（DIAG_UART2），方便 UART2 侧看到崩溃 */
#define EXC_UART2   0xFE660000UL
static void exc_putc(char c)
{
    volatile uint32_t *lsr = (volatile uint32_t *)(EXC_UART2 + 0x14);
    volatile uint32_t *thr = (volatile uint32_t *)(EXC_UART2 + 0x00);
    while (!(*lsr & (1u << 5))) ;
    *thr = (uint32_t)c;
}
static void exc_puts(const char *str)
{
    while (*str) { if (*str == '\n') exc_putc('\r'); exc_putc(*str++); }
}
static void exc_hex(uint64_t v)
{
    int i;
    static const char t[] = "0123456789abcdef";
    for (i = 15; i >= 0; i--) exc_putc(t[(v >> (i * 4)) & 0xF]);
}

void abort_handler(void)
{
    uint64_t esr, far, elr, sp;

    __asm volatile("mov %0, sp" : "=r"(sp));
    __asm volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm volatile("mrs %0, far_el1" : "=r"(far));

    exc_puts("\r\n[EXC] sp=");
    exc_hex(sp);
    exc_puts(" elr=");
    exc_hex(elr);
    exc_puts("\r\n      esr=");
    exc_hex(esr);
    exc_puts(" far=");
    exc_hex(far);
    exc_puts("\r\n");

    for (;;)
        ;
}
