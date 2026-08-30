/* M3: core1 侧 rpmsg 协议层实现 */
#include "rpmsg_core.h"
#include "rpmsg_mbox.h"
#include "rpmsg_vring.h"
#include "uart.h"        /* UART2 诊断 */
#include "FreeRTOS.h"
#include "task.h"

volatile int rpmsg_ready = 0;

static struct vring vq0;   /* core1 发送（Linux rvq） */
static struct vring vq1;   /* core1 接收（Linux svq） */
static uint16_t vq0_last_avail;   /* 必须是 16 位 */
static uint16_t vq1_last_avail;   /* 必须是 16 位 */

static uint32_t linux_ept_addr;   /* 从第一条消息 hdr.src 学习 */
static volatile int have_linux_ept;

int rpmsg_have_linux_ept(void) { return have_linux_ept; }
uint32_t rpmsg_linux_ept(void) { return linux_ept_addr; }

/* 从 vq 的 avail 取一个缓冲，返回缓冲物理地址（-1 无） */
static int vq_take_buffer(struct vring *vr, uint16_t *last_avail,
                          uintptr_t *buf_phys, uint16_t *desc_idx)
{
    uint16_t idx;  /* 修改：从 uint32_t 改为 uint16_t */

    if (vring_get_avail(vr, last_avail, &idx) < 0)
        return -1;

    *desc_idx = idx;
    *buf_phys = (uintptr_t)vr->desc[idx].addr;
    return 0;
}

// /* 握手：等待 Linux 的 first_notify 门铃（A2B ch3），校验 magic。
//  * 注意：Linux 的 rockchip_rpmsg 驱动在启动 ~6.6s 才 probe 并踢首个门铃，
//  * core1 从 u-boot 阶段就开始轮询，必须**无限等待**（不能设短超时）。 */
// static int rpmsg_wait_handshake(void)
// {
//     uint32_t data;

//     /* 等 Linux 的 first_notify 门铃（A2B_STATUS bit3）。
//      * 注意：不能读 A2B_CMD/DAT(0x20/0x24)——M3 实测 core3 读它们会触发
//      * 同步总线故障（[EXC]）。只靠 STATUS 位判断门铃到达。 */
//     while (!mbox_has_doorbell())
//         ;

//     mbox_read_doorbell(&data);   /* 只清 STATUS（见 rpmsg_mbox.c） */

//     /* Linux 刚 memset 了 vring，core1 重建视图并重置游标 */
//     vring_init(&vq0, VQ0_BASE);
//     vring_init(&vq1, VQ1_BASE);
//     vq0_last_avail = 0;
//     vq1_last_avail = 0;

//     uart_puts("[rpmsg] handshake OK\r\n");
//     uart_diag_u64("ST", MBOX_REG(MBOX_A2B_STATUS));   /* 诊断：STATUS 读回 */

//     /* 诊断：握手后立即探测 vring 共享内存可读性 */
//     uart_diag_u64("VRING0", *(volatile uint32_t *)VQ0_BASE);
//     uart_diag_u64("AVAIL",  *(volatile uint32_t *)(VQ0_BASE + VRING_AVAIL_OFF));
//     uart_diag_mark("NS-probe ok");

//     return 0;
// }



// static int rpmsg_wait_handshake(void)
// {
//     uart_puts("[RPMsg] rpmsg_wait_handshake_starting\r\n");
//     /* 替换旧的 while (!mbox_has_doorbell()) 轮询 */
//     /* 拥抱中断驱动：死等 222 中断发来的第一声握手门铃 */
//     ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
//     uart_puts("[RPMsg] rpmsg_wait_handshake\r\n");
//     /* 【重要】ISR 222 已经清除了 A2B_STATUS，所以千万不要再调用 mbox_read_doorbell！ */

//     /* Linux 刚 memset 了 vring，core1 重建视图并重置游标 */
//     vring_init(&vq0, VQ0_BASE);
//     vring_init(&vq1, VQ1_BASE);
//     vq0_last_avail = 0;
//     vq1_last_avail = 0;

//     uart_puts("[rpmsg] handshake OK\r\n");

