/* GammaOS latency test ROM.
 *
 * The whole screen is black, and white for as long as any button is held. Nothing else runs: the
 * loop reads the keypad register and writes the colour every few cycles, so a press shows on the
 * next line the display draws. Filmed together with the KPA's latency light (the power LED lit by
 * the bootloader within a millisecond of the press), the time between the LED and the screen
 * turning white is the whole button to picture latency of the emulator path.
 *
 * Display mode 0 with every layer off shows the backdrop, palette entry 0, on the whole screen. */
#define REG_DISPCNT  (*(volatile unsigned short *)0x04000000)
#define REG_KEYINPUT (*(volatile unsigned short *)0x04000130)
#define BACKDROP     (*(volatile unsigned short *)0x05000000)

#define WHITE 0x7FFF
#define BLACK 0x0000

int main(void)
{
	REG_DISPCNT = 0;
	for (;;) {
		/* KEYINPUT bits read 0 while a button is held, ten buttons */
		BACKDROP = (~REG_KEYINPUT & 0x03FF) ? WHITE : BLACK;
	}
}
