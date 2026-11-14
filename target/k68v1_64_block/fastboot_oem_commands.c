/*
 * Fastboot oem commands of the Konkr Pocket Block target.
 *
 * BOOT_TRACE builds (bring-up without a serial console, platform/common/boot_trace.c):
 *   oem tryboot[:args]  boot the system once, with the watchdog armed (and the kernel arguments
 *                  given, if any); any hang comes back to fastboot
 *   oem trace      the LK log of the previous boot
 *   oem lastkmsg   the kernel console the previous boot left in ramoops
 *   oem pstore     every ramoops zone in pstore (panic and oops records, console)
 *   oem savedtrace the log the last boot attempt copied to misc at its milestones
 *   oem savenow    copy this boot's log to misc now, and show the reset reason it saw
 */
#include <debug.h>
#include <reg.h>
#include <target.h>
#include <platform/mt_reg_base.h>

void fastboot_register(const char *prefix, void (*handle)(const char *arg, void *data, unsigned sz),
		       unsigned allowed_when_security_on, unsigned forbidden_when_lock_on);
void fastboot_okay(const char *info);
void fastboot_info(const char *reason);

#ifdef BOOT_TRACE
extern void boot_trace_request_try(const char *extra_args);
extern void boot_trace_dump(void (*info)(const char *));
extern void boot_trace_dump_kmsg(void (*info)(const char *));
extern void boot_trace_dump_pstore(void (*info)(const char *));
extern void boot_trace_dump_saved(void (*info)(const char *));
extern void boot_trace_force_save(void (*info)(const char *));
extern void mtk_arch_reset(char mode);
extern void mdelay(unsigned long msec);

/* oem tryboot[:extra kernel arguments] */
static void cmd_oem_tryboot(const char *arg, void *data, unsigned sz)
{
	extern void mt65xx_backlight_off(void);
	extern void mt_disp_power(unsigned int on);

	boot_trace_request_try(arg && arg[0] == ':' ? arg + 1 : "");
	fastboot_okay("");
	mdelay(100);
	/* the next LK finds the panel and the display engine off, as after a power on */
	mt65xx_backlight_off();
	mt_disp_power(0);
	mtk_arch_reset(1);
}

static void cmd_oem_trace(const char *arg, void *data, unsigned sz)
{
	boot_trace_dump(fastboot_info);
	fastboot_okay("");
}

static void cmd_oem_lastkmsg(const char *arg, void *data, unsigned sz)
{
	boot_trace_dump_kmsg(fastboot_info);
	fastboot_okay("");
}

static void cmd_oem_pstore(const char *arg, void *data, unsigned sz)
{
	boot_trace_dump_pstore(fastboot_info);
	fastboot_okay("");
}

static void cmd_oem_savenow(const char *arg, void *data, unsigned sz)
{
	boot_trace_force_save(fastboot_info);
	fastboot_okay("");
}

static void cmd_oem_savedtrace(const char *arg, void *data, unsigned sz)
{
	boot_trace_dump_saved(fastboot_info);
	fastboot_okay("");
}
#endif

void fastboot_oem_register()
{
#ifdef BOOT_TRACE
	fastboot_register("oem tryboot", cmd_oem_tryboot, 1, 0);
	fastboot_register("oem trace", cmd_oem_trace, 1, 0);
	fastboot_register("oem lastkmsg", cmd_oem_lastkmsg, 1, 0);
	fastboot_register("oem pstore", cmd_oem_pstore, 1, 0);
	fastboot_register("oem savedtrace", cmd_oem_savedtrace, 1, 0);
	fastboot_register("oem savenow", cmd_oem_savenow, 1, 0);
#endif
}
