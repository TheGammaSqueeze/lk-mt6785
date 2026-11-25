# Konkr Pocket Advance stock files

Copied unmodified from the stock firmware package `BW03_20260730-260730_1755` (Android 12) for the
release flash package (`tools/release/make_release.sh`):

- `lk.img`: the stock LK, flashed to `lk_a` and `lk_b` by the package that goes back to stock.
- `tee.img`: the stock secure firmware, flashed to `tee_a` and `tee_b` by the package that goes back to stock,
  and the file the patched one in `tools/konkr/firmware/images` is made from.
- `preloader_k85v1_64.bin`: the stock preloader. It is never flashed by our packages; SP Flash
  Tool reads its memory setup to bring up the console while it connects.
- `MT6785_Android_scatter.txt`: the stock partition layout, from which
  `tools/release/lk_scatter.py` writes the scatters that tick only `lk_a`, `lk_b`, `tee_a` and `tee_b`.

`SHA256SUMS` lists their checksums. These are the console maker's files, redistributed so the flash
package works out of the box.
