/*
 * Konkr Pocket Advance: no analog sticks, no analog triggers. L2 and R2 are plain buttons
 * (GPIO94 = BTN_TL2, GPIO93 = BTN_TR2 in the device tree, active low) and work as TOGGLES: one
 * press turns rewind (L2) or fast forward (R2) on at full level, the next press turns it off.
 * The cores keep reading the pairmini's analog "trigger level" (0..255) unchanged.
 *
 * The pairmini implementation (ayaneo_joypad.c) powers an ADC rail, drives GPIO15 as an
 * output and talks to an SGM58031 at i2c6 address 0x48. On the KPA GPIO15 is one phase of
 * the volume encoder and nothing answers at 0x48 (the kernel's gamepad@48 node is a shell),
 * so every entry point here is a neutral no-op: sticks centred, triggers released, dpad
 * emulation off. The callers keep working unchanged.
 */

#define KPA_GPIO_L2    94u
#define KPA_GPIO_R2    93u
#include "../common/pico/pico_joypad_toggles.inc"
