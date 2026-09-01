# core1 固件：改代码 → 编译 → 烧录 SOP

> 适用范围：改 `core1/` 里的任意代码（main.c 实验、gic.c、timer.c、FreeRTOS 配置等）。
> 网络现状：PC=192.168.1.110，板子=192.168.1.115（同网段；IP 会变，先 `ip addr` 确认）。
> 板子当前跑着 M2.2 的 FreeRTOS（2 任务 + tick）。

## 一、编译 + 打包（PC 上）

```bash
cd /home/na/rk3568_amp/core1

# ① 编译（只改了 main.c 等 .c/.S 文件 → 直接 make）
make

# ⚠️ 改了 FreeRTOSConfig.h / port*.c / 头文件 → 必须 clean（Makefile 无头文件依赖）：
make clean && make

# ② 检查产物
readelf -h core1.elf | grep Entry        # 必须是 0x7000000
ls -l core1.bin                           # 记录新大小

# ③ 打包成 amp.img（data-size 必须 ≥ core1.bin 大小；变大了要改 amp.its 的 data-size）
MKIMAGE=/home/na/rk3586/linux/rk356x-linux-20251212/rk356x-linux/u-boot/tools/mkimage
$MKIMAGE -f amp.its amp.img
md5sum amp.img

# ④ 放进传输目录
cp amp.img /tmp/xfer/
```

## 二、起传输服务器（PC 上，已在跑则跳过）

```bash
# 目录可能被清，重新放一次：
mkdir -p /tmp/xfer && cp amp.img /tmp/xfer/
cd /tmp/xfer && python3 -m http.server 8000 &
# 验证（用 PC 当前 IP，不是旧 IP！）：
curl --noproxy '*' -s http://10.164.40.210:8000/amp.img -o /dev/null -w "%{http_code}\n"   # 应 200
```10.164.40.210

## 三、板内下载 + 烧录（PC 串口操作）

```bash
# ① 串口可能被 USB 重插重置成 9600，先设波特率：
stty -F /dev/ttyUSB0 1500000 raw -echo

# ② 通过串口 shell 让板子执行（用 read_console 助手或直接敲）：
#    wget -q -T 8 http://10.164.40.19:8000/amp.img -O /tmp/amp.img
#    md5sum /tmp/amp.img          ← 必须和 PC 端 md5 一致
#    dd if=/tmp/amp.img of=/dev/mmcblk0p7 bs=512 conv=fsync && sync
#    reboot
```

助手模板（PC 上执行，自动发命令 + 收输出）：

```bash
stty -F /dev/ttyUSB0 1500000 raw -echo
read_console() { timeout 20 cat /dev/ttyUSB0 > /tmp/c.txt & local pid=$!; sleep 1
  printf '\n%s\n' "$1" > /dev/ttyUSB0; sleep 18; kill $pid 2>/dev/null; cat /tmp/c.txt; }
read_console 'wget -q -T 8 http://10.164.40.19:8000/amp.img -O /tmp/amp.img && md5sum /tmp/amp.img && dd if=/tmp/amp.img of=/dev/mmcblk0p7 bs=512 conv=fsync 2>&1 | tail -1 && sync && echo FLASH_OK'

 wget -q -T 30 http://10.164.40.210:8000/amp.img -O /tmp/amp.img; dd if=/tmp/amp.img of=/dev/mmcblk0p7 bs=512 conv=fsync; sync; reboot

```

## 四、重启验证

```bash
# 抓启动 + 任务输出（~30 秒）
(timeout 30 cat /dev/ttyUSB0 > /tmp/boot.txt &); sleep 2
printf 'reboot\n' > /dev/ttyUSB0; sleep 28
grep -aE "=== core1|\[irq\]|\[t1\]|\[t2\]|EXC|FATAL" /tmp/boot.txt | head -20
```

预期：`=== core1 FreeRTOS` → `[t1] hello` / `[t2] tick = N` 交替出现且 tick 递增。

## 五、常见坑

1. **PC IP 会变**：先 `ip addr` 看 wlp0s20f3 的 inet，别用记下来的旧 IP
2. **改了头文件忘了 clean**：FreeRTOSConfig.h 改动不触发重编 → `make clean && make`
3. **amp.its 的 data-size**：core1.bin 变大超过 data-size，u-boot 会解析失败 → 改大
4. **串口波特率被重置**：USB 重插后默认 9600，先 `stty ... 1500000`
5. **服务器端口被占**：`ss -tlnp | grep 8000` 看是否已有 python3 在跑（有就不用再起）
6. **wget 走代理**：板子上 wget 直接连局域网 IP，一般无代理问题；PC 上 curl 要 `--noproxy '*'`
