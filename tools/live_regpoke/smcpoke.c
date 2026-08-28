/*
 * smcpoke: issue an MTK SIP SMC from kernel EL1 (NS) and dump the return regs.
 * Used to READ the MCSI (Mediatek Cache Snoop Interconnect) register file through
 * ATF, because mcusys/mcucfg are secure-write/read-protected (a raw ioremap readl
 * returns 0). Sweep MCSI offsets with the worker core hotplug OFFLINE vs ONLINE;
 * the bit(s) that toggle are the snoop-admission control LK must reproduce.
 *
 *   insmod smcpoke.ko fid=0x8200028A a0=0x0 a1=0 sweep=0x40   (MCSI_A_READ sweep 0..0x40)
 *   fid default = MCSI_A_READ. sweep>0: loop a0 from a0..a0+sweep step 4.
 * ret/regs go to dmesg (grep SMCPOKE). Module init returns -EINVAL so it never stays.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/arm-smccc.h>

static unsigned long fid = 0x8200028AUL; /* MTK_SIP_KERNEL_MCSI_A_READ */
static unsigned long a0 = 0x0UL;
static unsigned long a1 = 0x0UL;
static unsigned long a2 = 0x0UL;
static int sweep = 0;   /* if >0, loop a0..a0+sweep step 4 */
static char *tag = "";
module_param(fid, ulong, 0);
module_param(a0, ulong, 0);
module_param(a1, ulong, 0);
module_param(a2, ulong, 0);
module_param(sweep, int, 0);
module_param(tag, charp, 0);

static void one(unsigned long arg0)
{
	struct arm_smccc_res res;
	arm_smccc_smc(fid, arg0, a1, a2, 0, 0, 0, 0, &res);
	pr_err("SMCPOKE fid=%08lx a0=%08lx -> a0=%08lx a1=%08lx a2=%08lx a3=%08lx\n",
	       fid, arg0, res.a0, res.a1, res.a2, res.a3);
}

static int __init sp_init(void)
{
	pr_err("SMCPOKE begin fid=%08lx a0=%08lx sweep=%d tag=%s ---\n", fid, a0, sweep, tag);
	if (sweep > 0) {
		unsigned long o;
		for (o = a0; o <= a0 + (unsigned long)sweep; o += 4)
			one(o);
	} else {
		one(a0);
	}
	pr_err("SMCPOKE done tag=%s\n", tag);
	return -EINVAL;
}
static void __exit sp_exit(void) {}
module_init(sp_init);
module_exit(sp_exit);
MODULE_LICENSE("GPL");
