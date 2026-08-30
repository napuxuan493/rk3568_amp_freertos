#!/usr/bin/env python3
"""uartctl.py —— 板子串口交互助手（发送命令 / 读 N 秒）

和 bootcap.py 配合的日常工具：不进 picocom，直接命令行发命令、读输出。

用法：
    python3 uartctl.py send "echo hello" "cat /proc/uptime"   # 发多条命令并读响应
    python3 uartctl.py read 5                                  # 只读 5 秒
    python3 uartctl.py send "" "md5sum /tmp/amp.img"           # 先回车再发命令

注意：/dev/ttyUSB0 上不能有 picocom 占用；波特率 1500000。
"""

import sys
import time
import serial

DEFAULT_PORT = '/dev/ttyUSB0'
DEFAULT_BAUD = 1500000


def drain(ser, seconds):
    end = time.time() + seconds
    out = b''
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            out += chunk
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    ser = serial.Serial(DEFAULT_PORT, DEFAULT_BAUD, timeout=0.15)
    mode = sys.argv[1]

    if mode == 'send':
        for line in sys.argv[2:]:
            ser.write((line + '\n').encode())
            time.sleep(0.4)
        sys.stdout.buffer.write(drain(ser, 1.0))
    elif mode == 'read':
        seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
        sys.stdout.buffer.write(drain(ser, seconds))
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
