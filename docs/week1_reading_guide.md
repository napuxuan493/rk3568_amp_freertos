# 第一周导读：AMP 机制解析（读 SDK 驱动，为 M2 裸机移植做准备）

> 阅读对象都在 SDK 内核里，路径前缀 `$SDK/kernel/`：
> `$SDK = /home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux`
> 产出：一篇《AMP 机制解析》日志（放 `journal/`，延续 8_xx 编号习惯）

## 为什么读这三个文件

M2（FreeRTOS 移植到 core1）的**另一半**是 Linux 侧怎么把 core1 拉起来、怎么和它通信。
这三兄弟就是答案：

```
rockchip-mailbox.c  ── 最底层：两个核之间怎么"敲门"（硬件中断通道）
        │
rockchip_rpmsg.c    ── 中间层：在 mailbox 之上搭 virtio/vring 消息通道
        │
rockchip_amp.c      ── 最上层：启动/配置 core1（CPU 唤醒 + GIC 中断配置）
```

## 文件一：rockchip-mailbox.c（drivers/mailbox/，约 300 行，先读它）

Linux 通用 mailbox 框架（`drivers/mailbox/mailbox.c`）的 Rockchip 实现。

按顺序读：
1. `rockchip_mbox_of_match`（175 行附近）：兼容哪些芯片 → 确认 rk3568 在里面
2. `rockchip_mbox_send_data`（48 行）：**写寄存器发数据**——读 `writel` 的目标地址和值来源
3. `rockchip_mbox_read_msg`（121 行）+ `rockchip_mbox_irq`（140 行）：**中断来了怎么收**
4. `rockchip_mbox_startup`（74 行）：通道初始化时做什么

思考题：
- mailbox 的"通道"（channel）在 Rockchip 上对应什么硬件概念？
- `rockchip-mbox-demo.c` 是官方 demo，读它理解用户怎么用这个框架

## 文件二：rockchip_rpmsg.c（drivers/rpmsg/，约 410 行，核心）

它把 mailbox 封装成 **virtio 设备**（这就是 M3 你在 core1 侧要实现的东西的 Linux 参照物）。

按顺序读：
1. `struct rk_virtio_dev`（29 行）+ `struct rk_rpmsg_dev`（43 行）：两个数据结构各管什么
2. `rk_rpmsg_notify`（80 行）：**发消息时怎么通知对方**——这里调了 mbox
3. `rk_rpmsg_find_vq`（115 行）：**vring（virtqueue）怎么从共享内存里找/初始化**
4. `rk_rpmsg_config_ops`（245 行）：virtio 标准配置回调表（get_status/set_status/find_vqs...）
5. `rockchip_rpmsg_probe`（291 行）：解析 dts（mboxes/memory-region/reg）→ 注册 virtio 设备
6. `rk_rpmsg_rx_callback`（58 行）：**收到中断后，怎么把数据从 vring 里取出来**

思考题：
- `rockchip,vdev-nums` 和 `rockchip,link-id` 在 dts 里是干什么的（结合 rk3588-amp.dtsi 看）
- vring 的内存是 dts 里哪块区域？（对照 `rpmsg_reserved` / `rpmsg_dma_reserved`）

## 文件三：rockchip_amp.c（drivers/soc/rockchip/，约 625 行）

core1 的"启动器 + GIC 配置器"。

按顺序读：
1. `rockchip_amp_boot_cpus`（205 行）：**怎么把 core1 从 WFI 唤醒**（SMCCC/psci 调用）
2. `boot_cpu_store` / `boot_cpu_show`（140/94 行）：sysfs 接口 `/sys/.../boot_cpu` 怎么用
3. `amp_gic_get_irqs_config`（442 行）：**GIC 中断怎么分配给不同核**（这是 AMP 的关键！
   core1 要能自己收中断）
4. `rockchip_amp_probe`（514 行）：初始化顺序 = 时钟 → pinctrl → sysfs
5. `rockchip_amp_match`（609 行）：`rockchip,rk3568-amp` 就在这里 → 你的板子官方支持

思考题：
- 为什么 AMP 需要单独配 GIC？core1 中断打到哪个核由什么决定？
- `boot_on` 字段（236 行附近）是兼容旧方案的，猜猜它干嘛的

## 文件四：rk3588-amp.dtsi（kernel/arch/arm64/boot/dts/rockchip/，62 行，模板）

读完全部：`rockchip-amp` 节点 + 4 块 reserved-memory（amp 固件区 / rpmsg 区 / rpmsg_dma 池 / mcu 区）
+ `rpmsg` 节点（mboxes、vdev-nums、link-id）+ `mailbox0` 节点。

思考题（M2 你要为 rk3568 仿写一份）：
- 为什么固件区和 rpmsg 区要 `no-map`？（提示：Linux 不能碰这块内存，涉及页表）
- `memory-region = <&rpmsg_dma_reserved>` 是给谁用的 DMA 池？
- rk3568 的内存地址布局和 rk3588 不同，仿写时哪些值要改？

## 验收标准

- [ ] 能徒手画出这张图（不看文件）：Linux 发消息 → core1 收到的**完整路径**（哪几个函数）
- [ ] 能徒手画出：core1 发消息 → Linux 收到的路径
- [ ] 日志《AMP 机制解析》写完（含上面 4 组思考题的答案）
- [ ] 能在 `/sys` 下找到 AMP 相关的 sysfs 节点（在板子上跑时验证）
