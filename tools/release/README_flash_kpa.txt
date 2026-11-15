GammaOS Pico for the Konkr Pocket Advance: the LK, release @VERSION@ (lk-mt6785 @COMMIT@)

GammaOS Pico writes one thing on the console: the bootloader (LK), in both of its slots, lk_a and lk_b.
Android, its data and every other partition stay as they are. With a GammaOS Pico microSD card in the
slot the console starts GammaOS Pico; without it (or with Boot to Android chosen in the menu) it starts
Android as before. The card's contents are in the separate sdcard package.

  spflash/gammaos/   SP Flash Tool v5 (Windows): the GammaOS Pico LK to lk_a and lk_b
  spflash/stock/     SP Flash Tool v5 (Windows): the stock LK (BW03) back to lk_a and lk_b
  mtkclient/         mtkclient (Linux): the same two, flash_gammaos.sh and flash_stock.sh
  fastboot/          fastboot (Windows .bat, Linux .sh): for a console already running GammaOS Pico
  SHA256SUMS         checksums of every file here

SP Flash Tool and mtkclient talk to the console's own preloader over USB, so they work on a
stock console with a locked bootloader. fastboot needs the GammaOS Pico LK already installed.

Tested on a console: SP Flash Tool v5.1924 and the fastboot scripts. Not yet tested on a console:
mtkclient, which writes the same two partitions with the same files. SP Flash Tool V6 does not
work with these packages: use v5.

1. Windows: SP Flash Tool v5
   The release's gammaos-pico-kpa-windows-tools.zip has both things this needs: SP Flash Tool
   v5.1924 and the MediaTek USB driver installer. Run the driver installer once, then open
   flash_tool.exe from SP Flash Tool's folder.
   a. Download tab, Scatter-loading File: Browse to MT6785_Android_scatter.txt in spflash/gammaos/
      (Browse, not the history list).
   b. Mode "Download Only". Only lk_a and lk_b are ticked. Never use "Format All + Download" or
      "Firmware Upgrade": they rewrite the whole flash.
   c. Power the console off, click Download, then connect it over USB. Wait for the green tick.
   The preloader file is there because SP Flash Tool reads its memory setup to connect; it is not
   written.

2. Linux: mtkclient (https://github.com/bkerler/mtkclient)
   Install it once (Debian/Ubuntu; see its README-INSTALL.md for other systems):
       sudo apt install git python3-pip libusb-1.0-0
       git clone --recursive https://github.com/bkerler/mtkclient
       pip3 install --user -r mtkclient/requirements.txt mtkclient/
       sudo cp mtkclient/Setup/Linux/*.rules /etc/udev/rules.d/
       sudo udevadm control -R && sudo udevadm trigger
       sudo usermod -aG dialout,plugdev $USER      (then log out and in)
   Then: power the console off and unplug it, run mtkclient/flash_gammaos.sh, and plug the console
   in when mtkclient waits for it. When it reports Done, unplug and hold POWER to start the
   console. If mtk is not on your PATH, run it as MTK="python3 /path/to/mtkclient/mtk.py"
   ./flash_gammaos.sh. ModemManager can grab the console's USB port: stop it while flashing
   (sudo systemctl stop ModemManager) if mtkclient never connects.

3. fastboot (updates, GammaOS Pico already installed)
   Put the console in its bootloader: hold SELECT + START + R while powering on (or
   "adb reboot bootloader" from Android). Then run fastboot/flash_gammaos.bat (Windows) or
   fastboot/flash_gammaos.sh (Linux, with fastboot installed).

Going back to stock: the same ways with spflash/stock/, mtkclient/flash_stock.sh or
fastboot/flash_stock.*. The console then starts Android as it did from the factory, whatever is
on the card.

Power on buttons (GammaOS Pico LK)
  SELECT held                start GammaOS Pico from the card (after a Boot to Android)
  SELECT + START held        start Android
  SELECT + START + R held    the bootloader (fastboot)

If the console ever does not start after a flash, SP Flash Tool or mtkclient restore the stock LK
whatever is in lk_a and lk_b, since both work below the LK.

Source: https://github.com/TheGammaSqueeze/lk-mt6785 (branch lk-gba-emu-sd-card-kpa).
