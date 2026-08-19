# GBA boot animation pipeline

Turns the Blender source (`gba.blend`) into the `logo_anim.bin` blob the LK boot
animation player reads from the "logo" partition.

1. `blender -b gba.blend --python render_frames.py`
   Renders the 60fps/300-frame scene at 640x360, every 2nd frame -> 150 PNGs
   in /tmp/gba_frames (30fps of the 5s).
2. `python3 encode.py`
   RGB565-converts each frame, appends 15 fade-to-black frames, raw-deflate
   (wbits=-15, so LK's zunzip() reads it directly) compresses each, and writes
   `logo_anim.bin`: header ['GBA1', ver, w, h, nframes, fps] then per frame
   [u32 comp_len][deflate data] of a w*h RGB565 image.

Flash `logo_anim.bin` to the `logo` partition (13 MB, ample) and the built LK to
`lk_a`. The player is in platform/mt6785/mt_disp_drv.c (ayaneo_rainbow_thread).
