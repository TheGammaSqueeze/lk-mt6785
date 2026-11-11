# Licences

This repository mixes code and data from several sources. Each part keeps its own terms; the MIT
licence in [LICENSE](LICENSE) applies only where it says so below.

| Part | Terms |
|---|---|
| Little Kernel core (Travis Geiselbrecht): `kernel/`, `arch/`, `lib/` and the other files carrying his MIT notice | MIT ([LICENSE](LICENSE)) |
| GammaOS Pico additions by TheGammaSqueeze: `emu/`, the `platform/mt6785/ayaneo_*` drivers, the panel drivers `dev/lcm/st7123p_hd640_boe_dsi_vdo/` and `dev/lcm/st7703_hd720_dsi_vdo/` (code), `project/k85v1_64_kpa.mk`, the build scripts (`build_*.sh`), `tools/ayaneo/` scripts, `tools/konkr/` scripts, `tools/release/` and the documentation | MIT ([LICENSE](LICENSE)) |
| GammaOS Pico changes inside MediaTek files (`platform/`, `app/`, `dev/`) | the file's own terms below; the GammaOS Pico lines in them are also offered under MIT |
| MediaTek BSP (`platform/`, `app/mt_boot/`, `dev/`, `include/`, `certs/` and other files carrying MediaTek's copyright statement) | MediaTek's terms in each file's header; third-party code inside it is listed in [NOTICE](NOTICE) |
| Stock firmware files of the consoles: `tools/konkr/stock/` (Konkr Pocket Advance BW03 LK, preloader, scatter), the stock cert partitions and `lk_main_dtb` under `tools/ayaneo/` and `tools/konkr/`, the panel init tables transcribed from the stock LKs, and the amplifier profile and DSP firmware extracted from the stock vendor image into `platform/mt6785/ayaneo_aw88166_acf.h` | the property of their makers (Konkr/Arbor, AYANEO, MediaTek, Awinic); redistributed unmodified so the console can be installed and restored. Not covered by the MIT licence. |
| MediaTek test signing keys (`tools/ayaneo/keys/`) | MediaTek's published default test keys, included for signing images the consoles accept |
| SNES Classic menu assets (`tools/ayaneo/snes/assets/`) | Nintendo's. Not covered by the MIT licence; included only as the menu's look, no games. |
| Emulator cores (`cores/`, git submodules) | each core's own licence in its repository: armGBA (MPL 2.0), gpSP (GPL 2.0), gambatte (GPL 2.0), snes9x (non-commercial snes9x licence), Genesis Plus GX (non-commercial licence). The cores run as separate programs loaded from the SD card. |
| Demo games in the release's SD card package, and their box covers (made by `tools/release/demo_games.py` from each game's own art; `tools/release/art/` holds title screens captured from two of the games) | each game's own licence, shipped beside it on the card (`licences/games/`) |

If you are a rights holder and want something removed, open an issue.
