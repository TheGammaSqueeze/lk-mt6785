# GammaOS Pico

GammaOS Pico is a retro handheld OS that lives in the bootloader of MediaTek MT6785 handhelds.
It replaces only the LK bootloader and runs Game Boy, Game Boy Color, Game Boy Advance, SNES,
Mega Drive / Genesis, Master System, Game Gear and SG-1000 games straight from a microSD card.
Take the card out and the console boots its stock Android as before.

![A game library](documentation/screenshots/library.png)

Supported consoles:

- **Konkr Pocket Advance**: the main target. Downloads are on the
  [releases page](https://github.com/TheGammaSqueeze/lk-mt6785/releases); details in
  [documentation/konkr-pocket-advance.md](documentation/konkr-pocket-advance.md).
- **AYANEO Pocket Air Mini**: the original port of this LK, see
  [documentation/pocket-air-mini](documentation/pocket-air-mini/README.md).

## What makes it different

GammaOS Pico isn't an app on top of Android or a Linux distro. It runs inside the console's
bootloader, before any operating system, and owns the hardware outright.

- **Fast on.** From power on to the GBA boot logo in about two and a half seconds (Android takes
  around 25). Hold Power to save where you are and switch off; turn it back on and you're back
  in the game.
- **Low latency by design.** The panel is retimed to each console's real refresh rate (59.73 Hz
  for GBA and Game Boy, 60.1 Hz for SNES, 50 Hz for PAL), every frame is paced to the panel's scan,
  buttons are read as late as possible, and run-ahead ("Preemptive Frames") hides the games' own lag.
- **Android stays.** Only the bootloader changes. Pull the card (or pick Boot to Android) and the
  console starts its stock Android, untouched.
- **Everything on the card.** Emulators, settings, saves, states and box art all live on the
  microSD card. Swap the card, swap the whole setup.
- **Your card, your way.** Games are found in GammaOS's `roms/<system>/` or where another front
  end keeps them: `ROMs/<system>/` (ES-DE), `Emulation/roms/<system>/` (EmuDeck) or any
  `<folder>/roms/<system>/`, with the usual folder names (`gba`, `snes`, `megadrive`,
  `mastersystem`, `gamegear` and so on). Drop a PNG cover named like the game into a `boxart`
  folder beside it and the console sizes it for the menu at the next power on.
- **Zipped games welcome.** Games can stay in .zip or .7z archives, unpacked as they start (about
  half a second for a 16 MB GBA game, resume at power on included). An archive with several games
  lists each one, and one that cannot be opened says why instead of failing quietly.
- **Made for big libraries.** Tens of thousands of games list in moments. Hold a direction to
  speed through a library, jump letter by letter with L / R, or press Y to search by name, in one
  system or all of them, with an on-screen keyboard. Box art loads as you reach it.
- **The good stuff from the start.** Rewind, fast forward, save states (written to the card while
  the game plays on), scanline and LCD filters, colour correction and backlight black frame
  insertion, in a box art menu: the Current theme, with the Classic and GBA Indigo themes in beta.
- **The real time.** The console's own clock runs the games that keep time (Pokemon Ruby,
  Sapphire, Emerald, Gold, Silver and Crystal, Boktai), shows in the menus, and is set with its
  time zone in Settings > Date & Time. Each game shows how long it has been played and when last.
- **Tilt, gyro and rumble.** The console's own motion sensor and vibration motor stand in for the
  cartridges': Yoshi Topsy-Turvy and Kirby Tilt 'n' Tumble tilt (a flick of the console makes
  Kirby jump), WarioWare: Twisted! turns and rumbles, as do Drill Dozer and the Game Boy Color's
  rumble cartridges.
- **Headphones.** Sound moves to the 3.5 mm jack as soon as headphones are plugged in, and back
  to the speakers when they come out.
- **In your language.** The menus and the in-game menu in 17 languages: English, French, German,
  Spanish, Italian, Dutch, Portuguese, Russian, Japanese, Korean, Simplified and Traditional
  Chinese, Polish, Turkish, Arabic, Hebrew and Persian (Settings > Language).

> **Put the real Game Boy Advance BIOS on the card.** Copy your own console's BIOS as
> `bios/gba_bios.bin` (16 KB). GBA games then start on Nintendo's own BIOS: the original boot logo
> and chime, and the exact BIOS routines every game was made against. Without it armGBA falls back
> to its built-in replacement, which runs most games but not all of them the same way, and gpSP
> (the second GBA emulator) cannot run at all. The BIOS is Nintendo's and is not included.

