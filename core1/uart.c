/* M2.2 正式方案：core1 控制台 = UART4 @ 0xFE680000（40pin，uart4m1：GPIO3_B1=RX / GPIO3_B2=TX）
 *
 * core1 在 u-boot 阶段启动，Linux 尚未初始化 UART4，且 dts 已把 uart4 设为
 * disabled（Linux 不再占用）——所以 core1 必须自行完成三步初始化：
 *   ① CRU 时钟：SCLK_UART4 mux 选 xin24m（24MHz）+ 去 gate
 *   ② IOC 引脚：GPIO3_B1/B2 配成 UART4 功能（mux=4）
 *   ③ UART4 寄存器：波特率 1500000 / 8N1（div = 24M/(16*1.5M) = 1）
 *
 * 寄存器（RK3568 TRM + clk-rk3568.c/pinctrl-rockchip.c 核对）：
 *   CRU           = 0xFDD20000
 *   CLKSEL_CON(58)= 0x100 + 58*4 = 0x1E8   sclk_uart4_mux 位[13:12]（父=src/frac/xin24m）
 *   CLKGATE_CON(28)= 0x300 + 28*4 = 0x3B0  位8=PCLK_UART4 位11=SCLK_UART4（1=关 0=开）
 *   SYS_IOC       = 0xFDC60000
 *   GPIO3B_IOMUX  = 0x48（grf_mux_offset 0 + GPIO3 组 0x40 + B 组 8）
 *                   B1 位[7:4]  B2 位[11:8]  mux=4
 * 注：CRU/IOC 均为 HIWORD_MASK 写（高 16 位对应位置 1 才允许写低 16 位）。
 */
#include "uart.h"

#define UART4_BASE      0xFE680000UL   /* 正式：UART4 40pin（uart4m1） */
#define REG(off)        (*(volatile uint32_t *)(UART4_BASE + (off)))

#define UART_DLL        0x00    /* 分频低 8 位（DLAB=1 时）/ 发送保持寄存器 */
#define UART_THR        0x00    /* 发送保持寄存器 */
#define UART_DLH        0x04    /* 分频高 8 位（DLAB=1 时） */
#define UART_FCR        0x08    /* FIFO 控制 */
#define UART_LCR        0x0C    /* 线路控制 */
#define UART_LSR        0x14    /* 线路状态 */
#define LSR_THRE        (1u << 5)   /* 发送保持寄存器空 */

#define CRU_BASE        0xFDD20000UL
#define CLKSEL_CON58    0x1E8UL     /* sclk_uart4_mux 位[13:12] */
#define CLKGATE_CON28   0x370UL     /* RK3568_CLKGATE_CON(28)=28*4+0x300=0x370；位8=PCLK_UART4 位11=SCLK_UART4 */

#define IOC_BASE        0xFDC60000UL
#define GPIO3B_IOMUX    0x48UL

static inline void cru_write32(uint32_t off, uint32_t mask_hi, uint32_t val_lo)
{
    *(volatile uint32_t *)(CRU_BASE + off) = (mask_hi << 16) | val_lo;
}

/* ── M2.2 诊断：用 UART2（调试口）打印 UART4 相关寄存器读回值 ── */
#define DIAG_UART2      0xFE660000UL
static void diag_putc(char c)
{
    volatile uint32_t *lsr = (volatile uint32_t *)(DIAG_UART2 + 0x14);
    volatile uint32_t *thr = (volatile uint32_t *)(DIAG_UART2 + 0x00);
    while (!(*lsr & (1u << 5))) ;
    *thr = (uint32_t)c;
}
static void diag_puts(const char *str)
{
    while (*str) { if (*str == '\n') diag_putc('\r'); diag_putc(*str++); }
}
static void diag_hex(uint64_t v)
{
    int i;
    static const char t[] = "0123456789abcdef";
    for (i = 15; i >= 0; i--) diag_putc(t[(v >> (i * 4)) & 0xF]);
}

void uart_diag_u64(const char *tag, uint64_t v)
{
    diag_puts("[");
    diag_puts(tag);
    diag_puts("]=");
    diag_hex(v);
    diag_puts("\r\n");
}

