# M3 双向 rpmsg 通信 —— 完整调试日志与代码变更索引

日期：2026-08-26
状态：**达成**（Linux rx_count 10026+，core1 周期 hello 34+，双向持续）
核心固件：`core1/amp.img` = be159d4b（p7 当前）
内核：boot.img（含 CONFIG_RPMSG_ROCKCHIP_TEST，p3 活动槽）

---

## 〇、文档目的

本文档 = M3 全过程日志 + **所有代码改动的位置索引**（文件:行号/函数 + 改了什么），
方便后续查阅与维护。

---

## 一、架构与协议（先建立全局图）

```
Linux(core0, virtio master)              core1(core3, virtio slave)
  rockchip_rpmsg.c  virtio master          FreeRTOS rpmsg_task
  mailbox: rx=ch0(B2A)  tx=ch3(A2B)        mailbox 仅握手 + B2A kick
  vq0@0x7c00000 = Linux rvq                 ←core1 发：avail 取空缓冲→填→used→kick
  vq1@0x7c08000 = Linux svq                 ←core1 收：轮询 avail→读→used 归还
  缓冲池 0x8000000 (shared-dma-pool)
  link_id=0x03, magic=0x524D5347("RMSG"), NS addr=53, core1 ept=0x1
```

数据流（最终实现，**核心设计决策：收发全部轮询 vring，不用 mailbox 门铃**）：
- **握手**：轮询 mailbox A2B_STATUS bit3（需 A2B_INTEN 锁存）→ 检测后**立即关
  A2B_INTEN + 清 STATUS**（门铃中断在 core3 上触发 EC=0 异常风暴，必须绕开）
- **core1→Linux**：vq0 avail 取缓冲（Linux 回收后不踢门铃，必须轮询）→ 填
  rpmsg_hdr+payload → vq0 used → B2A kick（mbox_kick 可用）
- **Linux→core1**：轮询 vq1 avail idx（不用门铃）→ 解析 → vq1 used 归还 → B2A kick

---

## 二、代码变更清单（文件 → 位置 → 内容）

### 2.1 新增文件（core1 侧，全部在 /home/na/rk3568_amp/core1/）

| 文件 | 行数 | 职责 |
|---|---|---|
| rpmsg_mbox.h/c | 57/38 | mailbox 门铃：A2B_STATUS 握手检测、B2A kick、INTEN 开关 |
| rpmsg_vring.h/c | 79/43 | vring 布局 + 从侧 virtqueue 操作（avail 取/used 还 + 屏障） |
| rpmsg_core.h/c | 63/220 | rpmsg 协议：握手、NS 宣告、收（轮询 vq1）、发、回显 |

**rpmsg_mbox.c 关键位置**（最终稳定版逻辑）：
```c
void mbox_init(void)            // L27-33：开 A2B_INTEN（握手检测需要）
uint32_t mbox_read_doorbell()   // L36-44：关 INTEN + W1C 清 STATUS（止异常）
int mbox_kick(void)             // L46-63：查 B2A_STATUS 空闲→写 B2A_CMD(0)/DAT(0)
```

**rpmsg_vring.c 关键位置**：
```c
void vring_init(vr, base)       // L8-14：desc@+0 / avail@+0x400 / used@+0x1000
int vring_get_avail(...)        // L17-33：读 avail idx→dmb→取 desc→游标++
void vring_put_used(...)        // L36-43：写 used 项→dmb→idx++
```

**rpmsg_core.c 关键位置**（最终稳定版）：
```c
rpmsg_wait_handshake()          // L60-85：轮询 A2B_STATUS → 关 INTEN → 初始化 vring
rpmsg_ns_announce()             // L98-140：NS 消息(常量放 .rodata) → vq0 used → B2A kick
rpmsg_send(dst,data,len)        // L150-175：vq0 avail 取缓冲→填 hdr→used→kick
rpmsg_on_recv(src,payload,len)  // L181-193：学 Linux ept + 回显
rpmsg_handle_vq1()              // L196-214：轮询 vq1 avail→解析 hdr→on_recv→归还
rpmsg_poll()                    // L216-222：直接调 rpmsg_handle_vq1（无门铃）
```
注意：NS 消息用 **static const（.rodata）** 而非栈上结构体（L90-96）——
栈写曾触发冻结（见 §三 实验 12）。

### 2.2 修改文件

#### (1) main.c（core1 入口，新增 rpmsg_task）
- **L15**：`#include "rpmsg_core.h"`
- **L62-100**：`vRpmsgTask` 任务（握手→循环：rpmsg_poll + 每秒发 hello）
- **L141**：`xTaskCreateStatic(vRpmsgTask, "rpmsg", ..., 2, ...)`（优先级 2）

#### (2) Makefile（core1 构建）
- **L19-20**：SRCS 增加 `rpmsg_mbox.c rpmsg_vring.c rpmsg_core.c`
- **L45-56**：amp.img 目标，**必须用 SDK mkimage**（M2.2 的 totalsize 坑）

#### (3) startup.S（向量表 + ignore_async）
- **L66-79**：FIQ/SError 槽从 `abort_handler` 改为 `ignore_async`
- **L84-92**（新增函数）：
```asm
ignore_async:
    mrs     x0, elr_el1      /* EC=0 门铃噪声：跳过触发指令再返回 */
    add     x0, x0, #4
    msr     elr_el1, x0
    eret
```

