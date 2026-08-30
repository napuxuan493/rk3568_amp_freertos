# parameter.txt 修改流程 SOP（RK3568 KickPi K1）

> 适用：改分区大小 / 加分区 / 固定 GUID / 重排分区后重烧。
> 红线：**只用 upgrade_tool `di -p` 烧分区表，绝不用 u-boot 的 `gpt write`**
> （它把第一分区放 LBA 34，而 Rockchip 要求 uboot 在 LBA 0x4000，一写全错位）。
> 工具：`tools/linux/Linux_Upgrade_Tool/Linux_Upgrade_Tool/upgrade_tool`

## 一、当前分区表（2026-08 基线）

文件：`rockdev/parameter.txt`

```
FIRMWARE_VER: 1.0
MACHINE_MODEL: RK3568
MACHINE_ID: 007
MANUFACTURER: RK3568
MAGIC: 0x5041524B
ATAG: 0x00200800
MACHINE: 0xffffffff
CHECK_MASK: 0x80
PWR_HLD: 0,0,A,0,1
TYPE: GPT
GROW_ALIGN: 0
CMDLINE: mtdparts=:0x00002000@0x00004000(uboot),0x00002000@0x00006000(misc),0x00020000@0x00008000(boot),0x00020000@0x00028000(boot_b),0x00c00000@0x00058000(system),0x00040000@0x00c58000(oem),0x00010000@0x00c98000(amp),-@0x00ca8000(userdata:grow)
uuid:system=614e0000-0000-4000-8000-000000000000
uuid:boot=7A3F0000-0000-446A-8000-702F00006273
```

**mtdparts 语法**：`mtdparts=:<size>@<offset>(<name>),<size>@<offset>(<name>),...`
**单位是扇区（512 字节）**，十六进制。`-` 表示占满剩余空间（grow）。

| 分区 | size(扇区) | offset(扇区) | 实际大小 | 实际偏移 |
|---|---|---|---|---|
| uboot | 0x2000 | 0x4000 | 4MB | 8MB（**必须在这**，loader 间隙） |
| misc | 0x2000 | 0x6000 | 4MB | 12MB |
| boot | 0x20000 | 0x8000 | 64MB | 16MB |
| boot_b | 0x20000 | 0x28000 | 64MB | 80MB |
| system | 0xc00000 | 0x58000 | 6GB | 176MB |
| oem | 0x40000 | 0xc58000 | 128MB | 6314MB |
| amp | 0x10000 | 0xc98000 | 32MB | 6466MB |
| userdata | -（grow） | 0xca8000 | 剩余 | 6500MB |

换算：`字节 = 扇区 × 512`；下一分区 offset = 上一分区 offset + size
（验证：0x58000+0xc00000=0xc58000 ✓，0xc58000+0x40000=0xc98000 ✓）

## 二、三条铁律

1. **uboot 分区永远在 0x4000 扇区（8MB）**，别动它和它前面的布局
2. **system 分区的 GUID 必须 614e0000 前缀**（内核 `root=PARTUUID=614e0000-0000` 前缀匹配）
   ——靠 `uuid:system=` 行固定；**没有 uuid 行的分区，di -p 会随机生成 GUID**
3. **分区对齐**：保持 4MB 对齐（0x2000 扇区整数倍）最稳妥

## 三、修改场景

### 场景 A：只改分区大小（offset 不变）——数据保留
例：system 从 6GB 改 8GB：`0x00c00000@0x00058000(system)` → `0x01000000@0x00058000(system)`
- 只动 size，**不动 offset** → 已有数据完整保留
- 后续分区 offset 若因 size 变化而移动，那些分区会错位 → 需要同步改 + 重烧数据

### 场景 B：末尾加新分区——安全
例：加 64MB 的 log 分区：
```
...,-@0x00ca8000(userdata:grow)
```
改为：
```
...,0x00020000@0x00ca8000(log),-@0x00cc0000(userdata:grow)
```
（把 userdata 的 offset 从 0xca8000 移到 0xcc0000，新增 log 占 0xca8000-0xcc0000）
- 注意：userdata 的 offset 变了，**userdata 现有数据会错位**（userdata 通常是可清空的，无所谓）
- 若想彻底不碰 userdata，只能把新分区插在 grow 之前且 userdata 保持 grow 末尾……做不到，
  末尾 grow 分区前插分区必然移动它——userdata 可接受清空即可

