# GammaOS Pico: the emulator, menu and SD card objects every Pico board builds, with their
# compiler flags. Included by each platform's rules.mk (mt6785 for the AYANEO Pocket Air Mini and
# the Konkr Pocket Advance, mt6768 for the Konkr Pocket Block) where its object list is built.

# In-LK emulator driver: GBA (gpSP) takes over the same hooks as the GBC build.
ifeq ($(AYANEO_GBA),yes)
OBJS += emu/gba/gba_driver.o
OBJS += emu/gba/gba_core_loader.o
OBJS += emu/gba/core_file.o		# cores load from the SD card (core_file.h)
OBJS += emu/gba/gba_cc_lut444.o		# color-correction LUT (LK display-side, not core)
OBJS += emu/gba/gba_boxart.o
OBJS += emu/gba/gba_boxart_sd.o
OBJS += emu/gba/fat_ro.o
OBJS += emu/gba/sd_fat.o
OBJS += emu/gba/fat_wr.o
OBJS += emu/gba/fat_exfat.o
OBJS += emu/gba/gba_sd_save.o
OBJS += emu/gba/pico_input.o		# the buttons for every core: the player's map, hotkeys, the Controls page
$(BUILDDIR)/emu/gba/pico_input.o:       CFLAGS += -Os
ifeq ($(AYANEO_GBA_SD),yes)
OBJS += emu/gba/sd_fastboot.o
OBJS += emu/gba/menu_fastboot.o
OBJS += emu/gba/card_writer.o		# in-game saves written to the card off the game's thread
OBJS += emu/gba/pico_clock.o emu/gba/pico_stats.o	# the date and time, play time
OBJS += emu/gba/pico_state_shot.o	# a save state's picture and time (the Suspend Point List)
$(BUILDDIR)/emu/gba/pico_state_shot.o: CFLAGS += -O2
OBJS += emu/gba/pico_png.o emu/gba/pico_artimport.o	# box art put on the card as PNG
OBJS += emu/gba/pico_libmem.o	# the game library's memory
OBJS += emu/gba/sd_check.o	# oem sd-check: the card's clusters each used once
OBJS += emu/gba/rom_archive.o emu/gba/rom_7z.o emu/gba/rom_lzma.o	# games in .zip and .7z
OBJS += emu/gba/pico_version.o	# the version and commit, for the menus
OBJS += emu/gba/pico_fold.o	# games' names: accents folded for sorting and the ASCII menus
OBJS += emu/gba/pico_names.o	# games' names in any script: the names glyph pack
# The cores are loadable blobs on the SD card (/cores/*.blob, built in their own -pico repos);
# lk_a carries only the loader and the session runner (video, input, audio) for each.
OBJS += emu/gbc/gbc_core_loader.o
OBJS += emu/gbc/gbc_sd_run.o
OBJS += emu/snes9x/snes_core_loader.o
OBJS += emu/snes9x/snes_sd_run.o
OBJS += emu/genesis/genesis_core_loader.o
OBJS += emu/genesis/genesis_sd_run.o
OBJS += emu/nes/nes_core_loader.o
OBJS += emu/nes/nes_sd_run.o
# SNES-Classic-mini-style carousel menu engine (imported from lk-snes-menu). The
# software renderer is float-heavy, so build the render/scene/audio modules with
# hardware NEON FP (LK default -mfloat-abi=soft would make every op an aeabi call).
OBJS += emu/gba/menu/snes_pack.o emu/gba/menu/snes_scene.o emu/gba/menu/snes_render.o emu/gba/menu/snes_audio.o
OBJS += emu/gba/menu/snes_menu.o emu/gba/gba_snes_menu.o
# the Current menu theme (its own layout, drawn in software from its card pack)
OBJS += emu/gba/pico_current.o emu/gba/pico_i18n.o
# the settings every theme offers
OBJS += emu/gba/pico_settings.o
$(BUILDDIR)/emu/gba/pico_settings.o:    CFLAGS += -Os
# a core's own options (the Core Settings page for NES, SNES, Genesis/SMS/GG)
OBJS += emu/gba/pico_coreopt.o
$(BUILDDIR)/emu/gba/pico_coreopt.o:     CFLAGS += -Os
# a core's emulation on a big CPU core (each system's Big Core option)
OBJS += emu/gba/pico_bigrun.o
# cheat codes for every core: the release's packs and the player's own .cht, the Cheats page
OBJS += emu/gba/pico_cheats.o
$(BUILDDIR)/emu/gba/pico_cheats.o:      CFLAGS += -Os
# playing together over Bluetooth, the flow every theme shares (the consoles with the link)
ifneq ($(filter yes,$(KONKR_KPA) $(KONKR_BLOCK)),)
OBJS += emu/gba/pico_link_ui.o
$(BUILDDIR)/emu/gba/pico_link_ui.o:     CFLAGS += -Os
OBJS += emu/gba/pico_net_ui.o emu/gba/pico_scrape.o emu/gba/pico_ra.o	# the Network tab: Wi-Fi, box art, RetroAchievements
$(BUILDDIR)/emu/gba/pico_net_ui.o:      CFLAGS += -Os
$(BUILDDIR)/emu/gba/pico_scrape.o:      CFLAGS += -Os
$(BUILDDIR)/emu/gba/pico_ra.o:          CFLAGS += -Os
ifneq ($(wildcard emu/gba/pico_ss_creds.gen.h),)
$(BUILDDIR)/emu/gba/pico_scrape.o:      CFLAGS += -DPICO_SS_CREDS	# the generated (never committed) credentials
endif
# the Classic themes' in-game menu (its kit is kept where only these consoles have room)
OBJS += emu/gba/pico_classic_igm.o emu/gba/pico_classic_ui.o emu/gba/pico_classic_set.o
$(BUILDDIR)/emu/gba/pico_classic_igm.o: CFLAGS += -mfpu=neon -mfloat-abi=softfp -ffast-math -Os
$(BUILDDIR)/emu/gba/pico_classic_ui.o:  CFLAGS += -mfpu=neon -mfloat-abi=softfp -ffast-math -Os
$(BUILDDIR)/emu/gba/pico_classic_set.o: CFLAGS += -mfpu=neon -mfloat-abi=softfp -ffast-math -Os
endif
GBA_MENU_FP := -mfpu=neon -mfloat-abi=softfp -ffast-math
# perf-critical BLITTER: -O2 + NEON for full 60fps. This is the dominant per-frame
# cost (wallpaper + card + text blits); -Os here drops below 60fps and the double
# buffer visibly flickers. Keeping just this at -O2 fits the 2MB lk_a budget (raw
# ~1.98MB). The rest is orchestration/one-time build - -Os to fit alongside gpSP.
$(BUILDDIR)/emu/gba/menu/snes_render.o: CFLAGS += $(GBA_MENU_FP)
$(BUILDDIR)/emu/gba/menu/snes_audio.o:  CFLAGS += $(GBA_MENU_FP) -Os
$(BUILDDIR)/emu/gba/menu/snes_scene.o:  CFLAGS += $(GBA_MENU_FP) -Os
$(BUILDDIR)/emu/gba/menu/snes_menu.o:   CFLAGS += $(GBA_MENU_FP) -Os
# menu orchestration + the fastboot debug harness: not perf-critical, -Os to fit lk_a
# (the gpSP emulation core is a separate archive; gba_driver is just I/O + save/load).
$(BUILDDIR)/emu/gba/gba_snes_menu.o:    CFLAGS += -Os
$(BUILDDIR)/emu/gba/menu_fastboot.o:    CFLAGS += -Os
$(BUILDDIR)/emu/gba/gba_driver.o:       CFLAGS += -Os
$(BUILDDIR)/emu/gba/menu/snes_pack.o:   CFLAGS += -Os
$(BUILDDIR)/emu/gba/gba_snes_menu.o:    CFLAGS += $(GBA_MENU_FP) -Os
$(BUILDDIR)/emu/gba/pico_current.o:     CFLAGS += $(GBA_MENU_FP) -O2
$(BUILDDIR)/emu/gba/pico_i18n.o:       CFLAGS += -Os
# reclaim the last few KB of the 2MB lk_a budget from the (correctness-safe) SD
# driver objects that carry this menu variant
$(BUILDDIR)/emu/gba/gba_driver.o:       CFLAGS += -Os
$(BUILDDIR)/emu/gba/gba_sd_save.o:      CFLAGS += -Os
$(BUILDDIR)/emu/gba/sd_fastboot.o:      CFLAGS += -Os
$(BUILDDIR)/emu/gba/fat_ro.o:           CFLAGS += -Os
$(BUILDDIR)/emu/gba/fat_wr.o:           CFLAGS += -Os
$(BUILDDIR)/emu/gba/fat_exfat.o:        CFLAGS += -Os
$(BUILDDIR)/emu/snes9x/snes_core_loader.o: CFLAGS += -Os
$(BUILDDIR)/emu/snes9x/snes_sd_run.o:      CFLAGS += -Os
$(BUILDDIR)/emu/genesis/genesis_core_loader.o: CFLAGS += -Os
$(BUILDDIR)/emu/genesis/genesis_sd_run.o:      CFLAGS += -Os
$(BUILDDIR)/emu/nes/nes_core_loader.o:         CFLAGS += -Os
$(BUILDDIR)/emu/nes/nes_sd_run.o:              CFLAGS += -Os
endif
endif
