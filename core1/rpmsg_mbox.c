/* M3: core1 侧 mailbox 门铃驱动实现（见 rpmsg_mbox.h 的协议说明） */
#include "rpmsg_mbox.h"
#include "uart.h"        /* UART2 诊断 */

void mbox_init(void)
{
    /* 握手检测需要 A2B_INTEN（否则 STATUS 不锁存）；检测后立即关闭
     * （门铃中断在 core3 上触发 EC=0 异常），数据流改用 vring 轮询。 */
    MBOX_REG(MBOX_A2B_INTEN) |= (1u << MBOX_A2B_CHAN);
    uart_puts("[RPMsg] mbox initialized\r\n");
}

uint32_t mbox_read_doorbell(uint32_t *data)
{
    /* 握手完成：关 INTEN（止住门铃中断异常）+ W1C 清 STATUS。
     * 之后的收发全部走 vring 轮询。 */
    (void)data;
    MBOX_REG(MBOX_A2B_INTEN) &= ~(1u << MBOX_A2B_CHAN);
    MBOX_REG(MBOX_A2B_STATUS) = (1u << MBOX_A2B_CHAN);
    return RPMSG_LINK_ID;
}

int mbox_kick(void)
{
    uint32_t spins = 0;

    /* Linux 侧发送是同步的，它轮询 A2B_STATUS 等我们清；我们这边
     * 要等 B2A_STATUS 空闲（上一个踢的消息已被 Linux 取走并清除）。 */
    while ((MBOX_REG(MBOX_B2A_STATUS) & (1u << MBOX_B2A_CHAN)) && spins < 100000u)
        spins++;

    if (spins >= 100000u)
        return -1;   /* Linux 未消费上一条（异常） */

    /* 写 cmd/data 触发硬件：置 B2A_STATUS bit0 → 中断 Linux（电平，Linux 会 W1C） */
    MBOX_REG(MBOX_B2A_CMD(MBOX_B2A_CHAN)) = RPMSG_LINK_ID;
    MBOX_REG(MBOX_B2A_DAT(MBOX_B2A_CHAN)) = RPMSG_MBOX_MAGIC;

    return 0;
}
