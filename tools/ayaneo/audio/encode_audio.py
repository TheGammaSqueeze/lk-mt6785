#!/usr/bin/env python3
"""
Encode a boot sound into the AYANEO 'ABA1' blob the LK player reads.

Output format (little-endian):
  0  u32 magic 'ABA1'
  4  u32 version = 1
  8  u32 sample_rate (always 48000)
  12 u32 pcm_bytes (uncompressed, 48k stereo s16le)
  16 u16 channels (2)
  18 u16 bits (16)
  20 u32 comp_len
  24 raw-deflate(wbits=-15) of the PCM

Usage:
  encode_audio.py [input.wav] [-o out.bin] [--gain 1.0] [--max-seconds 5]

If no input WAV is given, a short built-in chime is generated so the pipeline
always produces a flashable artifact. Drop in a real WAV to replace it; any
sample rate / channel count / bit depth is accepted and converted to
48 kHz stereo 16-bit.
"""
import sys, struct, zlib, wave, argparse
import numpy as np

RATE = 48000
MAXPCM = 1024 * 1024          # must match AYANEO_PCM_MAX in ayaneo_audio.c
COMPMAX = 768 * 1024          # must match AYANEO_AUDIO_COMPMAX


def read_wav(path):
    # Prefer soundfile: handles float/24-bit/extensible WAV the stdlib rejects.
    try:
        import soundfile as sf
        a, sr = sf.read(path, always_2d=True, dtype='float32')
        return a, sr
    except Exception:
        pass
    with wave.open(path, 'rb') as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        a = np.frombuffer(raw, dtype='<i2').astype(np.float32) / 32768.0
    elif sw == 1:
        a = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) |
             (b[:, 2].astype(np.int32) << 16))
        v = np.where(v & 0x800000, v - 0x1000000, v)
        a = v.astype(np.float32) / 8388608.0
    elif sw == 4:
        a = np.frombuffer(raw, dtype='<i4').astype(np.float32) / 2147483648.0
    else:
        raise SystemExit("unsupported sample width %d" % sw)
    a = a.reshape(-1, ch)
    return a, sr


def gen_chime():
    """A simple two-note rising chime with a soft decay, ~1.1 s."""
    def note(freq, dur, t0):
        n = int(RATE * dur)
        t = np.arange(n) / RATE
        env = np.exp(-t * 3.2)
        wav = (np.sin(2 * np.pi * freq * t) +
               0.5 * np.sin(2 * np.pi * 2 * freq * t) * np.exp(-t * 6)) * env
        return wav.astype(np.float32)
    a1 = note(523.25, 0.55, 0)      # C5
    a2 = note(783.99, 0.75, 0)      # G5
    buf = np.zeros(int(RATE * 1.15), np.float32)
    buf[:len(a1)] += a1 * 0.8
    off = int(RATE * 0.35)
    buf[off:off + len(a2)] += a2 * 0.9
    return buf.reshape(-1, 1), RATE


def resample(a, sr):
    if sr == RATE:
        return a
    n_out = int(round(a.shape[0] * RATE / sr))
    xo = np.linspace(0, a.shape[0] - 1, n_out)
    xi = np.arange(a.shape[0])
    return np.stack([np.interp(xo, xi, a[:, c]) for c in range(a.shape[1])], axis=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input', nargs='?')
    ap.add_argument('-o', '--out', default='boot_audio.bin')
    ap.add_argument('--gain', type=float, default=1.0)
    ap.add_argument('--max-seconds', type=float, default=5.0)
    args = ap.parse_args()

    if args.input:
        a, sr = read_wav(args.input)
        src = args.input
    else:
        a, sr = gen_chime()
        src = "built-in chime"

    a = resample(a, sr)
    if a.shape[1] == 1:
        a = np.repeat(a, 2, axis=1)          # mono -> stereo
    elif a.shape[1] > 2:
        a = a[:, :2]

    a = a * args.gain
    maxframes = int(RATE * args.max_seconds)
    if a.shape[0] > maxframes:
        a = a[:maxframes]

    a = np.clip(a, -1.0, 1.0)
    pcm = (a * 32767.0).astype('<i2').tobytes()
    if len(pcm) > MAXPCM:
        pcm = pcm[:MAXPCM - (MAXPCM % 4)]

    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    comp = co.compress(pcm) + co.flush()
    if len(comp) > COMPMAX:
        raise SystemExit("compressed audio %d > COMPMAX %d; shorten the clip"
                         % (len(comp), COMPMAX))

    hdr = struct.pack('<4sIIIHHI', b'ABA1', 1, RATE, len(pcm), 2, 16, len(comp))
    with open(args.out, 'wb') as f:
        f.write(hdr + comp)

    secs = len(pcm) / (RATE * 2 * 2)
    print("source=%s  %.2fs  pcm=%d B  comp=%d B  -> %s"
          % (src, secs, len(pcm), len(comp), args.out))


if __name__ == '__main__':
    main()
