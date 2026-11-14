/*
 * led_stage.h - bring-up checkpoints shown on a board LED (a BLOCK_LED_DIAG build, see
 * platform/mt6768/block_led_diag.c); nothing in any other build. The argument is the stage
 * number, shown as that many blinks.
 */
#ifndef __LED_STAGE_H__
#define __LED_STAGE_H__

#ifdef BLOCK_LED_DIAG
void block_led_stage(unsigned int stage);
void block_led_mark(unsigned int colour);
#define LED_STAGE(c) block_led_stage(c)
#define LED_MARK(c)  block_led_mark(c)
#else
#define LED_STAGE(c) do { } while (0)
#define LED_MARK(c)  do { } while (0)
#endif

#endif /* __LED_STAGE_H__ */
