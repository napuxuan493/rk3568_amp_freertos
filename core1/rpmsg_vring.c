/* M3: core1 侧 vring 环形队列实现 */
#include "rpmsg_vring.h"

void vring_init(struct vring *vr, uintptr_t base)
{
    vr->num = VRING_NUM;
    vr->desc  = (struct vring_desc  *)(base + 0x0000);
    vr->avail = (struct vring_avail *)(base + VRING_AVAIL_OFF);
    vr->used  = (struct vring_used  *)(base + VRING_USED_OFF);
}

/* 修改前：int vring_get_avail(..., uint32_t *last_avail, uint32_t *desc_idx) */
/* 修改后：*/
int vring_get_avail(struct vring *vr, uint16_t *last_avail, uint16_t *desc_idx)
{
    uint16_t avail_idx; /* 必须是 16 位 */

    /* 1. 读生产者 idx */
    avail_idx = vr->avail->idx;

    /* 2. 判断是否相等 (利用 16 位自然溢出特性) */
    if (*last_avail == avail_idx)
        return -1;

    /* 3. 屏障 */
    vring_mb();

    *desc_idx = vr->avail->ring[*last_avail % vr->num];

    /* 4. 消费：本地游标前进 (16位累加，满 65535 自动归零) */
    (*last_avail)++;

    return 0;
}

void vring_put_used(struct vring *vr, uint32_t desc_idx, uint32_t len)
{
    uint32_t used_idx = vr->used->idx;

    /* 先写 used 项，再 dmb，再更新 idx —— 消费者（Linux）先见 idx 后读内容 */
    vr->used->ring[used_idx % vr->num].id  = desc_idx;
    vr->used->ring[used_idx % vr->num].len = len;
    vring_mb();
    vr->used->idx = used_idx + 1;
}
