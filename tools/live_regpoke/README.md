Live register-poke kernel module for the MT6785 device (multicore-at-LK research).
Build: pull the device /proc/config.gz to the kernel tree .config (must have CONFIG_MODULES=y),
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- prepare, then
  make -C <kernel> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- M=$PWD modules
Vermagic matches "4.14.186 SMP preempt mod_unload modversions aarch64" (MODVERSIONS ok, no sig).
Run: insmod regpoke.ko base=0x10006200 n=12 ; dmesg | grep REGPOKE   (init returns -EINVAL so it
  auto-unloads after printing; re-insmod with different base/n to sweep windows).
