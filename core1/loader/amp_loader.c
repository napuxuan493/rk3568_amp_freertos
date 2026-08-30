/* amp_loader.c — 内核模块版 core1 固件加载器
 *
 * 为什么不用 /dev/mem：STRICT_DEVMEM 下 mmap 被 devmem_is_allowed() 拒绝
 * （内核认为 0x7000000 仍是 RAM）。内核模块有 ioremap 权限，绕开限制。
 *
 * 用法（板子上）：
 *   insmod amp_loader.ko
 *   cat core1.bin > /dev/amp_loader
 *   echo on 3 > /sys/rk_amp/boot_cpu
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/io.h>

static unsigned long amp_phys = 0x7000000UL;
module_param(amp_phys, ulong, 0644);
#define AMP_FW_PHYS  amp_phys
#define AMP_FW_SIZE  (1024 * 1024)   /* 1MB：固件保留区大小 */

static void __iomem *fw_base;

static ssize_t amp_loader_write(struct file *f, const char __user *buf,
                                size_t count, loff_t *ppos)
{
    if (*ppos + count > AMP_FW_SIZE)
        return -EFBIG;

    if (copy_from_user((void __force *)fw_base + *ppos, buf, count))
        return -EFAULT;

    *ppos += count;
    pr_info("amp_loader: wrote %zu bytes @0x%llx (total %lld)\n",
            count, AMP_FW_PHYS + (unsigned long long)*ppos - count,
            (long long)*ppos);
    return count;
}

static ssize_t amp_loader_read(struct file *f, char __user *buf,
                               size_t count, loff_t *ppos)
{
    if (*ppos >= AMP_FW_SIZE)
        return 0;
    if (*ppos + count > AMP_FW_SIZE)
        count = AMP_FW_SIZE - *ppos;

    if (copy_to_user(buf, (const void __force *)fw_base + *ppos, count))
        return -EFAULT;

    *ppos += count;
    return count;
}

static const struct file_operations amp_loader_fops = {
    .owner  = THIS_MODULE,
    .llseek = generic_file_llseek,  /* 支持 dd skip=（之前缺这个，dd 无法 seek） */
    .read   = amp_loader_read,
    .write  = amp_loader_write,
};

static struct miscdevice amp_loader_dev = {
    .minor  = MISC_DYNAMIC_MINOR,
    .name   = "amp_loader",
    .fops   = &amp_loader_fops,
};

static int __init amp_loader_init(void)
{
    fw_base = ioremap(AMP_FW_PHYS, AMP_FW_SIZE);
    if (!fw_base) {
        pr_err("amp_loader: ioremap(0x%lx, %u) failed — 确认该地址已在 dts reserved-memory 中\n",
               (unsigned long)AMP_FW_PHYS, AMP_FW_SIZE);
        return -ENOMEM;
    }
    pr_info("amp_loader: ioremap 0x%lx ok\n", (unsigned long)AMP_FW_PHYS);

    return misc_register(&amp_loader_dev);
}

static void __exit amp_loader_exit(void)
{
    misc_deregister(&amp_loader_dev);
    iounmap(fw_base);
}

module_init(amp_loader_init);
module_exit(amp_loader_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMP core1 firmware loader (M2.1)");
