# core0 — Linux(RK3568 大核侧)

本目录是 **AMP 方案中 core0(Linux)侧的工程说明**。core0 自身不写代码——
Linux 内核是 Rockchip 官方 SDK 提供的,本工程只在其上做了少量修改。

## 目录结构

```
core0/
├── README.md                  ← 本文件
└── sdk_modifications/         ← 本工程在 SDK 里修改/新增的文件留档(共 9 个,160K)
```

## SDK 本体在哪

原始 SDK(含本工程全部修改):

```
/home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux/
```

```bash
# 编译内核 / boot.img
cd /home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux && ./build.sh kernel
# 产物在 output/kernel/ 下(芯片 boot.its + mkimage -E -p 0x800)
```

## sdk_modifications/ — 本工程在 SDK 里改过的文件(重要!)

换环境 / 重拷 SDK / 原始 SDK 丢失时,把这些文件按相对路径放回 SDK 即可恢复全部修改。
判定方法:SDK 原生文件 mtime 统一为 `2025-01-01`,凡 mtime 落在项目期间(2026-08)的就是被本工程动过的。

### kernel(Linux 侧,4 个)

| 留档文件(相对 core0/sdk_modifications/) | 放回 SDK 的位置 | 修改内容 |
| --- | --- | --- |
| `kernel/arch/arm64/boot/dts/rockchip/rk3568-amp.dtsi` | 同路径 | AMP 方案节点(amp/uart4/rpmsg/mailbox),被 k1.dtsi include |
| `kernel/arch/arm64/boot/dts/rockchip/rk3568-kickpi-k1.dtsi` | 同路径 | 主板 dts:新增 `#include "rk3568-amp.dtsi"` + `my_test_key` GPIO 中断测试节点 |
| `kernel/arch/arm64/boot/dts/rockchip/rk3568-kickpi-extend-40pin.dtsi` | 同路径 | 禁用 uart4(让给 core1 做控制台) |
| `kernel/drivers/mailbox/rockchip-mailbox.c` | 同路径 | 移除损坏行(编译不过的 `/1` 行) |
| `kernel/arch/arm64/configs/rockchip_linux_defconfig` | 同路径 | L787 增加 `CONFIG_RPMSG_ROCKCHIP_TEST=y`(rpmsg 回环测试驱动) |
| `kernel/drivers/soc/rockchip/rockchip_amp.c` | 同路径 | Linux 侧 AMP 驱动(8/21 动过,amp-cpus 相关) |

### u-boot(bootloader 侧,3 个)

| 留档文件(相对 core0/sdk_modifications/) | 放回 SDK 的位置 | 修改内容 |
| --- | --- | --- |
| `u-boot/drivers/cpu/rockchip_amp.c` | 同路径 | **M2.1 核心**:AMP 加载驱动(读 amp 分区 → 0x7000000 → psci_cpu_on) |
| `u-boot/fit/u-boot.its` | 同路径 | u-boot FIT 打包配置(8/21 动过) |
| `u-boot/my_env.sh` | 同路径 | **本工程新增**的环境脚本(交叉编译路径)。注意内含本机绝对路径,换环境需改 |

### 备注

- `u-boot/make.sh`、`kernel/.../rk3568-nvr-demo-v10.dtsi` mtime 异常(8/2、8/4)但内容无工程痕迹,未留档。
- 除上述源码外,SDK 内还有大量编译产物(kernel/include/config/、u-boot/include/config/、output/ 等),
  均为构建生成,不入库。
