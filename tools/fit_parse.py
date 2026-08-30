#!/usr/bin/env python3
"""fit_parse.py —— 解析 FIT 镜像（FDT 格式）的字符串表与结构树

背景：u-boot 的 fit_get_totalsize() 按属性"名字"查 FIT 根节点的 totalsize 属性
（image.h 的 FIT_TOTALSIZE_PROP）。属性名存在 FDT 文件末尾的字符串块
（off_dt_strings），M2.2 调试中发现：Ubuntu mkimage 不写 totalsize 属性名，
SDK u-boot mkimage 会写。用这个脚本一跑就知道打包工具对不对。

FDT 布局（大端）：
    头部 40 字节：magic(0) totalsize(4) off_dt_struct(8) off_dt_strings(12)
                  off_mem_rsvmap(16) ... size_dt_strings(32) size_dt_struct(36)
    结构块（off_dt_struct 起）：一串 token
    字符串块（off_dt_strings 起）：一串 '\0' 结尾的字符串

用法：
    python3 fit_parse.py amp.img            # 打印结构树 + 字符串表
    python3 fit_parse.py amp.img --strings  # 只看字符串表（查有没有 totalsize）
"""

import struct
import sys

FDT_MAGIC = 0xD00DFEED

# FDT 结构块 token
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


def parse_fit(filename):
    data = open(filename, 'rb').read()
    magic, totalsize, off_struct, off_strings, off_rsv = struct.unpack('>IIIII', data[:20])
    size_strings = struct.unpack('>I', data[32:36])[0]

    if magic != FDT_MAGIC:
        raise ValueError(f'{filename}: 不是 FIT/FDT（magic={magic:#x}，期望 0xd00dfeed）')

    strings = data[off_strings:off_strings + size_strings]

    def name_at(offset):
        """nameoff 是字符串块里的【字节偏移】，不是下标！取到下一个 '\0' 为止"""
        end = strings.index(b'\x00', offset)
        return strings[offset:end].decode('utf-8', 'replace')

    print(f'文件: {filename}')
    print(f'  totalsize     = {totalsize} ({totalsize:#x})  实际文件 {len(data)} 字节')
    print(f'  off_dt_struct = {off_struct:#x}')
    print(f'  off_dt_strings= {off_strings:#x}')
    print(f'  size_strings  = {size_strings}')
    print(f'  字符串块在 4KB 窗口内? {off_strings < 4096}   (u-boot 旧版只读 4KB 头)')
    print()

    # ---- 字符串表 ----
    print('== 字符串表（属性名全集）==')
    tbl = set()
    i = 0
    while i < size_strings:
        end = strings.index(b'\x00', i)
        name = strings[i:end].decode('utf-8', 'replace')
        if name:
            tbl.add(name)
            print(f'  [{i:#06x}] {name}')
        i = end + 1
    print(f'  共 {len(tbl)} 个属性名')
    print(f'  {"totalsize" in tbl}: {"✅ 有 totalsize（SDK mkimage）" if "totalsize" in tbl else "❌ 无 totalsize（Ubuntu mkimage，u-boot 会报 No totalsize）"}')
    print()

    # ---- 结构树（递归打印节点/属性）----
    print('== 结构树 ==')

    def walk(offset, depth):
        while offset < off_strings:
            tag = struct.unpack('>I', data[offset:offset + 4])[0]
            offset += 4
            if tag == FDT_BEGIN_NODE:
                end = data.index(b'\x00', offset)
                name = data[offset:end].decode('utf-8', 'replace')
                offset = (end + 1 + 3) & ~3  # 名字 '\0' 结尾，4 字节对齐
                print('  ' * depth + f'NODE {name}')
                offset = walk(offset, depth + 1)
            elif tag == FDT_END_NODE:
                return offset
            elif tag == FDT_PROP:
                length, nameoff = struct.unpack('>II', data[offset:offset + 8])
                offset += 8
                val = data[offset:offset + length]
                offset = (offset + length + 3) & ~3  # 值 4 字节对齐
                # 数值型属性显示数值，字符串型显示字符串，大数据块只显示预览
                name = name_at(nameoff)
                if name == 'data':
                    show = f'<{len(val)} 字节固件数据，前16字节> {val[:16].hex()}'
                elif len(val) == 4:
                    show = f'{struct.unpack(">I", val)[0]} (0x{struct.unpack(">I", val)[0]:x})'
                elif val.endswith(b'\x00'):
                    show = val.rstrip(b'\x00').decode('utf-8', 'replace')
                else:
                    show = val.hex()
                print('  ' * depth + f'PROP {name} = {show}')
            elif tag == FDT_NOP:
                pass
            elif tag == FDT_END:
                return offset
            else:
                print(f'  [未知 token {tag:#x} @ {offset:#x}]')
                return offset
        return offset

    walk(off_struct, 0)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    parse_fit(sys.argv[1])
