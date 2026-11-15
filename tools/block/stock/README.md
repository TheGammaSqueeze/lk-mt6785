# Konkr Pocket Block stock files

Copied unmodified from the stock firmware package `M8695_BW01_V02_20261014_user` (Android 12) for
the release flash package (`tools/release/make_release.sh`):

- `lk.img`: the stock LK, flashed to `lk_a` by the package that goes back to stock.
- `preloader_m8695_bw01.bin`: the stock preloader. It is never flashed by our packages; SP Flash
  Tool reads its memory setup to bring up the console while it connects.
- `MT6768_Android_scatter.txt`: the stock partition layout, from which
  `tools/release/lk_scatter.py` writes the scatters that tick only `lk_a`.

`SHA256SUMS` lists their checksums. These are the console maker's files, redistributed so the flash
package works out of the box.