void uart_diag_puts(const char *str)
{
    diag_puts(str);
}

void uart_diag_mark(const char *tag)
{
    diag_puts("[core1] ");
    diag_puts(tag);
    diag_puts("\r\n");
}

void uart_diag(void)
{
    diag_puts("\r\n[DIAG] clksel58=");
    diag_hex(*(volatile uint32_t *)(CRU_BASE + CLKSEL_CON58));
    diag_puts("\r\n[DIAG] gate28=");
    diag_hex(*(volatile uint32_t *)(CRU_BASE + CLKGATE_CON28));
    diag_puts("\r\n[DIAG] gpio3b_iomux=");
    diag_hex(*(volatile uint32_t *)(IOC_BASE + GPIO3B_IOMUX));
    diag_puts("\r\n[DIAG] uart4_lcr=");
    diag_hex(REG(UART_LCR));
    diag_puts(" uart4_lsr=");
    diag_hex(REG(UART_LSR));
    diag_puts("\r\n");
}

void uart_init(void)
{
    /* ① CRU：SCLK_UART4 mux 选 xin24m（父数组 {src, frac, xin24m}，索引 2）
     * 使能位 = 字段位置 + 16（逐位使能）：[13:12] → [29:28]；[6:0] → [22:16] */
    cru_write32(CLKSEL_CON58, 0x3u << 12, 2u << 12);  /* 使能[29:28]，mux 写 2 */
    cru_write32(CLKSEL_CON58, 0x7Fu,      0u);        /* 使能[22:16]，div 写 0 → div_con+1=1 → 24MHz */

    /* 去 gate：PCLK_UART4（位8）、SCLK_UART4（位11）写 0 = 使能（使能位 24/27） */
    cru_write32(CLKGATE_CON28, 0x1u << 8,  0u);       /* 使能位24，清位8：PCLK_UART4 开 */
    cru_write32(CLKGATE_CON28, 0x1u << 11, 0u);       /* 使能位27，清位11：SCLK_UART4 开 */

    /* ② IOC：GPIO3_B1(位7:4)、GPIO3_B2(位11:8) → mux 4（uart4m1） */
    {
        uint32_t v = (4u << 4) | (4u << 8);       /* B1=4(RX), B2=4(TX) */
        /* HIWORD_MASK：高16位每位对应低16位同一位，写1才允许写。
         * B1 位[7:4]  → 屏蔽位[23:20]；B2 位[11:8] → 屏蔽位[27:24]
         * （之前错写成 16|20，导致 B2 屏蔽位没置 → TX 引脚没配成 UART4） */
        uint32_t m = (0xFu << 20) | (0xFu << 24); /* 屏蔽位[23:20]=B1、[27:24]=B2 */
        *(volatile uint32_t *)(IOC_BASE + GPIO3B_IOMUX) = m | v;
    }

    /* ③ UART4：波特率 1500000（div=24M/(16*1.5M)=1），8N1，FIFO 使能 */
    REG(UART_LCR) = 0x83;       /* DLAB=1 + 8N1 */
    REG(UART_DLL) = 0x01;       /* 除数 = 1 */
    REG(UART_DLH) = 0x00;
    REG(UART_LCR) = 0x03;       /* DLAB=0 + 8N1 */
    REG(UART_FCR) = 0x01;       /* FIFO 使能 */
}

void uart_putc(char c)
{
    while (!(REG(UART_LSR) & LSR_THRE))
        ;
    REG(UART_THR) = (uint32_t)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');        /* 串口终端要 \r\n */
        uart_putc(*s++);
    }
}

static const char hex_tab[] = "0123456789abcdef";

void uart_puthex(uint64_t v)
{
    int i;
    uart_puts("0x");
    for (i = 15; i >= 0; i--)
        uart_putc(hex_tab[(v >> (i * 4)) & 0xF]);
}

void uart_putdec(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0) {
        uart_putc('0');
        return;
    }
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i)
        uart_putc(buf[--i]);
}
