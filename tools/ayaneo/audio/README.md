# Boot audio pipeline

Plays a short PCM boot sound out the loudspeaker in sync with the first frame of
the LK boot animation. Gated behind `AYANEO_BOOT_AUDIO` (default `yes`).

## Hardware path

MT6359 PMIC integrated codec (main DAC -> LINEOUT L) feeds a board class-D amp
enabled by two GPIOs (`spk_en` = GPIO24, `spk2_en` = GPIO21, from the device
dtbo `ext_spk_control`). The player brings up the whole chain from scratch,
faithfully translated from the mt6785 kernel driver:

1. Audio MTCMOS power domain (`spm_mtcmos_ctrl_audio`, SPM `AUDIO_PWR_CON`).
2. AFE clocks + DL1 downlink memif (48 kHz, stereo, 16-bit) -> interconnect
   (`AFE_CONN3/4`) -> ADDA downlink SRC.
3. MT6359 codec analog: bias, clocks, SDM/NCP, DL SRC, LINEOUT L from the main
   DAC (`mt6359_dl_lineout_on`).
4. Assert the amp GPIOs, then start the memif DMA.

The memif is a hardware ring, so a small teardown thread stops playback after
the clip length so it plays once. `video_rainbow_boot_stop()` also stops it at
the kernel handoff so audio is quiet before the kernel re-inits the codec.

Player: `platform/mt6785/ayaneo_audio.c`. Fixed format 48 kHz / stereo / 16-bit.

## Storage

The audio blob lives in the `boot_b` partition at offset `0x01000000` (16 MB),
past the video animation blob at offset 0. One partition, one flash.

Blob format (`ABA1`): `[magic, ver, rate, pcm_bytes, ch, bits, comp_len]` then
raw-deflate (wbits=-15) of 48 kHz stereo s16le PCM. LK `zunzip()` decodes it
straight into the DMA buffer.

## Build the blob

    # 1. encode a WAV (any rate/channels/depth) to the ABA1 blob.
    #    with no input it generates a built-in chime.
    python3 encode_audio.py yoursound.wav -o boot_audio.bin

    # 2. combine the video animation blob and the audio blob into boot_b.
    python3 build_boot_b.py ../gba_anim/logo_anim.bin boot_audio.bin boot_b.img

Flash `boot_b.img` to `boot_b` and the signed LK to `lk_a`.

Tunables: `encode_audio.py --gain` scales volume, `--max-seconds` caps length.
Lineout analog gain is `ZCD_CON1` (0 dB step 8) in `ayaneo_audio.c`.