## Screenshots

Captured on a Konkr Pocket Advance from the display buffers, with the demo games from the release;
the in-game menu shots lay its drawer over the game the way the display hardware blends them.

| | |
|---|---|
| ![Home](documentation/screenshots/home.png) | ![Game library](documentation/screenshots/library.png) |
| Home: the systems on the card, with the date, the time and the battery | A system's library: box art, how long each game has been played and when last |
| ![Recently played](documentation/screenshots/recent.png) | ![Settings](documentation/screenshots/settings.png) |
| Resume: the games played most recently | Settings |
| ![Date and time](documentation/screenshots/date-time.png) | ![BIOS boot](documentation/screenshots/bios-boot.png) |
| Date & Time: the console's clock and time zone, which the games' clocks run on | BIOS Boot: the Game Boy Advance start-up screen |
| ![Japanese](documentation/screenshots/language-ja.png) | ![Arabic](documentation/screenshots/language-ar.png) |
| In Japanese | In Arabic, written right to left |
| ![In game](documentation/screenshots/in-game.png) | ![The in-game menu](documentation/screenshots/pico-menu.png) |
| In game (Tobu Tobu Girl Deluxe, LCD filter) | The in-game menu over the game (Menu button) |
| ![Display page](documentation/screenshots/pico-menu-display.png) | ![Gameplay page](documentation/screenshots/pico-menu-gameplay.png) |
| Display: brightness, filters, aspect ratio, colour correction, BFI | Gameplay: CPU clock, run-ahead, rewind, the GBA core, the live panel refresh |
| ![Core options](documentation/screenshots/core-options.png) | ![System page](documentation/screenshots/pico-menu-system.png) |
| armGBA's own options | System: resume, the BIOS intro and the battery |
| ![Game Boy Color display page](documentation/screenshots/pico-menu-gbc.png) | ![Theme setting](documentation/screenshots/settings-theme.png) |
| A Game Boy Color game's Display page: palette, colour correction, dark filter | Picking a theme |
| ![Classic theme](documentation/screenshots/theme-classic.png) | ![Classic theme display page](documentation/screenshots/theme-classic-display.png) |
| The Classic theme (beta) | Its Display page, with the themes |
| ![GBA Indigo theme](documentation/screenshots/theme-gba-indigo.png) | |
| The GBA Indigo theme (beta) | |

## Buttons

At power on:

| Hold | Starts |
|---|---|
| nothing | whatever the card is set to (GammaOS Pico, or Android after Boot to Android) |
| SELECT | GammaOS Pico (the way back after Boot to Android) |
| SELECT + START | Android |
| SELECT + START + R | the bootloader (fastboot), for updates |

No card, or a card without GammaOS Pico on it, always starts Android.

In a game:

| Button | Does |
|---|---|
| Menu | the in-game menu (save and load states, filters, settings); hold 1.5 s to go back to the menu |
| R2 | fast forward on / off |
| L2 | rewind on / off |
| SELECT + START + L + R (hold) | restart the game |
| Volume wheel | volume; with SELECT held, brightness |
| X / Y | autofire B / A |
| Power (hold 1.2 s) | save and switch off |

In the Classic theme's game list, START plays, SELECT sorts and X switches between systems. During
the boot logo, hold B to skip it; the volume wheel works there too.

## The menus

Everything GammaOS Pico can set, as it is laid out on the console (the Current theme, the default).
Every menu is in the language chosen in Settings.

### Home

| Tab | What it holds |
|---|---|
| Games | the systems on the card, each with its console and logo; Left / Right move between systems, A opens a system's library (its games with their box art, how long each has been played and when last), where holding Up / Down speeds up, Left / Right turn a page and L / R jump from letter to letter; Y searches the system (or, on this page, every system) with an on-screen keyboard |
| Resume | the games played most recently, newest first, to carry on with one press |
| Settings | the console settings below |

L and R switch between the tabs, A picks and B goes back. The header shows the menu you are in,
the date, the time and the battery.

### Settings

