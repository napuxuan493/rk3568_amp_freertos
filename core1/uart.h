#ifndef M2_UART_H
#define M2_UART_H

#include <stdint.h>

void uart_init(void);
void uart_diag(void);   /* M2.2 诊断：UART2 打印 UART4 配置读回 */
void uart_diag_mark(const char *tag); /* M2.2 诊断：UART2 打印进度标记 */
void uart_diag_puts(const char *str); /* M2.2 诊断：UART2 公共打印（configASSERT 用） */
void uart_diag_u64(const char *tag, uint64_t v); /* M2.2 诊断：UART2 打印数字 */
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint64_t v);
void uart_putdec(uint32_t v);

#endif
