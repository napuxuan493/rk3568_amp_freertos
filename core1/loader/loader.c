/* M2.1 固件加载器（Linux 用户态）：把 core1.bin 拷到物理 0x7000000
 *
 * 前置条件：
 *   1. dts 已加 reserved-memory（no-map）并重编烧录 —— 0x7000000 变成
 *      Reserved 区后 /dev/mem 才能访问（STRICT_DEVMEM 下 RAM 区被禁，
 *      no-map 保留区不算 RAM，允许访问）
 *   2. 板子上有 /dev/mem 访问权限（root）
 *
 * 用法：
 *   aarch64-linux-gnu-gcc loader.c -o loader   （或板子上 gcc）
 *   ./loader core1.bin
 *   echo on 3 > /sys/rk_amp/boot_cpu
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define CORE1_LOAD_ADDR 0x7000000UL

int main(int argc, char *argv[])
{
    struct stat st;
    unsigned char *buf, *map;
    size_t size;
    FILE *f;
    int fd;
    long i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <core1.bin>\n", argv[0]);
        return 1;
    }

    if (stat(argv[1], &st) != 0 || st.st_size <= 0) {
        perror("stat");
        return 1;
    }
    size = (size_t)st.st_size;

    buf = malloc(size);
    if (!buf)
        return 1;
    f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    if (fread(buf, 1, size, f) != size) { perror("fread"); return 1; }
    fclose(f);

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CORE1_LOAD_ADDR);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }

    memcpy(map, buf, size);   /* 固件就位（O_SYNC 写穿） */

    printf("loaded %zu bytes to 0x%lx, first bytes: ", size, (unsigned long)CORE1_LOAD_ADDR);
    for (i = 0; i < 8 && i < (long)size; i++)
        printf("%02x ", buf[i]);
    printf("\n");

    munmap(map, size);
    close(fd);
    free(buf);
    return 0;
}
