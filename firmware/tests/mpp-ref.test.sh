#!/bin/sh
# The image's rockchip-mpp package and tools/build-arm64.sh's static MPP must
# be the same MPP commit: both apply gs/player/mpp-patches/, which are ported
# to that one commit, and maburplay is the same decoder in either build.
#
#   sh firmware/tests/mpp-ref.test.sh

cd "$(dirname "$0")/../.." || exit 1
pkg=$(sed -n 's/^ROCKCHIP_MPP_VERSION *= *//p' firmware/package/rockchip-mpp/rockchip-mpp.mk)
dev=$(sed -n 's/^MPP_REF=//p' tools/build-arm64.sh)
if [ -z "$pkg" ] || [ -z "$dev" ]; then
	echo "FAIL: could not read the MPP refs (package '$pkg', build-arm64.sh '$dev')"
	exit 1
fi
if [ "$pkg" != "$dev" ]; then
	echo "FAIL: MPP refs differ: $pkg (firmware/package/rockchip-mpp) vs $dev (tools/build-arm64.sh)"
	exit 1
fi
echo "mpp-ref: ok ($pkg)"
