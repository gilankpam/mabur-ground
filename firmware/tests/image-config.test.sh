#!/bin/sh
# Host-side test for the image's online-upgrade prerequisites: an HTTPS
# client that verifies certificates, a synced clock, and the build time in
# os-release. Greps the config files so a later edit can't silently drop a line.
#
#   sh firmware/tests/image-config.test.sh

cd "$(dirname "$0")/.." || exit 1

fail=0
need() { # need <file> <fixed string>
	grep -qF -- "$2" "$1" || { echo "FAIL: $1 lacks $2"; fail=1; }
}

for f in configs/rk3566_common.fragment configs/orangepi_zero2w_defconfig; do
	for k in BR2_PACKAGE_LIBCURL=y BR2_PACKAGE_LIBCURL_CURL=y \
		BR2_PACKAGE_LIBCURL_MBEDTLS=y BR2_PACKAGE_MBEDTLS=y \
		BR2_PACKAGE_CA_CERTIFICATES=y; do
		need "$f" "$k"
	done
done
need package/busybox/busybox.config CONFIG_NTPD=y
need board/common/post-build-script.sh 'BUILD_EPOCH='
grep -q 'UPGRADE=' board/common/post-build-script.sh && { echo "FAIL: post-build still writes UPGRADE="; fail=1; }
[ -x board/common/overlay/etc/init.d/S41ntpd ] || { echo "FAIL: S41ntpd not executable"; fail=1; }
[ -x board/common/overlay/usr/share/udhcpc/default.script.d/ntpd ] || { echo "FAIL: udhcpc ntpd hook not executable"; fail=1; }

[ $fail = 0 ] && echo "image-config: ok"
exit $fail
