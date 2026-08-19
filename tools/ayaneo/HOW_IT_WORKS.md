# How the LK boot animation + sound actually works

Short version: yes, it really is blitting every frame by hand on the CPU, out of
the bootloader, before the kernel or any OS exists. There is no GPU, no video
decoder, no audio HAL, no scheduler you would recognise. LK (Little Kernel, the
MediaTek bootloader) gives you a framebuffer, a timer, an MMU and one CPU core.
Everything below is built on top of that.

## The video

**Storage.** A normal boot logo partition is tiny and the SP Flash Tool download
agent rejects anything that is not a signed logo image, so the animation lives in
the inactive `boot_b` partition (33 MB, raw, no type/size check). It is a flat
blob: a 24-byte header (`GBA1`, width, height, frame count, fps) followed by, per
frame, a `[u32 length][raw-DEFLATE data]` record. Each frame is a full RGB565
image compressed with raw deflate (wbits -15) so LK's built-in `zunzip()` (the
same inflate it already uses to unpack the kernel) can decode it directly.

**Streaming.** The blob is bigger than any buffer LK will give you, so nothing is
loaded up front. A sliding window reads from the partition with `partition_read()`
into a ring, `memmove()`-compacting as it goes, and hands the decoder just the
bytes for the next frame. So decode is genuinely streaming off eMMC.

**Drawing a frame.** For each frame we `zunzip()` it into a scratch buffer, then
`anim_blit()` walks every output pixel of the 1280x960 panel, looks up the source
pixel (a precomputed column map plus a row divide - integer nearest-neighbour
scale), unpacks RGB565, repacks it as the panel's eBGRA8888, and writes it. That
is ~1.2 million pixels converted and written per frame, by hand, on one core. The
"cover" scale (fill the 4:3 panel, crop the sides) and the fade are just arithmetic
in that same loop.

**Getting it on screen.** This is the part that is genuinely cursed. The panel is
in DSI *video mode*: in that mode a bare display trigger early-returns and
`primary_display_config_input()` with an unchanged address is a no-op, so writing
into the framebuffer does nothing visible. The trick is to keep **two** scan-out
buffers and hand the overlay a *different* address every frame - that forces the
hardware to re-latch and actually push the new frame. `config_input()` blocks on
`FRAME_DONE`, which conveniently paces us to the panel refresh and yields the CPU.
The backlight also has to be turned on explicitly at the first frame, because the
normal boot flow only enables it *after* the logo call we hijacked.

**Threading.** It runs on its own LK thread at high priority with a guaranteed
`thread_sleep()` per frame (spinning without sleeping trips the watchdog and
resets the device - learned that the hard way). Boot continues in parallel; the
thread is told to stop right before the kernel display handoff. Frame pacing is
time-based: it computes which frame *should* be showing for the elapsed
wall-clock time and skips ahead if decode fell behind, so it plays at real speed
instead of slow motion. Resolution/fps are tuned (640x480 @ 60) so a single core
can actually keep up: the wall is decode + the per-pixel blit, not the panel.

## The sound

There is **no audio support in LK at all** - no AFE driver, no codec driver,
nothing. Playing a sound means bringing up the entire audio hardware path from
bare registers, translated from the Linux kernel driver for this exact SoC:

1. **Power**: turn on the audio MTCMOS power domain via SPM (without this every
   AFE register read just hangs the bus).
2. **Clocks + DMA**: enable the AFE, configure the DL1 downlink "memif" as a
   48 kHz / stereo / 16-bit ring buffer pointed straight at a PCM buffer in DRAM,
   wire it through the interconnect to the ADDA downlink.
3. **The serial link**: the codec (MT6359) is a *separate chip*. DL audio crosses
   to it over a serial bus (MTKAIF). You have to switch the codec's data pads into
   audio mode (`playback_gpio_set`) and match the protocol (PROTOCOL_2_CLK_P2) on
   both ends, or the DAC receives nothing.
4. **Analog**: power the codec's analog LDO rails, then run the real headphone
   power-up sequence (bias, charge pump, output-stage and feedback-loop ramps) -
   the loudspeaker amp on this board is wired to the headphone outputs, not
   line-out, which took a logic-analyser-style diff of the live device to work out.
5. **Amp**: assert the external class-D amp enable GPIOs.

The PCM is stored the same way as the video (deflate blob in `boot_b`, at a fixed
offset past the frames), decoded into a DRAM buffer, cache-flushed, and the AFE
DMAs it to the codec with zero further CPU involvement. It plays exactly once by
watching the hardware DMA read pointer and tearing down the instant the ring
wraps, then ramps the gain to mute, drops the amp, and stops the memif in that
order so there is no pop or hiss.

## Debugging it

The device is locked down (no debugfs, no `setenforce`, no `/dev/mem`), so the
audio bring-up was debugged blind by reading registers back over the LK UART:
confirm the power domain latched, the DMA pointer advances at exactly 48 kHz, the
codec registers read back what we wrote, the PCM buffer is non-zero. Each silent
boot narrowed it down one register at a time until sound came out.

So: yes. It's blitting every frame and hand-driving the audio codec, from the
bootloader, on one CPU, before anything else is alive.
