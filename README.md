# RK3568 AMP 异构双系统项目（旗舰项目）

> 目标：Core0 跑 Linux 5.10（SDK 内核），Core1 跑自己移植的 FreeRTOS 裸核，
> mailbox + 共享内存 vring（RPMsg）双向通信——覆盖拓竹岗位全部核心考点，
> 衔接大疆异构计算方向。

## 目录结构

```
rk3568_amp/
├── README.md            # 本文件
├── journal/             # 每日日志（8_17 起，用模板）
├── docs/                # 机制解析、架构文档
│   └── week1_reading_guide.md   # 第一周 SDK 驱动导读
├── labs/                # 学习实验（跑在 PC 上，不需要板子）
│   └── 01_freertos_posix/       # FreeRTOS POSIX 模拟器实验（已可运行）
├── core0/               # 后续：Linux 侧（rpmsg 应用、dts 片段）
└── core1/               # 后续：FreeRTOS 裸核工程
    └── freertos-kernel/ # FreeRTOS V10.6.2 LTS 源码（已克隆）
```

## 里程碑

| 里程碑 | 内容 | 状态 |
|---|---|---|
| 实验 01 | FreeRTOS 机制速通（POSIX 模拟器，6 实验） | ✅ 可运行 |
| 周 1 阅读 | 三个驱动 + dtsi 导读（week1_reading_guide.md） | ⏳ 待读 |
| M1 | FreeRTOS 概念验收（答出 README 里的 5 个源码问题） | ⏳ |
| M2 | FreeRTOS 移植到 RK3568 core1（startup/GIC/tick/串口/LED） | ⏳ |
| M3 | RPMsg 双向通信（Linux ↔ core1 vring） | ⏳ |
| M4 | 实时任务 + 延迟测量（对比 Linux 原生） | ⏳ |
| M5 | 中间件（日志/命令/状态上报）+ core1 固件升级 | ⏳ |
| M6 | 测试自动化 + 架构文档 + 演示 | ⏳ |

## 关键环境事实

- SDK：`/home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux`（kernel 5.10.160）
- 内核已开启：`CONFIG_RPMSG_ROCKCHIP=y` `CONFIG_RPMSG_VIRTIO=y` `CONFIG_ROCKCHIP_AMP=y`
- 官方支持：`rockchip_amp.c` 里有 `compatible = "rockchip,rk3568-amp"`
- dts 模板：`rk3588-amp.dtsi`（rk3568 需仿写，注意内存布局差异）
- 裸机工具链：SDK `prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu`（已验证可编裸机 ELF）
- 板子：KickPi K1（RK3568），另有 M.2 PCIe3x2 / mini-PCIe / SATA0 / 双 GMAC（外设轨道）

## 外设深化轨道（并行，占 20~25% 时间）

- ETH：第 2~3 周（GMAC+PHY+MDIO+NAPI）
- PCIe/NVMe：第 5~7 周（fio 测速 → 读 pcie-dw-rockchip.c → 画枚举/DMA 数据流）
- CAN：已覆盖，补读 `rockchip_canfd.c`
