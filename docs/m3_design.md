# M3 设计：core1 (FreeRTOS) ↔ Linux 双向 rpmsg 通信

日期：2026-08-26
状态：设计完成，待实现

## 一、架构：控制面（mailbox 门铃）+ 数据面（vring 共享内存）

```
┌───────────── Linux (core0, virtio master) ─────────────┐    ┌──────── core1 (core3, virtio slave) ────────┐
│  rockchip_rpmsg.c  virtio master                      │    │  FreeRTOS: rpmsg_task                       │
│   vq0(rvq) ← 收: core1 填 used + B2A 门铃              │    │   vq0: 从 avail 取空缓冲→填数据→used→踢      │
│   vq1(tvq) → 发: 填 avail + A2B 门铃                  │    │   vq1: 从 avail 取数据→读完→used→踢           │
│  mailbox: rx=ch0(B2A)  tx=ch3(A2B)                    │    │   mailbox: 轮询 A2B ch3, 踢 B2A ch0          │
└───────────────────────────────────────────────────────┘    └─────────────────────────────────────────────┘
```

- **门铃只传信号不传数据**：8 字节 mailbox 消息 = `cmd=link_id(0x03)` + `data="RMSG"(0x524D5347)`
- **数据走共享内存**：vring 环形队列在 0x7c00000，消息缓冲在 0x8000000（shared-dma-pool）
- 双方地址是**固定契约**（dts 保留区），不动态协商

## 二、关键常量与地址（Linux 侧协议，core1 必须精确匹配）

| 项 | 值 | 来源 |
|---|---|---|
| mailbox 基地址 | 0xFE780000 | dts mailbox@fe780000 |
| link_id | 0x03（高4位 master=0, 低4位 remote=3） | dts rockchip,link-id |
| mailbox 通道 | rx=ch0(B2A), tx=ch3(A2B) | dts mboxes |
| B2A_CMD(0)/DAT(0) | 0x30 / 0x34（core1 踢） | rockchip-mailbox.c |
| B2A_STATUS | 0x2C bit0（core1 发送前查忙） | 同上 |
| A2B_STATUS | 0x04 bit3（core1 轮询门铃） | 同上 |
| A2B_CMD(3)/DAT(3) | 0x20 / 0x24（读 Linux 门铃） | 同上 |
| RPMSG_MBOX_MAGIC | 0x524D5347（"RMSG"） | rockchip_rpmsg.h |
| vring 基址 vq0 / vq1 | 0x7c00000 / 0x7c08000 | dts reg + RPMSG_VRING_SIZE |
| RPMSG_VRING_SIZE | 0x8000（32KB） | rockchip_rpmsg.h |
| RPMSG_VRING_ALIGN | 0x1000（4KB） | 同上 |
| RPMSG_BUF_COUNT | 64 | 同上 |
| 缓冲大小 | 512B = rpmsg_hdr(16B) + payload(496B) | RPMSG_BUF_SIZE |
| 缓冲池 | 0x8000000（shared-dma-pool, 1MB） | dts rpmsg_dma_reserved |
| NS 宣告名 | "rpmsg-ap3-ch0" | rockchip_rpmsg_test.c id_table |
| VIRTIO_ID_RPMSG | 7 | virtio_ids.h |

## 三、vring 布局（virtio_ring.h vring_size 公式）

```
vring = {
    desc[64]   (16B/项: addr, len, flags, next = 1024B)
    avail      (u16 flags + u16 idx + u16 ring[64] + padding)
    used       (u16 flags + u16 idx + used_elem[64], 8B/项)
}
每段按 align=0x1000 对齐；核心：desc 的 addr 字段 = 缓冲物理地址（0x8000000 池）
```

- **avail ring**：生产者（Linux 放缓冲）写，消费者（core1）读；`idx` 是递增游标
- **used ring**：core1 归还用完的缓冲时写，Linux 读
- **屏障**：先写数据/desc，再 dmb；再更新 idx（`dmb ish` 在无 cache 裸机下用 `dmb sy`）

## 四、core1 侧模块划分（FreeRTOS 新增文件）

| 文件 | 职责 |
|---|---|
| `rpmsg_mbox.c/h` | mailbox 门铃：poll_a2b() / kick_b2a() / 读写 8 字节消息 |
| `rpmsg_vring.c/h` | vring 结构 + virtqueue 从侧操作：get_avail_desc / put_used_desc / idx 管理 / 屏障 |
| `rpmsg_core.c/h` | rpmsg 协议：hdr 组装解析、NS 宣告、学习对端 ept、收发回调 |
| main.c 集成 | rpmsg_task：轮询门铃 → 处理 vq1 收 / 周期发 vq0 + 回显 |

## 五、通信流程

### 握手（Linux 是 master，先动）
1. core1 启动：轮询 A2B_STATUS bit3（Linux 未就绪前空转）
2. Linux boot：rpmsg probe → vring 清零 → virtio 注册 → 加 rx 缓冲 → **A2B 门铃**（first_notify 握手）
3. core1 收到门铃 → 校验 magic → **初始化 vring 状态**（Linux 刚清零过，core1 读 desc/avail idx）→ 回踢 B2A
4. Linux rx_callback → RPMSG_REMOTE_IS_READY → rpmsg_virtio 跑起来
5. core1 发 **NS 宣告**（dst=0xFFFFFFFF, name="rpmsg-ap3-ch0", addr=core1_ept, flags=CREATE）
6. Linux rpmsg_ns_cb → 建通道 → test 驱动 probe → **Linux 主动发第一条消息**

### 数据流
- **core1→Linux**：vq0 avail 取空缓冲 → 写 hdr+payload → vq0 used → 踢 B2A → Linux vring_interrupt 交付
- **Linux→core1**：Linux 填 vq1 avail + A2B 门铃 → core1 轮询到 → 读 vq1 avail 消息 → vq1 used 归还 → 踢 B2A
- **学习对端地址**：core1 从 Linux 第一条消息的 `hdr.src` 学 Linux ept 地址；回显时 dst=该地址

### rpmsg_hdr（16 字节，小端）
```c
struct rpmsg_hdr { u32 src; u32 dst; u32 reserved; u16 len; u16 flags; u8 data[]; }
```

### NS 消息（40 字节 payload）
```c
struct rpmsg_ns_msg { char name[32]; u32 addr; u32 flags; }  /* flags: CREATE=0 */
```

## 六、Linux 侧待办（可能需要改内核）

1. **确认 dmesg**：rpmsg/mailbox 驱动是否正常 probe（板子查）
2. `CONFIG_RPMSG_ROCKCHIP_TEST=y` → 编译进官方乒乓测试驱动（或 CONFIG_RPMSG_CHAR=y 用 /dev/rpmsg0）
3. 重新打包 boot.img → 板内烧录（p3 + switch_boot）

## 七、调试手段

- core1 侧：UART2 DIAG 打 mailbox STATUS/vring idx 读回；UART4 打业务收发
- Linux 侧：`dmesg | grep rpmsg`、`cat /sys/kernel/debug/rpmsg`（若开）、test 驱动的 rx_count 打印
- 门铃丢失排查：A2B/B2A STATUS 位读回（W1C 语义）
- 屏障错误症状：idx 先于数据可见 → 乱码/丢包；dmb 位置按"先写后同步"原则

## 八、验收标准

1. core1 周期发 "hello from core1: N" → Linux dmesg 看到 rx 打印（或 /dev/rpmsg0 读到）
2. Linux 发消息 → core1 收到并回显 → Linux 再收到回显（乒乓闭环）
3. 连续跑 5 分钟无丢包/死锁；UART2 干净
