OUTPUT_ARCH(aarch64)
ENTRY(_start)

/* ★ 链接地址 = dts amp-cpus 的 entry（0x7000000），三者必须一致：
 *   链接脚本 . / 固件加载地址 / amp-cpus entry */
SECTIONS
{
    . = 0x7000000;

    .text : {
        KEEP(*(.text.start))        /* _start 必须在最前 */
        . = ALIGN(2048);
        KEEP(*(.text.vectors))      /* 异常向量表（2KB 对齐） */
        *(.text*)
    }

    .rodata : { *(.rodata*) }

    .data : { *(.data*) }

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    _stack_bottom = .;
    . += 0x8000;                    /* core1 栈：32KB */
    _stack_top = .;

    _end = .;
}
