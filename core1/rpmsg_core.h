/* M3: core1 侧 rpmsg 协议层（匹配 Linux virtio_rpmsg_bus.c + rockchip_rpmsg.c）
 *
 * 数据流（virtio slave 视角）：
 *   发（core1→Linux）：vq0 avail 取空缓冲 → 写 rpmsg_hdr+payload → vq0 used → 踢 B2A
 *   收（Linux→core1）：A2B 门铃 → vq1 avail 取数据缓冲 → 解析 hdr → vq1 used 归还 → 踢 B2A
 *
 * 消息格式（小端）：
 *   struct rpmsg_hdr { u32 src; u32 dst; u32 reserved; u16 len; u16 flags; u8 data[]; }
 *   struct rpmsg_ns_msg { char name[32]; u32 addr; u32 flags; }   // flags: CREATE=0
 *
 * 地址约定：
 *   - Linux 的 NS 端点地址 = 53（virtio_rpmsg_bus.c RPMSG_NS_ADDR）
 *   - core1 自己声明 ept 地址 = 1（随 NS 宣告发给 Linux）
 *   - Linux 的 ept 地址：从收到的第一条消息 hdr.src 学习（回显时用 dst=该地址）
 */
#ifndef M3_RPMSG_CORE_H
#define M3_RPMSG_CORE_H

#include <stdint.h>

#define RPMSG_BUF_SIZE      512u        /* hdr 16 + payload 496，与 Linux 一致 */
#define RPMSG_HDR_SIZE      16u
#define RPMSG_PAYLOAD_MAX   496u

#define RPMSG_NS_ADDR       53u         /* Linux 的 name service 端点 */
#define CORE1_EPT_ADDR      1u          /* core1 宣告的端点地址 */
#define RPMSG_NS_NAME       "rpmsg-ap3-ch0"

struct rpmsg_hdr {
    uint32_t src;
    uint32_t dst;
    uint32_t reserved;
    uint16_t len;
    uint16_t flags;
    uint8_t  data[];
} __attribute__((packed));

struct rpmsg_ns_msg {
    char     name[32];
    uint32_t addr;
    uint32_t flags;                     /* 0=CREATE 1=DESTROY */
} __attribute__((packed));

/* 状态 */
extern volatile int rpmsg_ready;        /* 握手完成、NS 已宣告 */

/* Linux 侧 ept 地址（从第一条消息 hdr.src 学习） */
int rpmsg_have_linux_ept(void);
uint32_t rpmsg_linux_ept(void);

/* 等待 Linux 握手门铃 → 初始化 vring → 宣告 NS 端点。返回 0 成功 */
int rpmsg_init(void);

/* 发送一条消息到指定 ept（dst）。返回实际发送字节数或负错误码 */
int rpmsg_send(uint32_t dst, const void *data, uint16_t len);

/* 轮询：处理 A2B 门铃（收 vq1）+ 发送待发消息。应在任务循环中反复调用 */
void rpmsg_poll(void);

/* 收到消息的回调（由 rpmsg_poll 调用）：默认回显 */
void rpmsg_on_recv(uint32_t src, const void *payload, uint16_t len);

#endif /* M3_RPMSG_CORE_H */
