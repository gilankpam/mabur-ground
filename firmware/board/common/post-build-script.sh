#!/usr/bin/bash

eval $(grep BR2_DEFCONFIG ${O}/.config)
echo "BUILD_CONFIG=$(basename $(basename $BR2_DEFCONFIG) _defconfig)" >> $TARGET_DIR/etc/os-release

# The version comes from firmware/build.sh (the release tag, snapshot-<sha8>,
# pr<N>-<sha8>, or the local sha); the online upgrade always takes the latest
# release's package for this board.
: "${MABUR_GS_VERSION:?post-build: MABUR_GS_VERSION is not set (build through firmware/build.sh)}"
DOWNLOAD_URL="https://github.com/gilankpam/mabur-ground/releases/latest/download/$(basename $(basename $BR2_DEFCONFIG) _defconfig).tar.gz"

cat <<EOF >$TARGET_DIR/etc/os-release
PRETTY_NAME="mabur GS"
NAME="mabur GS"
HOME_URL="https://github.com/gilankpam/mabur-ground"
BUG_REPORT_URL="https://github.com/gilankpam/mabur-ground/issues"
BUILD_CONFIG=$(basename $(basename $BR2_DEFCONFIG) _defconfig)
UPGRADE=$DOWNLOAD_URL
BUILD_DATE="$(date)"
VERSION="$MABUR_GS_VERSION"
EOF

cp ${O}/.config $TARGET_DIR/etc/default/br-config

AUTOMOUNT_INSERT_LINE=$(grep -n "# now run any rc scripts" $TARGET_DIR/etc/inittab| cut -d: -f1)
grep -q automount $TARGET_DIR/etc/inittab || sed -i "${AUTOMOUNT_INSERT_LINE}i # Start automount daemon\n::sysinit:/usr/sbin/automount\n" $TARGET_DIR/etc/inittab

grep -q gadget $TARGET_DIR/etc/inittab || echo '
# Start gadget
::sysinit:/usr/sbin/gadget init' >> $TARGET_DIR/etc/inittab

grep -q "framebuffer getty" $TARGET_DIR/etc/inittab || echo '
# framebuffer getty
tty1::askfirst:/sbin/getty -L tty1 0 vt100' >> $TARGET_DIR/etc/inittab

# Per-board edits to mabur's shipped defaults: board/common/maburplay-defaults.sh
# (the rationale and the boards are there).
MABURPLAY_DEFAULTS=$TARGET_DIR/usr/share/config-defaults/maburplay.toml
BOARD=$(basename $(basename $BR2_DEFCONFIG) _defconfig)
sh "$(dirname "$0")/maburplay-defaults.sh" "$BOARD" "$MABURPLAY_DEFAULTS" || exit 1
