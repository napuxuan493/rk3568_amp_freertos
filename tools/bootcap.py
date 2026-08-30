#!/usr/bin/env python3
"""bootcap.py —— 抓取 RK3568 AMP 板子启动日志（发 reboot + 串口读 N 秒）

用途：M2.2 调试中定位 u-boot 的 AMP 加载 / core1 输出 / Linux 启动问题。
把整段启动输出（u-boot → core1 DIAG → Linux shell）存成文件，便于 grep 分析。

用法：
    python3 bootcap.py                       # 默认：40 秒，输出 /tmp/bootlog.txt
    python3 bootcap.py -s 60                 # 抓 60 秒
    python3 bootcap.py -o /tmp/bl.txt -s 30  # 30 秒，输出到指定文件
    python3 bootcap.py --cmd 'echo x'        # 不重启，发别的命令

注意：/dev/ttyUSB0 上不能有 picocom 占用；板子在 UART2（调试口 1500000）。
"""

import argparse
import time
import serial

DEFAULT_PORT = '/dev/ttyUSB0'
DEFAULT_BAUD = 1500000

def main():
    ap = argparse.ArgumentParser(description='抓取板子启动日志')
    ap.add_argument('-p', '--port', default=DEFAULT_PORT, help=f'串口（默认 {DEFAULT_PORT}）')
    ap.add_argument('-b', '--baud', type=int, default=DEFAULT_BAUD, help=f'波特率（默认 {DEFAULT_BAUD}）')
    ap.add_argument('-s', '--seconds', type=float, default=40.0, help='抓取秒数（默认 40）')
    ap.add_argument('-o', '--output', default='/tmp/bootlog.txt', help='输出文件（默认 /tmp/bootlog.txt）')
    ap.add_argument('--cmd', default='reboot', help='打开串口后发送的命令（默认 reboot）')
    args = ap.parse_args()

    print(f"[bootcap] 打开 {args.port} @ {args.baud} ...")
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    ser.reset_input_buffer()

    ser.write((args.cmd + '\n').encode())
    print(f"[bootcap] 已发送: {args.cmd!r}")

    t0 = time.time()
    out = b''
    while time.time() - t0 < args.seconds:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
            # 简单进度（每 2 秒一次）
            if int(time.time() - t0) % 2 == 0 and int((time.time() - t0) * 10) % 20 == 0:
                print(f"  ...已抓 {len(out)} 字节 / {time.time()-t0:.0f}s")

    with open(args.output, 'wb') as f:
        f.write(out)
    print(f"[bootcap] 完成：{len(out)} 字节 -> {args.output}")

if __name__ == '__main__':
    main()
