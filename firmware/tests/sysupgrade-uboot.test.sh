#!/bin/sh
# Host-side test for board/common/overlay/sbin/sysupgrade's U-Boot step.
#
#   sh firmware/tests/sysupgrade-uboot.test.sh
#
# sysupgrade writes u-boot.bin at sector 64 (32 KiB), the Rockchip offset.
# On the Orange Pi Zero 2W that write runs over the H618's SPL at 256 KiB
# and the board no longer boots, so there it must never write a bootloader,
# whatever the package or the command line hands it: an old package with
# u-boot.bin inside, --uboot=, or the boot-time DVR autoupdate (--archive).
#
# Runs a copy with /tmp and /etc/os-release redirected into a temp dir and
# dd stubbed. BOOT_DISK points at a plain file, so a step that gets as far as
# the disk check dies there ("not found") instead of touching anything.

cd "$(dirname "$0")/.." || exit 1
SRC=$PWD/board/common/overlay/sbin/sysupgrade

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

mkdir -p "$T/bin"
printf '#!/bin/sh\necho "dd $*" >>"%s/dd.log"\n' "$T" >"$T/bin/dd"
printf '#!/bin/sh\n' >"$T/bin/ps"
printf '#!/bin/sh\n' >"$T/bin/free"
chmod +x "$T/bin/dd" "$T/bin/ps" "$T/bin/free"
# /tmp first, through a placeholder: $T itself lives under /tmp.
sed -e "s|/tmp|@TMP@|g" -e "s|/etc/os-release|$T/os-release|g" \
	-e "s|/overlay/.firstboot|$T/firstboot|g" -e "s|@TMP@|$T/tmp|g" \
	"$SRC" >"$T/sysupgrade"
touch "$T/disk"

pass=0
fail=0
ok() { pass=$((pass + 1)); }
ko() { fail=$((fail + 1)); echo "FAIL [$name] $*"; }

# run <board> <args...>: output in $T/out, dd calls in $T/dd.log.
run() {
	printf 'BUILD_CONFIG=%s\n' "$1" >"$T/os-release"
	shift
	rm -rf "$T/dd.log" "$T/tmp" && mkdir -p "$T/tmp"
	PATH=$T/bin:$PATH BOOT_DISK=$T/disk sh "$T/sysupgrade" "$@" -x >"$T/out" 2>&1
}

# pkg <with-uboot>: a sysupgrade package like the old image repo published.
pkg() {
	rm -rf "$T/p" && mkdir -p "$T/p" && cd "$T/p" || exit 1
	echo rootfs >rootfs.squashfs && md5sum rootfs.squashfs >rootfs.squashfs.md5sum
	if [ "$1" = yes ]; then
		echo uboot >u-boot.bin && md5sum u-boot.bin >u-boot.bin.md5sum
	fi
	tar zcf "$T/pkg.tar.gz" ./*
	cd - >/dev/null || exit 1
}

echo uboot >"$T/u.bin"

name="opi --uboot"
run orangepi_zero2w --uboot="$T/u.bin"
grep -q "bootloader left alone" "$T/out" && ok || ko "no skip message: $(tail -3 "$T/out")"
grep -q "Aborting" "$T/out" && ko "aborted: $(tail -3 "$T/out")" || ok
[ -f "$T/dd.log" ] && ko "dd ran: $(cat "$T/dd.log")" || ok

name="opi old package with u-boot.bin"
pkg yes
run orangepi_zero2w --archive="$T/pkg.tar.gz"
grep -q "bootloader left alone" "$T/out" && ok || ko "no skip message: $(tail -3 "$T/out")"
[ -f "$T/dd.log" ] && ko "dd ran: $(cat "$T/dd.log")" || ok

name="opi package without u-boot.bin"
pkg no
run orangepi_zero2w --archive="$T/pkg.tar.gz"
grep -q "bootloader left alone" "$T/out" && ok || ko "no skip message: $(tail -3 "$T/out")"
[ -f "$T/dd.log" ] && ko "dd ran: $(cat "$T/dd.log")" || ok

name="radxa --uboot still writes"
run radxa_zero3 --uboot="$T/u.bin"
grep -q "bootloader left alone" "$T/out" && ko "skipped on a Rockchip board" || ok
grep -q "not found" "$T/out" && ok || ko "did not reach the disk: $(tail -3 "$T/out")"

echo "sysupgrade-uboot: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