#### (4) portASM.S（FreeRTOS port，SVC 处理器）
- **L219-250 FreeRTOS_SWI_Handler**：
  - 开头加：`MRS ESR → LSR #26 → CMP #0 → B.EQ ignore_async`（EC=0 忽略）
  - portSAVE_CONTEXT **之后重新读 ESR**（portSAVE_CONTEXT 破坏 X0/X1，
    不重读会导致 SVC 判断错误 → 误入 abort）
- **L244-247 FreeRTOS_Abort**：保留 `B abort_handler`（真实 abort 仍诊断）

#### (5) port.c（FreeRTOS port，任务初始 PSTATE）
- **L101-108 portINITIAL_PSTATE**：
  `portEL1 | portSP_EL0 | 0x140`（加 A=bit8、F=bit6 屏蔽 —— 实验产物，
  最终被 ignore_async 方案取代，可保留可回退）
- **L302-320 FreeRTOS_Tick_Handler**：移除 ICC_RPR 断言（M2.2 遗留，非本实验）

#### (6) gic.c（M2.2 遗留，未改于本会话）
- gic_init 末尾 3ms settle 延时（M2.2 时序竞争修复）

#### (7) 内核侧
- `kernel/arch/arm64/configs/rockchip_linux_defconfig` **L787**：
  新增 `CONFIG_RPMSG_ROCKCHIP_TEST=y`（官方乒乓测试驱动，匹配 "rpmsg-ap3-ch0"）
- boot.img 打包：**官方流程** `./build.sh kernel`（mk-kernel.sh → mk-fitimage.sh
  用芯片 boot.its + `mkimage -E -p 0x800`）；本次为手动等效路径

#### (8) 工具脚本（/home/na/rk3568_amp/tools/）
- bootcap.py / uartctl.py / fit_parse.py（M2.2 存的调试工具，本会话复用）

---

## 三、关键调试过程（异常风暴完整排查链）

**现象**：mailbox 门铃到达后，core1 在"门铃消费后的下一条指令"崩溃 [EXC]。

| # | 实验 | 代码位置 | 结果 | 结论 |
|---|---|---|---|---|
| 1 | 读 A2B_CMD/DAT(0x20/0x24) | rpmsg_mbox.c | [EXC] elr=门铃后指令 | 这些读取触发总线故障 |
| 2 | 屏蔽 DAIF A/F | rpmsg_core.c + port.c | 仍崩 | 不是异步异常 |
| 3 | FIQ/SError 槽改 eret | startup.S | 仍崩 | 不是 FIQ/SError |
| 4 | 解码 esr=0x02000000 | — | EC=0 "Unknown" | 同步异常但 EC 未定义 |
| 5 | 定位：走 SWI_Handler→abort | portASM.S | 同步异常 | 经 SVC 处理器转 abort |
| 6 | EC=0 忽略（eret） | portASM.S | 任务推进但**冻结在下一访问** | 异常重触发死循环 |
| 7 | **ELR+4 跳过触发指令** | startup.S ignore_async | **流程全部走通!** | 跳过故障指令可推进 |
| 8 | 但 ELR-skip 跳过关键写 | 多次 | NS 数据损坏、通道创建不稳定 | 锤子方案，布局敏感 |
| 9 | GIC ack+EOI（IAR1 读） | startup.S | IAR 读取阻塞 | 不是 Group1 IRQ |
| 10 | 关 A2B_INTEN | rpmsg_mbox.c | 流程通但**后续门铃不锁存** | 收发要改轮询 |
| 11 | **vring 直接轮询 avail idx** | rpmsg_core.c | **✅ 稳定双向通信** | 绕开故障源，正解 |
| 12 | 栈上写 nsm 冻结 | rpmsg_core.c | NS 卡死 | NS 结构体改 .rodata 常量 |
| 13 | 清理标记后崩溃 | 全文件 | vring_put_used 真实 data abort | 标记时序/布局是 ELR-skip 依赖 |

**最终方案**：mailbox 只做一次性握手（关 INTEN 止异常），收发全走 vring 轮询。

---

## 四、验证结果（当前板子）

```
UART2: [rpmsg] handshake OK / NS announced / R2 ready
       [linux_ept]=0x400  [rx]=0x400(刷屏,证明持续收)  [rpmsg] echo(持续)
       [core1] R3 tx ok(每秒)
Linux: dmesg "creating channel rpmsg-ap3-ch0 addr 0x1"
       "new channel: 0x400 -> 0x1!"
       "rx msg ... rx_count 10026+"(持续增长)
       "hello from core1" × 34+
```

## 五、当前状态与遗留

- 板子跑稳定版 be159d4b（带诊断标记；标记是 ELR-skip 方案的隐性时序依赖，
  **清理必须重新全量验证**）
- 内核 defconfig 已补 RPMSG test，以后 `./build.sh kernel` 直接出正确 boot.img
- 遗留：mailbox 中断路径硬件根因未深挖（轮询已绕开）；/dev/rpmsg0 未开；
  清理版固件待做
