#!/bin/sh
# Host-side test for firmware/scripts/pack-images.sh: what goes into each
# board's upgrade package <board>.tar.gz.
#
#   sh firmware/tests/pack-images.test.sh
#
# The RK3566 packages carry U-Boot; the Orange Pi's must not. An upgrader
# writes u-boot.bin at the Rockchip offset (32 KiB); on the H618 that write
# runs over the SPL at 256 KiB with a misplaced copy and the board no longer
# boots.

cd "$(dirname "$0")/.." || exit 1
PACK=$PWD/scripts/pack-images.sh

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); }
ko() { fail=$((fail + 1)); echo "FAIL [$board] $*"; }

# fake_images <dir> <uboot-name>: what Buildroot leaves in images/.
fake_images() {
	mkdir -p "$1"
	echo rootfs >"$1/rootfs.squashfs"
	echo uboot >"$1/$2"
	echo sdcard >"$1/sdcard.img"
	echo scr >"$1/boot.scr"
}

check_board() {
	board=$1 uboot=$2 want=$3
	dir=$T/$board/images
	fake_images "$dir" "$uboot"
	if ! sh "$PACK" "$dir" "$board" >"$T/$board.log" 2>&1; then
		ko "pack-images.sh failed: $(cat "$T/$board.log")"
		return
	fi
	got=$(tar tzf "$dir/$board.tar.gz" 2>/dev/null | sed 's|^\./||' | sort | tr '\n' ' ')
	[ "$got" = "$want" ] && ok || ko "tarball has '$got', want '$want'"
	mkdir -p "$T/$board/x"
	tar xzf "$dir/$board.tar.gz" -C "$T/$board/x" 2>/dev/null
	(cd "$T/$board/x" && md5sum --quiet -c ./*.md5sum >/dev/null 2>&1) && ok || ko "md5sums do not verify"
	for f in "${board}_sdcard.img" "${board}_boot.scr" "${board}_rootfs.squashfs"; do
		[ -f "$dir/$f" ] && ok || ko "missing $f"
	done
}

check_board radxa_zero3 u-boot-rockchip.bin \
	"rootfs.squashfs rootfs.squashfs.md5sum u-boot.bin u-boot.bin.md5sum "
check_board emax_wyvern-link u-boot-rockchip.bin \
	"rootfs.squashfs rootfs.squashfs.md5sum u-boot.bin u-boot.bin.md5sum "
check_board orangepi_zero2w u-boot-sunxi-with-spl.bin \
	"rootfs.squashfs rootfs.squashfs.md5sum "
board=orangepi_zero2w
[ -f "$T/orangepi_zero2w/images/orangepi_zero2w_u-boot.bin" ] && ok ||
	ko "the OPi's bootloader file for SD/dd flashing is missing"

echo "pack-images: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
