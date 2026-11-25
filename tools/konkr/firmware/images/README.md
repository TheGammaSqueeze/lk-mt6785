# The secure firmware GammaOS Pico installs

`tee_kpa_gammaos.img` (Konkr Pocket Advance) and `tee_block_gammaos.img` (Konkr Pocket Block): each
console's stock `tee.img` (`tools/konkr/stock`, `tools/block/stock`) after `../gammaos_cpc_patch.py`
with the console's profile, which lets the bootloader power the other CPU cores (see `../README.md`).
They differ from the stock file in one instruction and in the certificate that covers it.

These two files are the ones the release packages carry. They are kept here, and not made again
for every release, because the tool signs with a random salt: each run gives a file that is as
valid but not the same bytes, and these are the bytes that were started on both consoles.
`tools/release/make_release.sh` checks them against `SHA256SUMS` and against the stock files
before it packs them.
