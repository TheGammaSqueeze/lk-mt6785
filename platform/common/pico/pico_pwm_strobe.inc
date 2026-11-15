/*
 * GammaOS Pico backlight strobe (BFI), shared by the boards (platform/common/pico): the PWM
 * divider that drops the dimming carrier to one period per panel frame, the per frame restart
 * that locks the pulse to the frame end (disp_pwm_strobe_tick, from the pacer), the BFI levels and
 * the lit window the brightness rides in. Included as text by each board's ddp_pwm.c after its
 * g_pwm_inited, g_pwm_main_id, PWM_REG_SET/GET and pwm_get_reg_base. A board with BFI defines
 * PICO_BFI_DIV() (the divider for one period per frame) and PICO_BFI_MIN_PULSE (the shortest
 * pulse its backlight boost lights); without them only the plain free running strobe is built.
 */
/* AYANEO motion-blur strobe: when non-zero, disp_pwm_set_backlight writes this into the PWM
 * clock divider (CON_0), dropping the dimming carrier to ~frame rate so the backlight strobes
 * instead of dimming uniformly = shorter sample-and-hold = less perceived motion blur. 0 = off
 * (normal ~kHz dimming). 256 gives ~98Hz on this panel (flicker-free, real blur reduction). The
 * user's brightness rides on the duty (CON_1), so brightness and strobe cooperate. */
static unsigned int g_strobe_div = 0;
static volatile int g_strobe_locked;   /* 1 = re-align the pulse every frame (disp_pwm_strobe_tick) */
volatile unsigned int g_dbg_strobe_ticks;   /* oem bfi: frame ticks seen (locked strobe only) */
#if defined(PICO_BFI_DIV)
/* "Strobe" is black frame insertion done with the backlight. One PWM period per panel
 * frame (26 MHz / (424 + 1) / 1024 = 59.74 Hz), the counter restarted at every RDMA frame end
 * by the pacer (disp_pwm_strobe_tick from rot_pace), so the lit part of the period is the
 * vertical blanking right after the panel finished scanning the new frame (the ~3.8 ms at
 * vfp 294) plus a little: the pulse is at most BFI_WINDOW/1024 of the period and the user's
 * brightness scales it (disp_pwm_set_backlight), so brightness still works under BFI. */
/* lit fraction of the frame in 1024ths at full brightness: 512 = half the frame dark (the
 * usual BFI duty; a quarter was far too dark). oem bfi:N tunes it live. */
static unsigned int g_bfi_window = 512u;
static int g_bfi;
static unsigned int s_last_level_1024 = 1023u;
static int g_bfi_mode;                 /* 0 off, 1 one pulse per frame */
void disp_pwm_bfi_window(unsigned int w)
{
	if (w < 64u) w = 64u;
	if (w > 1024u) w = 1024u;
	g_bfi_window = w;
	if (g_bfi && g_pwm_inited)
		disp_pwm_set_backlight(g_pwm_main_id, (int)s_last_level_1024);
}
unsigned int disp_pwm_bfi_window_get(void) { return g_bfi_window; }
/* oem bfi: the live strobe state and the PWM registers */
int disp_pwm_bfi_state(char *out, int n)
{
	unsigned int rb = g_pwm_inited ? pwm_get_reg_base(g_pwm_main_id) : 0;
	if (n < 0)   /* second line: the registers and the tick count */
		return snprintf(out, -n, "pwm: en=%x con0=%08x con1=%08x ticks=%u",
				rb ? PWM_REG_GET(rb + DISP_PWM_EN_OFF) : 0, rb ? PWM_REG_GET(rb + DISP_PWM_CON_0_OFF) : 0,
				rb ? PWM_REG_GET(rb + DISP_PWM_CON_1_OFF) : 0, g_dbg_strobe_ticks);
	return snprintf(out, n, "lock=%d div=%u win=%u bfi=%d mode=%d last=%u init=%d",
			g_strobe_locked, g_strobe_div, g_bfi_window, g_bfi, g_bfi_mode, s_last_level_1024, g_pwm_inited);
}
#endif
void disp_pwm_strobe_mode(int on)
{
#if defined(PICO_BFI_DIV)
	/* on = BFI level as the user reads it, the DARK fraction: 1 = 25 % dark (lit 768/1024),
	 * 2 = 50 % (512), 3 = 75 % dark (lit 256); 4..6 = the same fractions with TWO pulses per
	 * frame (carrier at half the period, ~119 Hz: far less flicker, a faint double image on
	 * fast motion), still re-aligned to the frame end every frame */
	{
		int lvl = on ? ((on - 1) % 3) + 1 : 0;
		g_bfi_mode = on ? 1 : 0;   /* one pulse per frame */
		if (lvl) g_bfi_window = (lvl >= 3) ? 256u : (lvl == 2 ? 512u : 768u);
		g_bfi = lvl ? 1 : 0;
		g_strobe_div = lvl ? PICO_BFI_DIV() : 0u;
	}
	g_strobe_locked = on ? 1 : 0;
	if (g_pwm_inited)
		disp_pwm_set_backlight(g_pwm_main_id, (int)s_last_level_1024);   /* re-applies divider + scaled duty */
	return;
#endif
	g_strobe_div = on ? 256u : 0u;
	/* Apply the divider to CON_0 immediately. The brightness path caches same-level calls and
	 * skips the PWM write, so a menu toggle would otherwise not take effect until the next
	 * brightness change. Guarded until the PWM is inited: the early-boot settings load just sets
	 * the variable, and the first post-init backlight set then applies it. CON_1 (duty) is left
	 * as-is so the current brightness is preserved. */
	if (g_pwm_inited) {
		unsigned int reg_base = pwm_get_reg_base(g_pwm_main_id);
		PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
		PWM_REG_SET(reg_base + DISP_PWM_CON_0_OFF, g_strobe_div << 16);
		PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 1);
		PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
	}
}


