# Menu themes

Each folder here is a theme for the game list menu, picked on the Display page (Theme row) and saved
with the other settings. `tools/ayaneo/gba/make_sdcard.sh` packs every folder into the one
`gammaos/menu.pack`: `classic` first, then the others in name order.

A theme folder holds:

| File | What |
|---|---|
| `thumbnail.png` | 172 x 98, shown on the Theme row |
| `packed.png` | the UI atlas, 512 x 1024 RGBA, every sprite in the same rectangle as the asset tree's `sprites/packed.png` (whose `packed.json` stays as it is) |
| `wall.png` | optional: the home wallpaper, 1536 x 552, opaque, tiling left to right like the asset tree's `layout/asset/sprite/wallpaper/wall.png`; without it the theme keeps the tree's wallpaper |

`classic` has only a thumbnail: its atlas and wallpaper are the asset tree's own. The packer refuses
an atlas or a wallpaper of another size. A theme changes those pixels only; the layout, the scenes,
the fonts and the sounds are shared by every theme. The pack keeps the theme as its name, so renaming a folder
resets the choice to Classic.

| Theme | Notes |
|---|---|
| `classic` | the original look |
| `gba-indigo` | GBA Indigo: an indigo Game Boy Advance shell, cartridge cards, silver icons, and the Mario Graphite wallpaper (grey character outlines on charcoal) |
