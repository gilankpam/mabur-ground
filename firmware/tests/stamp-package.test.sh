#!/bin/sh
# Host-side test for firmware/scripts/stamp-package.sh: VERSION and CHANGELOG
# go in first, the original members keep their names and order, so an older
# image's sysupgrade still installs the package.
#
#   sh firmware/tests/stamp-package.test.sh

cd "$(dirname "$0")/.." || exit 1
STAMP=$PWD/scripts/stamp-package.sh

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

pass=0
fail=0
ok() { pass=$((pass + 1)); }
ko() { fail=$((fail + 1)); echo "FAIL $*"; }

mkdir -p "$T/pkg" "$T/meta" "$T/meta2" "$T/x"
cd "$T/pkg" || exit 1
echo rootfs >rootfs.squashfs
echo uboot >u-boot.bin
md5sum rootfs.squashfs >rootfs.squashfs.md5sum
md5sum u-boot.bin >u-boot.bin.md5sum
tar czf "$T/p.tar.gz" rootfs.squashfs rootfs.squashfs.md5sum u-boot.bin u-boot.bin.md5sum
printf 'version=v1\n' >"$T/meta/VERSION"
printf 'one \xe2\x80\xa6\ntwo\n' >"$T/meta/CHANGELOG"
printf 'version=v2\n' >"$T/meta2/VERSION"
printf 'three\n' >"$T/meta2/CHANGELOG"

list() { tar tzf "$1" | sed 's|^\./||' | tr '\n' ' '; }
want="VERSION CHANGELOG rootfs.squashfs rootfs.squashfs.md5sum u-boot.bin u-boot.bin.md5sum "

sh "$STAMP" "$T/p.tar.gz" "$T/meta" || ko "stamp failed"
got=$(list "$T/p.tar.gz")
[ "$got" = "$want" ] && ok || ko "members '$got', want '$want'"
tar xzf "$T/p.tar.gz" -C "$T/x"
(cd "$T/x" && md5sum --quiet -c ./*.md5sum >/dev/null 2>&1) && ok || ko "md5sums do not verify"
cmp -s "$T/x/VERSION" "$T/meta/VERSION" && ok || ko "VERSION differs"
cmp -s "$T/x/CHANGELOG" "$T/meta/CHANGELOG" && ok || ko "CHANGELOG differs"

sh "$STAMP" "$T/p.tar.gz" "$T/meta2" || ko "restamp failed"
got=$(list "$T/p.tar.gz")
[ "$got" = "$want" ] && ok || ko "restamped members '$got', want '$want'"
rm -rf "$T/x"; mkdir "$T/x"
tar xzf "$T/p.tar.gz" -C "$T/x"
cmp -s "$T/x/VERSION" "$T/meta2/VERSION" && ok || ko "restamp kept the old VERSION"
cmp -s "$T/x/CHANGELOG" "$T/meta2/CHANGELOG" && ok || ko "restamp kept the old CHANGELOG"

echo "stamp-package: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
