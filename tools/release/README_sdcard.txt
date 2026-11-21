GammaOS Pico: the microSD card, release @VERSION@ (lk-mt6785 @COMMIT@)

The same card works in the Konkr Pocket Advance and the Konkr Pocket Block: it carries the menus for
both screens. Copy everything in this folder to the root of a microSD card formatted FAT32 or
exFAT, put the card in the console and power it on. The console needs the GammaOS Pico LK first
(its flash package).

  gammaos/boot.cfg     boot=sd starts GammaOS Pico; boot=android starts Android (the menu's Boot to
                       Android sets it; hold SELECT at power on to come back)
  gammaos/menu.pack    the menu
  gammaos/cheats/      the cheats for the in-game menu's Cheats page, from RetroArch's cheat
                       database (libretro-database, CC BY-SA 4.0, licences/cheats/); your own
                       RetroArch .cht files go in gammaos/cheats/<system>/ (gb, gbc, gba, nes,
                       snes, md, sms, gg), named as the game
  cores/               the emulator cores: armGBA and gpSP (Game Boy Advance), gambatte (Game
                       Boy, Game Boy Color), snes9x and armSNES (SNES), Genesis Plus GX (Mega
                       Drive / Genesis, Master System, Game Gear, SG-1000), armNES (NES, Famicom
                       and Famicom Disk System)
  bios/                the Game Boy Advance BIOS goes here: copy your own as bios/gba_bios.bin
                       (16 KB). GBA games then run on Nintendo's BIOS with its boot logo; without
                       it armGBA's built-in replacement runs them, not every game the same way,
                       and gpSP cannot run (see bios/README.txt). Famicom Disk System games
                       need the Disk System BIOS as bios/disksys.rom (8 KB)
  roms/<system>/       games: gb, gbc, gba, nes, snes, genesis, sms, gg, sg (ROM files, or .zip /
                       .7z; NES games as .nes or .unf, Disk System games as .fds)
  roms/<system>/boxart/<game>.ART   box art, named like the game file
  licences/            the demo games', the cores' and the cheat database's licences and credits

Buttons
  At power on: hold SELECT for GammaOS Pico, SELECT + START for Android, SELECT + START + R (R1 on
  the Konkr Pocket Block) for the bootloader (fastboot). With nothing held the card decides
  (gammaos/boot.cfg).
  In a game: Menu opens the in-game menu (hold it 1.5 s for the menu; on the Konkr Pocket Block
  the Menu button is the side key), R2 fast forward on/off, L2 rewind on/off, SELECT + START + L +
  R held restarts the game, X and Y are autofire buttons, and holding Power for 1.2 s saves your
  place and switches off. On the Konkr Pocket Advance the volume wheel sets the volume (with
  SELECT held, the brightness); on the Konkr Pocket Block the in-game menu's Sound and Display
  pages do. A Disk System game's disk side is changed from the in-game menu (Swap Disk Side).
  In the menu: L / R switch between Games, Resume and Settings, the D-pad moves, A picks, B goes
  back, Y searches. Settings holds the language, the date and time, and the themes.

The demo games in roms/ are freely licensed homebrew, redistributed under their licences
(licences/games/). Add your own games to the system folders.

Games can also stay where another front end keeps them: the console looks in roms/, at the
card's root, and in every folder at the root and its roms folder (ROMs/<system>/ for ES-DE,
Emulation/roms/<system>/ for EmuDeck). Folder names it knows (any case):
  gb gameboy | gbc gameboycolor | gba gameboyadvance | nes famicom fds
  snes sfc superfamicom snesna
  genesis megadrive md megadrivejp | sms mastersystem | gg gamegear | sg sg1000 sg-1000
A game found in two places is listed once. Libraries of thousands of games are fine (up to 32768
on a card).

Big libraries: in a system's library hold Up or Down to speed through it, Left / Right turn a
page, L / R jump from letter to letter, and Y opens Search (on the Games page Y searches every
system). Search has its own keyboard: A types the key, Y deletes, X adds a space, R goes to the
games found.

Games can stay in .zip or .7z archives (ZIP stored or deflated, ZIP64 too; 7z with LZMA, LZMA2
or no compression). A game is unpacked as it starts, about half a second for a 16 MB Game Boy
Advance game. An archive holding several games lists each one, named after its file inside
(folders inside do not matter). An archive the console cannot open shows why when you try to
start it: damaged, password protected, packed another way (bzip2, PPMd and the like), split into
parts, or with no game inside. When a game is on the card both unpacked and in an archive, the
unpacked one is used.

Box art: put a PNG cover named like the game (<game>.png, or <game>.<extension>.png) in a boxart
folder beside the game, for example ROMs/gba/boxart/Metroid Fusion.png. At the next power on the
console turns it into <game>.ART, sized for the menu, and deletes the PNG. A PNG it cannot read
stays where it is.

The console writes settings, saves (saves/) and save states (states/) to this card.

Source: https://github.com/TheGammaSqueeze/lk-mt6785 (branch lk-gba-emu-sd-card-kpa); each core's
source is linked in licences/cores/<core>/SOURCE.txt.
