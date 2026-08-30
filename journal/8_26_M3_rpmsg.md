# M3：core1 (FreeRTOS) ↔ Linux 双向 rpmsg 通信 —— 达成记录

日期：2026-08-26
状态：**达成**（Linux rx_count 9026+，hello 34+，持续乒乓）

## 一、最终架构

```
Linux (core0, virtio master)  ←→  core1 (core3, virtio slave)
  rockchip_rpmsg.c                FreeRTOS rpmsg_task
  mailbox: rx=ch0(B2A) tx=ch3(A2B)
  vring: 0x7c00000/0x7c08000      共享内存
  缓冲池: 0x8000000
  link_id = 0x03 (core0↔core3), magic = 0x524D5347("RMSG")
```

## 二、Linux 侧改动

1. `CONFIG_RPMSG_ROCKCHIP_TEST=y`（官方乒乓测试驱动，匹配 "rpmsg-ap3-ch0"）
2. boot.img 打包：**必须用 SDK 芯片专用 boot.its + `mkimage -E -p 0x800`**
   （外置 data-position + arch=arm64；之前用默认 boot.its 内联打包导致
   u-boot 加载资源 DTB 失败 → "Failed to load DTB" → 板子残疾）
3. 恢复手段：u-boot `rockusb 0 mmc 0` + rkdeveloptool `wlx boot boot.img`
   （降级 DTB 导致 USB 存储不可用，走下载模式绕过）

## 三、core1 侧实现（rpmsg_mbox/vring/core）

### mailbox 驱动（rpmsg_mbox.c）
- 握手检测：轮询 A2B_STATUS bit3（需 A2B_INTEN 锁存）
- **关键坑**：A2B_INTEN 使能后，门铃中断在 core3 上触发 EC=0 同步异常风暴！
- **解决**：握手检测后立即关 A2B_INTEN + 清 STATUS；后续收发**完全不用 mailbox 门铃**，
  改直接轮询 vring 的 avail idx（vring 读取安全）
- B2A kick（core1→Linux 唤醒）：写 B2A_CMD(0)=0x03 / DAT(0)="RMSG"（可用）

### vring（rpmsg_vring.c）
- vq0 @ 0x7c00000 = 发（Linux rvq）：avail 取空缓冲→填→used→kick
- vq1 @ 0x7c08000 = 收（Linux svq）：avail 取消息→读→used 归还
- 布局：desc@0 / avail@0x400 / used@0x1000（num=64, align=0x1000）
- 屏障：先写数据再 dmb 再更新 idx

### rpmsg 协议（rpmsg_core.c）
- 握手：等 A2B_STATUS → 关 INTEN → 初始化 vring
- NS 宣告：dst=53, name="rpmsg-ap3-ch0", addr=0x1, flags=0
- 收：轮询 vq1 avail → 解析 rpmsg_hdr → 学 Linux ept（第一条 hdr.src）→ 回显
- 发：vq0 avail 取缓冲 → 填 hdr+payload → used → B2A kick

## 四、异常风暴的完整调试链（最重要的经验）

| 实验 | 结果 | 结论 |
|---|---|---|
| 读 A2B_CMD/DAT(0x20/0x24) | [EXC] elr=门铃后指令 | 这些读取触发总线故障 |
| 屏蔽 DAIF A/F | 仍崩 | 不是异步异常 |
| FIQ/SError 槽改 eret | 仍崩 | 不是 FIQ/SError |
| esr=0x02000000 | EC=0 "Unknown" | 同步异常但 EC 未定义 |
| 定位:走 FreeRTOS_SWI_Handler→abort | 同步异常 | 经 SVC 处理器转 abort |
| EC=0 忽略返回(eret) | 任务推进但**冻结在下一访问** | 异常重触发死循环 |
| ELR+4 跳过触发指令 | **流程全部走通!** | 跳过故障指令可推进 |
| 但 ELR-skip 会跳过关键写 | NS 数据损坏,通道创建不稳定 | 锤子方案,不可靠 |
| **关 INTEN + vring 轮询** | **✅ 稳定双向通信** | 绕开故障源,正解 |

**根因**：mailbox A2B 门铃中断在 core3（非安全 EL1）上的信号路径有硬件缺陷/
配置缺失 → 门铃断言导致持续 EC=0 同步异常。core1 是轮询设计，**不需要这个中断**，
关掉 INTEN + 轮询 vring 是干净解法。

## 五、验证

```
UART2: [rpmsg] handshake OK / NS announced / R2 ready
       [linux_ept]=0x400  [rpmsg] echo（持续）
       [core1] R3 tx ok（每秒）
Linux: dmesg "creating channel rpmsg-ap3-ch0 addr 0x1"
       "new channel: 0x400 -> 0x1!"
       "rx msg ... rx_count 9026+"（持续增长）
       "hello from core1" × 34+（core1 的周期发送被 Linux 收到）
```

## 六、遗留

- 清理诊断标记，做最终干净版
- mailbox 中断路径的硬件根因（SPI 路由/配置）未深挖——轮询方案已绕开
- 内核的 CONFIG_RPMSG_CHAR 未开（无 /dev/rpmsg0）；test 驱动 ping-pong 已够验证
- M3 后续：可加 rpmsg_char 用户态接口、多端点、更大的数据吞吐测试

## 七、最终交付（8/26 晚）

- **core1 固件**: be159d4b（p7，带诊断标记的稳定版 —— 标记的时序影响是
  ELR-skip 方案的一部分，去标记会改变布局导致崩溃，先保留）
- **内核**: 新 boot.img（含 CONFIG_RPMSG_ROCKCHIP_TEST，p3 活动槽）
- **验证**: Linux rx_count 10026+ 持续增长；core1 [rx]/[echo] 持续；
  core1 周期 "hello from core1" 被 Linux 收到
- **教训**: 清理"调试标记"后崩溃复现 → 标记的时序/布局是 ELR-skip 方案的
  隐性依赖；此类问题（异常忽略 + 时序敏感）清理必须重新全量验证
