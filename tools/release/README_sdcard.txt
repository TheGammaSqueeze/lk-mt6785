GammaOS Pico for the Konkr Pocket Advance: the microSD card, release @VERSION@ (lk-mt6785 @COMMIT@)

Copy everything in this folder to the root of a microSD card formatted FAT32 or exFAT, put the
card in the console and power it on. The console needs the GammaOS Pico LK first (the flash package).

  gammaos/boot.cfg     boot=sd starts GammaOS Pico; boot=android starts Android (the menu's Boot to
                       Android sets it; hold SELECT at power on to come back)
  gammaos/menu.pack    the menu
  cores/               the emulator cores: armGBA and gpSP (Game Boy Advance), gambatte (Game
                       Boy, Game Boy Color), snes9x (SNES), Genesis Plus GX (Mega Drive /
                       Genesis, Master System, Game Gear, SG-1000)
  bios/                the Game Boy Advance BIOS goes here: copy your own as bios/gba_bios.bin
                       (16 KB). GBA games then run on Nintendo's BIOS with its boot logo; without
                       it armGBA's built-in replacement runs them, not every game the same way,
                       and gpSP cannot run (see bios/README.txt)
  roms/<system>/       games: gb, gbc, gba, snes, genesis, sms, gg, sg (raw ROM files, not zips)
  roms/<system>/boxart/<game>.ART   box art, named like the game file
  licences/            the demo games' and the cores' licences and credits

Buttons
  At power on: hold SELECT for GammaOS Pico, SELECT + START for Android, SELECT + START + R for
  the bootloader (fastboot). With nothing held the card decides (gammaos/boot.cfg).
  In a game: Menu opens the in-game menu (hold it 1.5 s for the menu), R2 fast forward on/off,
  L2 rewind on/off, SELECT + START + L + R held restarts the game, the volume wheel sets the
  volume (with SELECT held, the brightness), X / Y are autofire B / A, and holding Power for
  1.2 s saves your place and switches off.
  In the menu: L / R switch between Games, Resume and Settings, the D-pad moves, A picks, B goes
  back. Settings holds the language, the date and time, and the themes.

The demo games in roms/ are freely licensed homebrew, redistributed under their licences
(licences/games/). Add your own games to the system folders. Box art for your games can be made
with tools/ayaneo/gba/gen_boxart.py from the lk-mt6785 repository (a PNG per game, named like the
game).

The console writes settings, saves (saves/) and save states (states/) to this card.

Source: https://github.com/TheGammaSqueeze/lk-mt6785 (branch lk-gba-emu-sd-card-kpa); each core's
source is linked in licences/cores/<core>/SOURCE.txt.
