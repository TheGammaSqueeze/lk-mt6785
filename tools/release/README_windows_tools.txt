GammaOS Pico for the Konkr Pocket Advance: the Windows install's tools

The GammaOS Pico bootloader is installed on Windows with SP Flash Tool v5. This package has the
two things that needs, as MediaTek ships them, unchanged:

  SP Flash Tool v5.1924/      MediaTek's SP Flash Tool, version 5.1924 (flash_tool.exe)
  MediaTek USB driver/        MediaTek SP Driver 5.23.07.02, the USB driver installer
  SHA256SUMS                  checksums of every file here

SP Flash Tool V6 does not work with the GammaOS Pico flash packages: use this v5.

1. Run "MediaTek USB driver/MTK Driver Setup.exe" once and let it install the driver. Windows
   may ask for permission; restart if it asks.
2. Unzip the release's gammaos-pico-kpa-<version>-flash.zip as well.
3. Open "SP Flash Tool v5.1924/flash_tool.exe" and follow the flash package's README.txt
   (SP Flash Tool section): the Download tab, MT6785_Android_scatter.txt from spflash/gammaos/,
   mode Download Only, only lk_a and lk_b ticked.

The guides, with pictures: https://github.com/TheGammaSqueeze/GammaOSPico/wiki/Installing

Where these come from: SP Flash Tool v5.1924 is the archive published at spflashtool.com
(SP_Flash_Tool_v5.1924_Win.rar), unpacked as it is; only its Driver folder, which held nothing but
web links to a third-party file host, is left out, the driver installer itself being given
beside it. Both are MediaTek's software, redistributed here unchanged for installing GammaOS Pico;
they are not covered by GammaOS Pico's licence, and they come with no warranty.
