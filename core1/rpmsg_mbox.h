/* M3: core1 侧 mailbox 门铃驱动（Linux 侧协议见 rockchip-mailbox.c）
 *
 * 寄存器（RK3568 mailbox @ 0xFE780000，TRM + rockchip-mailbox.c 核对）：
 *   A2B_INTEN  0x00   A2B_STATUS 0x04   A2B_CMD(x) 0x08+8x  A2B_DAT(x) 0x0C+8x
 *   B2A_INTEN  0x28   B2A_STATUS 0x2C   B2A_CMD(x) 0x30+8x  B2A_DAT(x) 0x34+8x
 *
 * 方向约定（从 Linux/virtio master 视角）：
 *   - Linux 发 = 写 A2B_CMD/DAT（通道 3，"rpmsg-tx"）→ 硬件置 A2B_STATUS bit3
 *   - Linux 收 = B2A 中断（通道 0，"rpmsg-rx"）→ core1 写 B2A_CMD/DAT 触发
 *
 * core1 视角：
 *   - 收门铃：轮询 A2B_STATUS bit3 → 读 A2B_CMD(3)/A2B_DAT(3) → W1C 清 bit3
 *   - 踢门铃：查 B2A_STATUS bit0 空闲 → 写 B2A_CMD(0)=link_id, B2A_DAT(0)=magic
 *
 * 8 字节消息：cmd = link_id(0x03 = core0↔core3), data = 0x524D5347("RMSG")
 * 轮询而非中断：core1 未做 GIC 中断路由（amp-irqs 未配），轮询简单可靠。
 */
#ifndef M3_RPMSG_MBOX_H
#define M3_RPMSG_MBOX_H

#include <stdint.h>

#define MAILBOX_BASE        0xFE780000UL

#define MBOX_A2B_INTEN      0x00UL
#define MBOX_A2B_STATUS     0x04UL
#define MBOX_A2B_CMD(n)     (0x08UL + (n) * 8)
#define MBOX_A2B_DAT(n)     (0x0CUL + (n) * 8)
#define MBOX_B2A_STATUS     0x2CUL
#define MBOX_B2A_CMD(n)     (0x30UL + (n) * 8)
#define MBOX_B2A_DAT(n)     (0x34UL + (n) * 8)

/* dts: mboxes = <&mailbox 0 &mailbox 3>；link-id = <0x03> */
/* core1 视角 RX/TX*/
#define MBOX_A2B_CHAN  3u   /* A2B = AP→BB:Linux 发、core1 收 */
#define MBOX_B2A_CHAN  0u   /* B2A = BB→AP:core1 发、Linux 收 */

#define RPMSG_MBOX_MAGIC    0x524D5347UL  /* "RMSG" */
#define RPMSG_LINK_ID       0x03UL        /* master=0, remote=3 */

#define MBOX_REG(off)       (*(volatile uint32_t *)(MAILBOX_BASE + (off)))

/* 初始化：无（poll 模式不需要 INTEN；如需中断可置 A2B_INTEN bit3） */
void mbox_init(void);

/* 是否有来自 Linux 的门铃（A2B 通道3 状态位） */
static inline int mbox_has_doorbell(void)
{
    return (MBOX_REG(MBOX_A2B_STATUS) & (1u << MBOX_A2B_CHAN)) != 0;
}

/* 读并清除 Linux 的门铃消息（返回 cmd；data 通过指针带回） */
uint32_t mbox_read_doorbell(uint32_t *data);

/* 踢门铃通知 Linux（B2A 通道0；忙则等待。返回 0 成功） */
int mbox_kick(void);

#endif /* M3_RPMSG_MBOX_H */