//     /* 诊断：握手后立即探测 vring 共享内存可读性 */
//     uart_diag_u64("VRING0", *(volatile uint32_t *)VQ0_BASE);
//     uart_diag_u64("AVAIL",  *(volatile uint32_t *)(VQ0_BASE + VRING_AVAIL_OFF));
//     uart_diag_mark("NS-probe ok");

//     return 0;
// }

static int rpmsg_wait_handshake(void)
{
    uart_puts("[RPMsg] Polling for Linux handshake...\r\n");

    /* 1. 采用延时轮询，躲避 Linux 启动期间的 GIC 硬件重置 */
    while (!mbox_has_doorbell()) {
        vTaskDelay(pdMS_TO_TICKS(50)); 
    }

    uart_puts("[RPMsg] Doorbell arrived! Re-configuring IRQ 222...\r\n");

    /* 2. 【最关键的交换】必须先完整配置 GIC，再开启 Mailbox！ */
    #define MY_GICD_BASE 0xFD400000UL 
    uint32_t mbox_idx = 222 / 32;
    uint32_t mbox_bit = 222 % 32;

    *(volatile uint64_t *)(MY_GICD_BASE + 0x6000 + 222 * 8) = 0x300ULL;        /* 路由给 Core 3 */
    *(volatile uint32_t *)(MY_GICD_BASE + 0x0080 + mbox_idx * 4) |= (1u << mbox_bit); /* 分配 Group 1 */
    
    uint32_t cfg_reg = MY_GICD_BASE + 0x0C00 + (222 / 16) * 4;
    uint32_t cfg_bit = (222 % 16) * 2;
    *(volatile uint32_t *)cfg_reg &= ~(0x3u << cfg_bit);                       /* 电平触发 */

    uint32_t pri_reg = MY_GICD_BASE + 0x0400 + (222 / 4) * 4;
    uint32_t pri_lane = (222 % 4) * 8;
    uint32_t v = *(volatile uint32_t *)pri_reg;
    v = (v & ~(0xFFu << pri_lane)) | (0xE0u << pri_lane);
    *(volatile uint32_t *)pri_reg = v;                                         /* 设优先级 0xE0 */

    *(volatile uint32_t *)(MY_GICD_BASE + 0x0100 + mbox_idx * 4) |= (1u << mbox_bit); /* GIC 使能 */

    /* 3. 此时 GIC 已就绪。开启 Mailbox A2B 硬件中断！
     * 这句执行后，将瞬间触发正常 IRQ 222，进入 ISR 清除状态并唤醒任务，完美避开异常！ */
    mbox_init(); 

    /* 4. Linux 刚 memset 了 vring，core1 重建视图并重置游标 */
    vring_init(&vq0, VQ0_BASE);
    vring_init(&vq1, VQ1_BASE);
    vq0_last_avail = 0;
    vq1_last_avail = 0;

    uart_puts("[RPMsg] Handshake OK, switched to pure IRQ mode\r\n");

    return 0;
}

/* 宣告 NS 端点：让 Linux 建 "rpmsg-ap3-ch0" 通道（test 驱动 probe） */
/* NS 消息常量放在 .rodata：M3 调试(8/26)发现栈上写 nsm 会冻结，改用只读常量 */
static const struct rpmsg_ns_msg nsm_const = {
    .name = RPMSG_NS_NAME,
    .addr = CORE1_EPT_ADDR,
    .flags = 0,
};

static int rpmsg_ns_announce(void)
{
    const struct rpmsg_ns_msg *nsm = &nsm_const;
    uintptr_t buf;
    uint32_t desc_idx;
    int i;


    uart_diag_mark("NS-A pre-take");
    if (vq_take_buffer(&vq0, &vq0_last_avail, &buf, &desc_idx) < 0)
        return -1;
    uart_diag_u64("NS-B buf", buf);

    {
        struct rpmsg_hdr *hdr = (struct rpmsg_hdr *)buf;
        hdr->src = 0;                  /* NS 消息 src 用 0 */
        hdr->dst = RPMSG_NS_ADDR;
        hdr->reserved = 0;
        hdr->len = sizeof(*nsm);
        hdr->flags = 0;
        /* 拷贝 payload（从 .rodata 常量读，不写栈） */
        for (i = 0; i < (int)sizeof(*nsm); i++)
            ((uint8_t *)hdr->data)[i] = ((const uint8_t *)nsm)[i];
    }

    uart_diag_mark("NS-C pre-used");
    /* 放入 used ring 并踢门铃 */
    vring_put_used(&vq0, desc_idx, RPMSG_HDR_SIZE + (uint32_t)sizeof(*nsm));
    uart_diag_mark("NS-D pre-kick");
    if (mbox_kick() < 0)
        return -1;
    uart_diag_mark("NS-E post-kick");

    uart_puts("[rpmsg] NS announced\r\n");
    return 0;
}

