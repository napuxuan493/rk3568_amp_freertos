/* M3: core1 侧 vring 环形队列（virtio slave 视角，匹配 Linux virtio master）
 *
 * 内存布局（与 Linux virtio_ring.c 的 vring_size/vring_init 完全一致）：
 *   vq0 @ 0x7c00000  = core1 发送队列（Linux 的 rvq "input"）：
 *                      Linux 放空缓冲到 avail，core1 填数据 → 放 used → 踢
 *   vq1 @ 0x7c08000  = core1 接收队列（Linux 的 svq "output"）：
 *                      Linux 放数据到 avail + A2B 门铃，core1 读 → 放 used 归还
 *
 * 每段（num=64, align=4KB）：
 *   offset 0x000  desc[64]  （16B/项：addr=缓冲物理地址, len, flags, next）
 *   offset 0x400  avail     （u16 flags + u16 idx + u16 ring[64]）
 *   offset 0x1000 used      （u16 flags + u16 idx + used_elem[64] 8B/项）
 *
 * 屏障规则（Linux 侧 virtio_wmb/dma_wmb 对称）：
 *   - 生产者：先写数据，dmb，再更新 idx（我们发：先填缓冲，dmb，再写 used idx）
 *   - 消费者：先读 idx，dmb，再读数据（我们收：先读 avail idx，dmb，再读缓冲）
 * core1 MMU/DCache 已关（startup.S），用 dmb sy 保证顺序即可。
 */
#ifndef M3_RPMSG_VRING_H
#define M3_RPMSG_VRING_H

#include <stdint.h>

#define VRING_NUM           64u
#define VRING_ALIGN         0x1000u

#define VQ0_BASE            0x07C00000UL   /* core1 TX（Linux rvq） */
#define VQ1_BASE            0x07C08000UL   /* core1 RX（Linux svq） */

#define VRING_DESC_SIZE     (16u)          /* sizeof(struct vring_desc) */
#define VRING_AVAIL_OFF     (VRING_NUM * VRING_DESC_SIZE)              /* 0x400 */
#define VRING_USED_OFF      VRING_ALIGN                                /* 0x1000 */

/* 与 uapi/linux/virtio_ring.h 一致（小端，AArch64 直接按位宽读写） */
struct vring_desc {
    uint64_t addr;      /* 缓冲的物理地址（0x8000000 shared-dma-pool） */
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;               /* 生产者写入的下一个槽位（递增游标） */
    uint16_t ring[VRING_NUM];   /* desc 索引数组 */
};

struct vring_used_elem {
    uint32_t id;                /* desc 索引 */
    uint32_t len;               /* 实际使用字节数 */
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;               /* 消费者写入的下一个槽位 */
    struct vring_used_elem ring[VRING_NUM];
};

struct vring {
    uint32_t num;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
};

/* 由 Linux 清零并初始化；core1 在收到握手门铃后调用 vring_init 建立视图 */
void vring_init(struct vring *vr, uintptr_t base);

/* 从 avail ring 取下一个 desc 索引（无新项返回 -1）。
 * last_avail 是 core1 本地的消费游标（调用方维护，初始 0）。 */
/* 修改前：int vring_get_avail(..., uint32_t *last_avail, uint32_t *desc_idx); */
/* 修改后：*/
int vring_get_avail(struct vring *vr, uint16_t *last_avail, uint16_t *desc_idx);
/* 把一个 desc 归还到 used ring（len = 实际写入的字节数） */
void vring_put_used(struct vring *vr, uint32_t desc_idx, uint32_t len);

/* 屏障 */
static inline void vring_mb(void) { __asm volatile("dmb sy" ::: "memory"); }

#endif /* M3_RPMSG_VRING_H */
