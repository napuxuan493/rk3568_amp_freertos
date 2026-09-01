# M3 设计：core1 (FreeRTOS) ↔ Linux 双向 rpmsg 通信（中断驱动版）

日期：2026-08-26（初版） / 2026-08-30（更新为中断驱动实现）
状态：**已达成**（Linux rx_count 10000+，乒乓闭环稳定）

> 更新说明：初版设计用"轮询 mailbox STATUS + 轮询 vring"绕开 mailbox 中断
> （当时 GIC 未配置 mailbox SPI → 中断以 EC=0 异常投递导致崩溃）。
> **现已正确实现中断驱动方案**：在 core3 的 GIC 里配置 mailbox A2B 中断
> (INTID 222)，由 ISR 唤醒 rpmsg 任务，彻底解决。

---

## 一、架构：控制面（mailbox 门铃中断）+ 数据面（vring 共享内存）

```
┌───────────── Linux (core0, virtio master) ─────────────┐   ┌────── core1 (core3, virtio slave) ───────┐
│  rockchip_rpmsg.c  virtio master                      │   │  FreeRTOS: vRpmsgTask (任务通知驱动)      │
│   vq0(rvq) ← 收: core1 填 used + B2A 门铃              │   │   vq0: 发(avail取缓冲→填→used→B2A kick)   │
│   vq1(tvq) → 发: 填 avail + A2B 门铃(中断 222)        │   │   vq1: 收(avail取→解析→used归还)          │
│  mailbox: rx=ch0(B2A)  tx=ch3(A2B)                    │   │   mailbox: A2B 中断(222)唤醒任务           │
│  test 驱动收到必回(ping-pong)                          │   │   ISR: 清 STATUS + vTaskNotifyGiveFromISR  │
└───────────────────────────────────────────────────────┘   └───────────────────────────────────────────┘
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
| **A2B 中断号** | **INTID 222**（BB 侧，core3 的 GIC） | 实测定位（非 dts 的 183-186） |
| B2A_CMD(0)/DAT(0) | 0x30 / 0x34（core1 踢） | rockchip-mailbox.c |
| B2A_STATUS | 0x2C bit0（core1 发送前查忙） | 同上 |
| A2B_STATUS | 0x04 bit3（ISR 里 W1C 清） | 同上 |
| A2B_CMD(3)/DAT(3) | 0x20 / 0x24（Linux 敲门铃） | 同上 |
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
- **屏障**：先写数据/desc，再 dmb；再更新 idx（无 cache 裸机用 `dmb sy`）

## 四、core1 侧模块（中断驱动版）

| 文件 | 职责 |
|---|---|
| `rpmsg_mbox.c/h` | mailbox 门铃：A2B_INTEN 使能、ISR 清 STATUS、B2A kick |
| `rpmsg_vring.c/h` | vring 结构 + virtqueue 从侧操作：get_avail / put_used / idx / 屏障 |
| `rpmsg_core.c/h` | rpmsg 协议：握手、NS 宣告、学习 ept、收（榨干 vq1）、回显 |
| `main.c` | vRpmsgTask：`ulTaskNotifyTake` 死等唤醒 → `rpmsg_poll()`；ISR 处理 222 |
| `gic.c` | **mailbox A2B 中断(222)的 GIC 配置**（IGROUPR/ISENABLER/优先级） |

## 五、通信流程（中断驱动闭环）

### 握手（阻塞等待 + 握手后重配 GIC）
1. core1 启动 → `rpmsg_init()` **阻塞等待**（`ulTaskNotifyTake(pdTRUE, portMAX_DELAY)`）第一个 222 门铃
2. Linux boot（~6.6s）→ rpmsg probe → vring 清零 → 加 rx 缓冲 → **A2B 门铃 → 打 222 中断**
3. core1 ISR：清 A2B_STATUS(W1C) → `vTaskNotifyGiveFromISR` 唤醒握手
4. **握手完成后必须重新初始化 mailbox 中断的 GIC 配置** —— 因为 Linux 完全启动时会覆盖共享的 GICD 配置（GICD_CTLR/IGROUPR/IROUTER 是两核共享的）
5. core1 发 NS 宣告 → Linux 建通道 → test 驱动 probe → Linux 主动发消息

### 数据流（闭环）
- **core1→Linux**：vq0 avail 取缓冲 → 写 hdr+payload → vq0 used → **B2A kick** → Linux vring_interrupt 交付 → test 驱动 `rx_count++` → **test 驱动收到必回一条**
- **Linux→core1**：Linux 填 vq1 avail + **A2B 门铃(打 222)** → core1 ISR 清 STATUS + 唤醒任务 → `rpmsg_poll()` 榨干 vq1 → 剥 hdr → `rpmsg_on_recv` **回显**（`rpmsg_send(src,...)`）→ B2A kick → Linux 再回……
- **闭环**：test 驱动的"收到必回" + core1 的"收到必回显" = 无限乒乓

### rpmsg_hdr（16 字节，小端）
```c
struct rpmsg_hdr { u32 src; u32 dst; u32 reserved; u16 len; u16 flags; u8 data[]; }
```

### NS 消息（40 字节 payload）
```c
struct rpmsg_ns_msg { char name[32]; u32 addr; u32 flags; }  /* flags: CREATE=0 */
```

## 六、关键教训（中断驱动踩过的坑）

1. **GIC 必须配置 mailbox 的 BB 侧中断**（INTID 222）—— 不配置时门铃以
   EC=0 未定义同步异常投递 → core1 崩溃（当初轮询方案就是为了绕开它）
2. **电平中断必须 W1C 清 STATUS** —— ISR 里不清会中断风暴/重复触发
3. **共享寄存器会被对方覆盖** —— GICD 是两核共享的，Linux 启动后必须重配
4. **握手必须阻塞等待** —— Linux 要 ~6.6s 才 probe，短超时/轮询窗口会错过

## 七、Linux 侧配套

- `CONFIG_RPMSG_ROCKCHIP_TEST=y`（`rockchip_linux_defconfig` L787）
- 测试驱动：`kernel/drivers/rpmsg/rockchip_rpmsg_test.c`（L28 回调、L47 自动回复、
  L93 匹配 "rpmsg-ap3-ch0"）
- ⚠️ `MSG_LIMIT 10000`：乒乓跑到 1 万次后 test 驱动退出，需长期跑就改大/注释

## 八、验收标准（已达成）

1. core1 周期发消息 → Linux dmesg 看到 `rx msg ... rx_count++`（10000+）
2. Linux 发消息 → core1 中断唤醒 → 回显 → Linux 再收到（乒乓闭环）
3. 连续运行稳定；调试串口干净（诊断标记可控）