int rpmsg_init(void)
{
    /* 【删除所有的 msr daifset 汇编，解除 IRQ 屏蔽】 */
    /* 【删除 mbox_init() 的调用，因为它已经移到了 handshake 里面】 */

    if (rpmsg_wait_handshake() < 0)
        return -1;

    if (rpmsg_ns_announce() < 0) {
        uart_puts("[rpmsg] NS announce failed\r\n");
        return -1;
    }

    rpmsg_ready = 1;
    return 0;
}

int rpmsg_send(uint32_t dst, const void *data, uint16_t len)
{
    uintptr_t buf;
    uint32_t desc_idx;
    int i;

    if (len > RPMSG_PAYLOAD_MAX)
        len = RPMSG_PAYLOAD_MAX;

    /* 等 vq0 有空缓冲（Linux 回收 used → 重新放入 avail，但不再踢门铃，
     * 所以这里必须轮询 vq0 的 avail idx） */
    while (vq_take_buffer(&vq0, &vq0_last_avail, &buf, &desc_idx) < 0) {
        /* 顺便处理门铃（Linux 可能同时发消息过来） */
        rpmsg_poll();
    }

    {
        struct rpmsg_hdr *hdr = (struct rpmsg_hdr *)buf;
        hdr->src = CORE1_EPT_ADDR;
        hdr->dst = dst;
        hdr->reserved = 0;
        hdr->len = len;
        hdr->flags = 0;
        for (i = 0; i < (int)len; i++)
            hdr->data[i] = ((const uint8_t *)data)[i];
    }

    vring_put_used(&vq0, desc_idx, RPMSG_HDR_SIZE + (uint32_t)len);
    if (mbox_kick() < 0)
        return -1;

    return len;
}

void rpmsg_on_recv(uint32_t src, const void *payload, uint16_t len)
{
    /* 学习 Linux 的 ept 地址（第一条消息） */
    if (!have_linux_ept) {
        linux_ept_addr = src;
        have_linux_ept = 1;
        uart_diag_u64("linux_ept", linux_ept_addr);
    }

    /* 回显：把收到的内容原样发回去（Linux test 驱动 ping-pong） */
    if (len > 0 && len <= RPMSG_PAYLOAD_MAX) {
        rpmsg_send(src, payload, len);
        uart_puts("[rpmsg] echo\r\n");
    }
}

/* 处理一条来自 vq1 的消息 */
static void rpmsg_handle_vq1(void)
{
    uintptr_t buf;
    uint32_t desc_idx;

    while (vq_take_buffer(&vq1, &vq1_last_avail, &buf, &desc_idx) == 0) {
        struct rpmsg_hdr *hdr = (struct rpmsg_hdr *)buf;
        uint16_t len = hdr->len;

        if (len > RPMSG_PAYLOAD_MAX)
            len = RPMSG_PAYLOAD_MAX;

        /* 忽略 NS 相关（core1 是 slave，不处理 Linux 的 NS） */
        if (hdr->dst == CORE1_EPT_ADDR || hdr->dst == 0) {
            uart_diag_u64("rx", hdr->src);
            rpmsg_on_recv(hdr->src, hdr->data, len);
        }

        /* 归还缓冲到 vq1 used，让 Linux 复用 */
        vring_put_used(&vq1, desc_idx, 0);
        if (mbox_kick() < 0)
            break;
    }
}

void rpmsg_poll(void)
{
    if (!rpmsg_ready)
        return;

    /* M3(8/26)：直接轮询 vq1 的 avail idx 收消息（不用 mailbox 门铃，
     * 门铃路径在 core3 上触发异常）。 */
    rpmsg_handle_vq1();
}