| Row | Values |
|---|---|
| Brightness | the backlight, 0 to 100 % |
| Volume | 0 to 100 % |
| Black Frame Insertion | Off, 25 %, 50 %, 75 %: the backlight strobes for sharper motion |
| Theme | Current; Classic and GBA Indigo (beta). A applies |
| Colour | Blue, Indigo, Violet, Orchid, Rose, Crimson, Coral, Amber, Lime, Emerald, Teal, Sepia, Graphite |
| Language | the 17 languages. A applies |
| Date & Time | a page: Year, Month, Day, Hour, Minute (the console's clock, which Android shares), Time zone (UTC-12:00 to UTC+14:00 in quarter hours), 24-hour clock |
| BIOS Boot | a page: Skip BIOS Intro, BIOS Intro Background (Black or White), Mute BIOS Audio, BIOS Intro on Resume |
| Game Resume | a page: Resume on Power On (switching off in a game saves your place and the next power on goes straight back), Resume on Game Open (opening a game carries on where you left it; hold B while opening to start afresh) |
| Boot to Android | restarts the console into Android; hold SELECT at power on to come back |

### In a game

The Menu button opens the in-game menu over the game. Its first page: Resume, Save state and Load
state (Left / Right pick the slot), the Display, Sound, Gameplay and System pages, Reset game and
Exit game. The pages hold what each emulator offers:

| Page | Game Boy Advance | Game Boy / Game Boy Color | SNES | Mega Drive, Master System, Game Gear, SG-1000 |
|---|---|---|---|---|
| Display | Brightness, LCD Filter, Aspect Ratio, Color Correction, BFI, Show FPS | Brightness, LCD Filter, Aspect Ratio, Palette (original Game Boy), Color Correction, Correction Mode, Dark Filter, BFI, Show FPS | Brightness, LCD Filter, Aspect Ratio, Overscan, Hi-Res Blend, BFI, Show FPS | Brightness, Aspect Ratio, LCD Filter, BFI, Show FPS |
| Sound | Volume | Volume | Volume, Audio Filter | Volume |
| Gameplay | CPU Clock, Run-Ahead, Rewind, Benchmark, GBA Core (armGBA or gpSP), Core options, Latency LED, Panel Refresh | CPU Clock, Run-Ahead, Rewind | CPU Clock, Run-Ahead, Rewind, Turbo (auto-fire), Benchmark, Panel Refresh | CPU Clock, Run-Ahead, Rewind, Region, Refresh Rate |
| System | Resume on Power On, Resume on Game Open, BIOS Intro Background, Skip BIOS Intro, Battery | Resume on Power On, Battery | Resume on Power On, Battery | Resume on Power On, Battery |

armGBA's Core options: BIOS (Nintendo from the card, or armGBA's built-in), LCD Ghosting, Interlaced
Rows, Audio Low-Pass Filter, Solar Sensor (for Boktai), Motion Controls (tilt and gyro cartridges)
and Rumble.

The values: LCD Filter Off, Scanlines, LCD Grid, Dot Matrix, LCD1x, LCD3x (Genesis: Grid, Grid+);
Aspect Ratio Stretch, Fit, Pixel Perfect (SNES 4:3, Pixel, NTSC, PAL, Stretch; Genesis Pixel, Core,
Stretch, 4:3, Square, 16:9); Run-Ahead Off, Balanced, Responsive, Max; Overscan Crop 8, 12 or 16 px
or Off; Audio Filter Linear, Gaussian, Cubic, Sinc, None; Hi-Res Blend Off, Merge, Blur; Turbo Off,
A, B, A+B; Region Auto, USA, Europe, Japan.

### Classic theme (beta)

The SNES Classic style carousel, with its own pages from the top bar: Display (CRT filter, 4:3 or
pixel perfect, and the Theme row), Options (Mute BIOS Audio, Mute Menu Audio, burn-in reduction,
Boot to Android),
Language (English, French, German, Spanish, Italian, Dutch, Portuguese, Russian; the others leave
it in English), Legal notices and the manual. Suspend points are made when you reset a game.
X shows one system at a time; in a long carousel L / R jump from letter to letter.

## Install

