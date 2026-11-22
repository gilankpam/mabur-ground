#!/bin/sh
# pack-images.sh <images_dir> <board>
#
# Turns what Buildroot left in <images_dir> into the files a release
# carries: <board>_sdcard.img, <board>_boot.scr, <board>_u-boot.bin,
# <board>_rootfs.squashfs, <board>_emmc_bootloader.img (when built), and the
# upgrade package <board>.tar.gz.
#
# The package holds rootfs.squashfs and, on the RK3566 boards, u-boot.bin,
# each with its .md5sum. The Orange Pi's holds no U-Boot: an upgrader writes
# u-boot.bin at the Rockchip offset (32 KiB), and on the H618 that write
# runs over the SPL at 256 KiB. Its bootloader ships only as
# orangepi_zero2w_u-boot.bin, for flashing on purpose.
set -e

dir=$1
board=$2
[ -d "$dir" ] && [ -n "$board" ] || { echo "usage: pack-images.sh <images_dir> <board>" >&2; exit 2; }
cd "$dir"

if [ -f u-boot-rockchip.bin ]; then
	cp u-boot-rockchip.bin u-boot.bin
elif [ -f u-boot-sunxi-with-spl.bin ]; then
	cp u-boot-sunxi-with-spl.bin u-boot.bin
fi
for f in sdcard.img u-boot.bin emmc_bootloader.img rootfs.squashfs; do
	if [ -f "$f" ]; then
		cp "$f" "${board}_$f"
	fi
done
cp boot.scr "${board}_boot.scr"

rm -f ./*.md5sum
md5sum rootfs.squashfs >rootfs.squashfs.md5sum
set -- rootfs.squashfs rootfs.squashfs.md5sum
case "$board" in
orangepi_zero2w) ;;
*)
	md5sum u-boot.bin >u-boot.bin.md5sum
	set -- "$@" u-boot.bin u-boot.bin.md5sum
	;;
esac
tar zcf "$board.tar.gz" "$@"
