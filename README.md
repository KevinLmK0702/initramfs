# evergo initramfs (MT6833 / Redmi Note 11 5G)

Bring-up ramdisk for the mainline Linux port on the MT6833 evergo.  It is
consumed twice, from the same directory:

- `build_boot.py` (mt6833-port tree) packs it into the boot image ramdisk
- `build_all.sh` points `CONFIG_INITRAMFS_SOURCE` at it, as the kernel's
  embedded fallback

## Layout

```
init.c              the init program (source, version controlled)
Makefile            builds root/init from init.c
root/               the ramdisk tree -- this is what gets packed
  init              generated, not tracked
  lib/firmware/     firmware the kernel asks for through request_firmware()
  dev/ proc/ sys/   mount-point placeholders (kept via .gitkeep)
```

Everything outside `root/` is repository plumbing and never reaches the
device.  `build_boot.py` walks `root/` only.

## Build

```
make            # aarch64-linux-gnu-gcc -static -Os -> root/init
make clean
```

`mkinitramfs.sh` in the mt6833-port tree calls `make -C <this repo>` and
then refreshes `root/lib/firmware`, so the binary is always rebuilt from
the source here instead of being copied from the build tree.

## Firmware notes

`lib/firmware/novatek_ts_ct_fw.bin` is the **CSOT** panel blob.  The vendor
ships two, selected by LCM name: `k16a_36_02_0a_vdo` (Tianma) needs
`novatek_ts_fw.bin`, `k16a_42_02_0b_vdo` (CSOT) needs
`novatek_ts_ct_fw.bin`.  This unit is CSOT.  Flashing the Tianma blob makes
every button of the right-hand PIN column land one row higher.

## Relation to the xaga initramfs

Structure follows https://github.com/MT6895-Mainline/initramfs.git, which
keeps `init.c` + `Makefile` under version control and generates the binary.
The init program itself is different on purpose: the xaga one is a
~400-line freestanding switch_root helper, while this one is a 2300-line
bring-up diagnostic that dumps cmdline/DT/block devices/IRQs/touch pads and
then hands over to the Linux rootfs.