Grab the latest [release](https://github.com/TheGammaSqueeze/lk-mt6785/releases). The GammaOS Pico
LK goes to `lk_a` and `lk_b`. **The way to install it is SP Flash Tool (Windows) or mtkclient
(Linux):** both write to a stock console with a locked bootloader, and neither needs an unlock or
wipes anything. fastboot is only for updates once GammaOS Pico is already installed, never for the
first install (the stock bootloader will not flash over fastboot without an unlock, which erases
your data). The SD card package goes on a FAT32 or exFAT card as it is: menu,
emulators and a set of freely licensed demo games with box art. Each package has a README. Then
add the Game Boy Advance BIOS as `bios/gba_bios.bin` (see above) and your own games to
`roms/<system>/` (or keep an ES-DE or EmuDeck style ROM folder, see above). For box art, put a
`<game>.png` in the `boxart` folder beside the game: at the next power on it becomes a `.ART`
sized for the menu, and the PNG is deleted.

## Build from source

```sh
git clone --recursive -b lk-gba-emu-sd-card-kpa https://github.com/TheGammaSqueeze/lk-mt6785.git
cd lk-mt6785
./build_konkr_kpa.sh                    # out/lk_a_kpa_signed.img + out/sdcard/ (the card)
tools/release/make_release_kpa.sh       # the two release zips in out/release/
```

You need Linux, Python 3 with Pillow and numpy, `openssl`, and `arm-none-eabi-gcc`/`g++` for the
cores (the LK itself builds with the toolchain in `gcc/`). The build fetches any core submodule
that is missing and rebuilds a core when its commit changes.

## Emulator cores

Each emulator is a separate program loaded from the card (`/cores/*.blob`), built in its own
repository with the emulator's upstream as a submodule, unmodified, plus GammaOS Pico patches and
glue. They are submodules here under `cores/`:

| Core | Systems |
|---|---|
| [armGBA-pico](https://github.com/TheGammaSqueeze/armGBA-pico) | Game Boy Advance (default, plays the boot intro) |
| [gpsp-pico](https://github.com/TheGammaSqueeze/gpsp-pico) | Game Boy Advance (needs a BIOS on the card) |
| [gambatte-pico](https://github.com/TheGammaSqueeze/gambatte-pico) | Game Boy, Game Boy Color |
| [snes9x-pico](https://github.com/TheGammaSqueeze/snes9x-pico) | SNES |
| [genesis-plus-gx-pico](https://github.com/TheGammaSqueeze/genesis-plus-gx-pico) | Mega Drive / Genesis, Master System, Game Gear, SG-1000 |

## Repository layout

| Path | What |
|---|---|
| `emu/` | GammaOS Pico itself: the SD boot gate, FAT/exFAT, the menu, input, display and audio, and the loader and session code for each core |
| `cores/` | the emulator cores (submodules) |
| `platform/mt6785/`, `dev/lcm/` | the MT6785 platform code, the GammaOS Pico drivers (`ayaneo_*`) and the panel drivers |
| `project/` | build configurations: `k85v1_64_kpa.mk` (Konkr), `k85v1_64.mk` (Pocket Air Mini) |
| `tools/release/` | the release build: packages, scatter files, demo games and box covers |
| `tools/ayaneo/`, `tools/konkr/` | signing, the menu packer, the card builder, the stock firmware files |
| `tools/ayaneo/snes/themes/` | the menu themes (see its README) |
| `tools/ayaneo/pico_current/` | the Current theme's card packs, and the languages: `i18n/` holds the texts, `build_lang.py` makes a pack per language |
| `documentation/` | device docs, and `development/` for the engineering notes kept from the ports |
| `build_*.sh` | build entry points: `build_konkr_kpa.sh`, `build_ayaneo_gba_sd.sh`, `build_ayaneo_pocket_air_mini.sh` |

The rest (`app/`, `arch/`, `kernel/`, `lib/`, `include/`, `make/`, `target/`, `scripts/`, `certs/`,
`gcc/`) is the MediaTek LK it is built on.

## Licence

The Little Kernel code and the GammaOS Pico additions are MIT ([LICENSE](LICENSE)). The MediaTek
BSP files, the stock firmware files, the SNES Classic menu assets and the emulator cores keep their
own terms; [LICENSES.md](LICENSES.md) says which part is under what.

## Credit

Built on [svoboda18/lk](https://github.com/svoboda18/lk) (MediaTek LK). The emulators are their
authors' work: armGBA, gpSP, gambatte, snes9x and Genesis Plus GX. The story of how the first core
ended up in a bootloader is in [documentation/how-cursed-is-this.md](documentation/how-cursed-is-this.md).
