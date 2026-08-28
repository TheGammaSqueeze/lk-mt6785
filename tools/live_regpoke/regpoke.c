#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/moduleparam.h>

static unsigned long base = 0x10006200UL;
static int n = 12;
static int nz = 0;   /* nz=1: print only nonzero registers (sweep large regions) */
static char *tag = "";
module_param(base, ulong, 0);
module_param(n, int, 0);
module_param(nz, int, 0);
module_param(tag, charp, 0);

static int __init rp_init(void)
{
	int i;
	void __iomem *v = ioremap(base, (unsigned long)n * 4);
	pr_err("REGPOKE base=%08lx n=%d nz=%d tag=%s ---\n", base, n, nz, tag);
	if (!v) {
		pr_err("REGPOKE ioremap FAILED\n");
		return -EINVAL;
	}
	for (i = 0; i < n; i++) {
		u32 val = readl(v + (i * 4));
		if (nz && val == 0)
			continue;
		pr_err("REGPOKE %08lx = %08x\n", base + (i * 4), val);
	}
	pr_err("REGPOKE done tag=%s\n", tag);
	iounmap(v);
	return -EINVAL;
}
static void __exit rp_exit(void) {}
module_init(rp_init);
module_exit(rp_exit);
MODULE_LICENSE("GPL");
