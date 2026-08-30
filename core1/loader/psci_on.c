/* psci_on.c — 直接用标准 PSCI_CPU_ON 启动 core3 到 0x7000000
 * 目的：绕过 Rockchip AMP SMC（RK_SIP_AMP_CFG）的地址处理问题
 * （OP-TEE 切 core3 时可能用了存的旧地址而不是我们传的 entry）
 *
 * 用法：insmod psci_on.ko → 内核日志会打 CPU_ON 返回值
 *       → 然后读探针/看串口
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/arm-smccc.h>

#define PSCI_0_2_FN64_CPU_ON  0xC4000003UL
#define CORE3_ID              3UL
static unsigned long entry = 0x7000000UL;
module_param(entry, ulong, 0644);

static int __init psci_on_init(void)
{
    struct arm_smccc_res res;

    pr_info("psci_on: PSCI_CPU_ON(%lu, 0x%lx) ...\n", CORE3_ID, entry);
    arm_smccc_smc(PSCI_0_2_FN64_CPU_ON, CORE3_ID, entry, 0,
                  0, 0, 0, 0, &res);
    pr_info("psci_on: ret=%lld (0=成功, -4=already on, -5=on pending)\n",
            (long long)res.a0);
    return 0;
}

module_init(psci_on_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PSCI direct CPU_ON for AMP core3 (M2.1 debug)");
