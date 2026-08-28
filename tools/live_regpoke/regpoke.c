#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/moduleparam.h>

static unsigned long base = 0x10006200UL;
static int n = 12;
module_param(base, ulong, 0);
module_param(n, int, 0);

static int __init rp_init(void)
{
	int i;
	pr_err("REGPOKE base=%08lx n=%d ---\n", base, n);
	for (i = 0; i < n; i++) {
		unsigned long a = base + (i * 4);
		void __iomem *v = ioremap(a, 4);
		u32 val = v ? readl(v) : 0xDEADDEADU;
		pr_err("REGPOKE %08lx = %08x\n", a, val);
		if (v)
			iounmap(v);
	}
	return -EINVAL;
}
static void __exit rp_exit(void) {}
module_init(rp_init);
module_exit(rp_exit);
MODULE_LICENSE("GPL");
