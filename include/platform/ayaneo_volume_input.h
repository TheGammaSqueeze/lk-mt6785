#ifndef AYANEO_VOLUME_INPUT_H
#define AYANEO_VOLUME_INPUT_H

/* Board-independent volume control input.
 *
 * Returns +1 for one "volume up" step, -1 for one "volume down" step, 0 when
 * nothing happened since the previous call. Call it once per frame from the
 * emulator and menu loops; it keeps its own edge/decoder state.
 *
 * AYANEO Pocket Air Mini: the two MTK keypad keys (VOL_UP hw key 0x11,
 * VOL_DOWN hw key 0x00), edge detected, one press = one step.
 * Konkr Pocket Advance (KONKR_KPA): the rotary volume wheel on GPIO15/GPIO6,
 * gray coded quadrature, 24 detents per turn, decoded the way the kernel's
 * rotary-encoder driver does in half-period mode so one detent = one step
 * and the direction matches Android's KEY_VOLUMEUP/DOWN mapping. */
int ayaneo_volume_dir(void);

/* Configure the input pins (KPA encoder pull-ups). Safe to call more than once;
 * every caller of ayaneo_volume_dir() may call it at its own input init. */
void ayaneo_volume_input_init(void);

#endif
