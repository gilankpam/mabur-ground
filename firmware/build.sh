#!/usr/bin/env bash
#
# Builds the mabur GS image for one board:
#   firmware/build.sh <board> [make-target]
# Boards: radxa_zero3 runcam_wifilink emax_wyvern-link orangepi_zero2w.
# The make target defaults to `all`; `defconfig`, `menuconfig`,
# `savedefconfig`, `<pkg>-rebuild` and friends pass through to Buildroot.
#
# Downloads Buildroot into firmware/buildroot/ on first use and builds in
# firmware/output/<board>/. maburgs and maburplay come from the mabur-ground
# tree this script sits in, against the mabur at third_party/mabur, so the
# image is the commit it was built from. Non-interactive; any failure exits
# non-zero. On NixOS run it inside firmware/shell.nix.
#
# Environment:
#   MABUR_DIR         mabur checkout to build against (default: the
#                     third_party/mabur pin; ../mabur for a change that spans
#                     both repos)
#   DEVOURER_VERSION  devourer commit (default: devourer master at make time)
#   VERSION           image version for os-release (default: the exact tag,
#                     else the short sha, with -dirty for local changes)
set -eo pipefail

BUILDROOT_VERSION=2025.08.1
RK3566_BOARDS="radxa_zero3 runcam_wifilink emax_wyvern-link"
BOARDS="$RK3566_BOARDS orangepi_zero2w"

FW=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$FW/.." && pwd)
BOARD="$1"
TARGET="${2:-all}"

die() { echo "build.sh: $*" >&2; exit 1; }

case " $BOARDS " in
    *" $BOARD "*) ;;
    *) die "usage: firmware/build.sh <board> [make-target]; boards: $BOARDS" ;;
esac

MABUR_SRC="${MABUR_DIR:-$TREE/third_party/mabur}"
[ -f "$MABUR_SRC/common/CMakeLists.txt" ] ||
    die "no mabur checkout at $MABUR_SRC: run \`git submodule update --init third_party/mabur\` (or set MABUR_DIR)"
MABUR_SRC=$(cd "$MABUR_SRC" && pwd)

if [ -z "$VERSION" ]; then
    VERSION=$(git -C "$TREE" describe --tags --exact-match 2>/dev/null ||
              git -C "$TREE" rev-parse --short HEAD)
    git -C "$TREE" diff --quiet HEAD -- || VERSION="$VERSION-dirty"
fi

# Buildroot itself, once. rsync merges into an existing directory so a
# cached dl/ survives.
if [ ! -f "$FW/buildroot/Makefile" ]; then
    echo "Downloading Buildroot $BUILDROOT_VERSION..."
    tarball="$FW/buildroot-$BUILDROOT_VERSION.tar.gz"
    curl -fL -o "$tarball" "https://buildroot.org/downloads/buildroot-$BUILDROOT_VERSION.tar.gz"
    tar -xzf "$tarball" -C "$FW"
    rm "$tarball"
    mkdir -p "$FW/buildroot"
    rsync -a "$FW/buildroot-$BUILDROOT_VERSION/" "$FW/buildroot/"
    rm -rf "$FW/buildroot-$BUILDROOT_VERSION"
fi

O="$FW/output/$BOARD"
mkdir -p "$O"

# The defconfig's file name must stay <board>_defconfig: post-build-script.sh
# derives BUILD_CONFIG (and the sysupgrade asset name) from it.
DEFCONFIG="$O/${BOARD}_defconfig"
if [ -f "$FW/configs/${BOARD}_defconfig" ]; then
    cp "$FW/configs/${BOARD}_defconfig" "$DEFCONFIG"
else
    cat "$FW/configs/rk3566_common.fragment" "$FW/configs/$BOARD.fragment" > "$DEFCONFIG"
fi

# Command-line variables reach every sub-make and the post-build script's
# environment.
mk() {
    make -C "$FW/buildroot" O="$O" BR2_EXTERNAL="$FW" \
        MABUR_GS_SRC_DIR="$TREE" MABUR_GS_MABUR_DIR="$MABUR_SRC" \
        MABUR_GS_VERSION="$VERSION" "$@"
}

if [ "$TARGET" != savedefconfig ]; then
    mk BR2_DEFCONFIG="$DEFCONFIG" defconfig
fi
[ "$TARGET" = defconfig ] && exit 0

# A local site is rsynced once per build dir; without this a rebuild would
# keep the old tree's maburgs/maburplay, and post-build-script.sh would
# re-edit an already edited maburplay.toml.
[ "$TARGET" = all ] && mk mabur-dirclean

mk -j"$(nproc)" "$TARGET"
[ "$TARGET" = all ] || exit 0

IMAGES="$O/images"
cd "$IMAGES"
if [ -f u-boot-rockchip.bin ]; then
    cp u-boot-rockchip.bin u-boot.bin
elif [ -f u-boot-sunxi-with-spl.bin ]; then
    cp u-boot-sunxi-with-spl.bin u-boot.bin
fi
for f in sdcard.img u-boot.bin emmc_bootloader.img rootfs.squashfs; do
    [ -f "$f" ] && cp "$f" "${BOARD}_$f"
done
cp boot.scr "${BOARD}_boot.scr"
md5sum rootfs.squashfs > rootfs.squashfs.md5sum
md5sum u-boot.bin > u-boot.bin.md5sum
tar zcf "$BOARD.tar.gz" rootfs.squashfs u-boot.bin ./*.md5sum

kernel_loc=$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION="\(.*\)"$/\1/p' "$O/.config")
kernel_file="${BR2_DL_DIR:-$FW/buildroot/dl}/linux/$(basename "$kernel_loc")"
devourer_dir=$(ls -d "$O"/build/devourer-* 2>/dev/null | head -1)
{
    echo "board=$BOARD"
    echo "version=$VERSION"
    echo "ground=$(git -C "$TREE" rev-parse HEAD)"
    echo "mabur=$(git -C "$MABUR_SRC" rev-parse HEAD)"
    echo "devourer=${devourer_dir##*/devourer-}"
    echo "buildroot=$BUILDROOT_VERSION"
    echo "kernel=$kernel_loc"
    echo "kernel_sha256=$( [ -f "$kernel_file" ] && sha256sum "$kernel_file" | cut -d' ' -f1 || echo unknown)"
} > build-info.txt
cat build-info.txt
echo "Build completed: $IMAGES"