#if defined(PICO_BFI_DIV)
/* BFI: the brightness becomes the pulse width inside the per frame lit window. A low brightness
 * setting must not shrink the pulse to nothing: the board's backlight boost sets the shortest
 * pulse it still lights (PICO_BFI_MIN_PULSE, 1024ths of the frame). */
static inline int pico_bfi_pulse(int level_1024)
{
	level_1024 = (level_1024 * (int)g_bfi_window + 512) / 1024;
	if (level_1024 < PICO_BFI_MIN_PULSE) level_1024 = PICO_BFI_MIN_PULSE;
	return level_1024;
}
#endif

/* AYANEO backlight strobe (motion-blur / response-lag reduction).
 *
 * The DISP_PWM dimming carrier freq = src_clk / (div+1) / 1024. Stock div=0 gives a
 * ~kHz carrier, so a reduced duty just DIMS uniformly. Cranking the divider drops the
 * carrier toward the frame rate, at which point a duty < full makes the backlight go
 * DARK for part of each ~frame = a strobe that shortens the sample-and-hold time and
 * cuts perceived motion blur. div sweeps the strobe rate, duty the ON fraction (of 1024).
 *
 * Two modes:
 *  - disp_pwm_strobe(): FREE-RUNNING carrier. Simple but drifts against the panel scan,
 *    so at a strobe rate != frame rate the eye sums two frames = a double-image.
 *  - disp_pwm_strobe_locked() + disp_pwm_strobe_tick(): the tick restarts the PWM counter
 *    once per frame at the FRAME_DONE point (primary_display_config_input), so EXACTLY one
 *    pulse fires per frame, phase-locked to the panel. That removes the double-image while
 *    keeping the persistence cut. div sets the period (~1 frame), duty the ON fraction.
 * div=0 disables the strobe and restores the normal dimming carrier at the given duty. */
void disp_pwm_strobe(unsigned int div, unsigned int duty)
{
	unsigned int reg_base = pwm_get_reg_base(g_pwm_main_id);

	disp_pwm_init(g_pwm_main_id);
	g_strobe_locked = 0;   /* free-running: the per-frame tick does nothing */
	if (div > 0x3ff)  div = 0x3ff;
	if (duty > 0x3ff) duty = 0x3ff;

	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
	PWM_REG_SET(reg_base + DISP_PWM_CON_0_OFF, div << 16);
	PWM_REG_SET(reg_base + DISP_PWM_CON_1_OFF, (duty << 16) | 0x3ff);
	if (duty > 0)
		PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x1);
	else
		PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x0);
	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 1);
	PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
}

/* Vsync-locked strobe: same waveform, but re-latched each frame by disp_pwm_strobe_tick().
 * div=0 (or duty=0) disables the lock and restores the normal carrier at `duty` brightness. */
void disp_pwm_strobe_locked(unsigned int div, unsigned int duty)
{
	if (div == 0 || duty == 0) {
		disp_pwm_strobe(0, duty ? duty : 1023);   /* clears g_strobe_locked, restores carrier */
		return;
	}
	disp_pwm_strobe(div, duty);   /* apply the waveform (clears lock)... */
	g_strobe_locked = 1;          /* ...then arm the per-frame re-lock */
}

/* Called once per frame from the FRAME_DONE point in primary_display_config_input. Restart
 * the PWM counter (EN off->on) so the single pulse re-aligns to THIS frame = phase lock.
 * Cheap (2 register writes); no-op unless a locked strobe is active. */
void disp_pwm_strobe_tick(void)
{
	unsigned int reg_base;

	if (!g_strobe_locked)
		return;
	g_dbg_strobe_ticks++;
	reg_base = pwm_get_reg_base(g_pwm_main_id);
#if defined(PICO_BFI_DIV_FOLLOWS)
	/* a board whose refresh changes with the core: the period follows it at the next frame */
	if (g_bfi) {
		unsigned int d = PICO_BFI_DIV();
		if (d != g_strobe_div) {
			g_strobe_div = d;
			PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
			PWM_REG_SET(reg_base + DISP_PWM_CON_0_OFF, d << 16);
			PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 1);
			PWM_REG_SET(reg_base + DISP_PWM_COMMIT_OFF, 0);
		}
	}
#endif
	PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x0);
	PWM_REG_SET(reg_base + DISP_PWM_EN_OFF, 0x1);
}


