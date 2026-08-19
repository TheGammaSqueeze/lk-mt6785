#!/usr/bin/env python3
"""
Combine the video animation blob and the audio blob into one boot_b image.

Layout (must match ayaneo_audio.c AYANEO_AUDIO_OFF and the video reader):
  offset 0x00000000 : video animation blob (logo_anim.bin, "GBA1")
  offset 0x01000000 : audio blob (boot_audio.bin, "ABA1")

Usage:
  build_boot_b.py <video_blob> <audio_blob> <out_boot_b.img>
"""
import sys

AUDIO_OFF = 0x01000000

def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    video, audio, out = sys.argv[1], sys.argv[2], sys.argv[3]
    v = open(video, 'rb').read()
    a = open(audio, 'rb').read()
    if len(v) > AUDIO_OFF:
        raise SystemExit("video blob %d B overlaps audio offset 0x%x" %
                         (len(v), AUDIO_OFF))
    buf = bytearray(AUDIO_OFF + len(a))
    buf[:len(v)] = v
    buf[AUDIO_OFF:AUDIO_OFF + len(a)] = a
    open(out, 'wb').write(buf)
    print("wrote %s: video=%d B @0, audio=%d B @0x%x, total=%d B"
          % (out, len(v), len(a), AUDIO_OFF, len(buf)))

if __name__ == '__main__':
    main()