### 场景 C：固定某个分区的 GUID
```
uuid:oem=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```
（格式照抄 `uuid:system=`，值用 `uuidgen` 生成或按需写 614e0000 前缀等）

### 场景 D：重排/删除/在中间插分区——数据全部错位，必须重烧数据
- 中间任何分区的 offset 变化 → 该分区及之后的分区数据错位
- 需要重烧：parameter + 相应分区镜像（boot/uboot/system/oem/amp...）+ 可能 rootfs
- 强烈不建议，除非整盘重来

## 四、烧录 + 验证 SOP

```
① 备份当前分区表（可选，板子上执行）：
   sgdisk -b /tmp/gpt_backup.bin /dev/mmcblk0

② 编辑 rockdev/parameter.txt（按上面场景改）

③ 本地语法校验（可选，不碰板子）：
   upgrade_tool GPT rockdev/parameter.txt /tmp/check.gpt
   # 注意：本地 GPT 命令会【忽略 uuid 行】——GUID 是否正确只能烧录后查

④ 进 loader 模式：断电 → 按住 RECOVERY → 上电
   lsusb 应出现 2207:350a

⑤ 烧分区表：
   upgrade_tool di -p rockdev/parameter.txt
   # di -p 只写 GPT 头，分区数据不动（offset 未变时）

⑥ 重启验证（板子 shell）：
   lsblk -o NAME,SIZE,PARTUUID,MOUNTPOINT        # 分区表和 GUID
   blkid /dev/mmcblk0p5                          # system 应 614e0000-...
   cat /proc/cmdline                             # root=PARTUUID=614e0000-0000 应匹配
   cat /sys/devices/system/cpu/online            # 应 0-2（AMP 环境完好）

⑦ 若某分区数据错位/损坏，重烧对应镜像：
   upgrade_tool di -b kernel/boot.img            # boot
   upgrade_tool di -uboot rockdev/uboot.img      # uboot
   upgrade_tool di -s rockdev/rootfs.img         # system（会覆盖系统）
```

## 五、以后修改了"其他东西"的烧录速查

| 改了什么 | 烧法 | 备注 |
|---|---|---|
| parameter（分区表） | `di -p`（上文 SOP） | loader 模式 |
| u-boot 源码 → uboot.img | 板内 `wget` + `dd of=/dev/mmcblk0p1` 或 `di -uboot` | 重烧后 env 一般保留（maxcpus=3 等） |
| 内核/dts → boot.img | 板内 `wget` + `dd of=/dev/mmcblk0p3` 或 `di -b` | **OTA 包里的 boot.img 必须带 amp dts**（cpu3 disabled + core1-fw 保留区） |
| core1 固件 → amp.img | 板内 `wget` + `dd of=/dev/mmcblk0p7` + 重启 | 无需进 loader |
| rootfs | `di -s rockdev/rootfs.img` | 会覆盖 system |

**板内烧录模板**（免 loader，WiFi 同网段 10.164.40.210 起 HTTP）：
```bash
wget -q http://10.164.40.210:8000/amp.img -O /tmp/amp.img
dd if=/tmp/amp.img of=/dev/mmcblk0p7 bs=512 conv=fsync && sync
# 完成后 reboot
```

## 六、常见坑

1. **本地 `GPT` 命令忽略 uuid 行**——别拿它验证 GUID，必须烧录后 `blkid` 确认
2. **`di -p` 后 system GUID 变了 → 起不来**：症状 "Waiting for root device PARTUUID=614e0000-0000"，检查 uuid:system 行是否还在
3. **动了 uboot 分区位置 → 直接变砖**（loader 都起不来），只能 maskrom 重烧 loader
4. **分区大小写死记 512B 扇区**：0x20000 扇区 = 64MB（不是 2MB），算错会覆盖别的分区
5. **amp 分区数据**：di -p 不动它；重烧 parameter 后 amp 内容还在
6. **boot/boot_b 双槽**：你的切换脚本用 sgdisk 改名 p3/p4；重烧 parameter 会把名字恢复为 boot/boot_b（内容不变）
