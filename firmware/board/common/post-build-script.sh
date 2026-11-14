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

# Per-board edits to mabur's shipped defaults.
#
# /usr/share/config-defaults/maburplay.toml is mabur's own
# maburplay.default.toml, installed verbatim by package/mabur, and package/mabur
# tracks upstream master -- so the file gains keys on its own. The boards used to
# override it with a whole forked copy under
# board/*/overlay/usr/share/config-defaults/, which went stale the moment
# upstream added anything: that is how [colortrans] = true shipped disabled on
# every image after the GPU colortrans stage landed. Patch only the lines that
# actually differ, here, and everything else upstream ships stays current.
#
# This runs after both package install and the rootfs overlays (see
# target-finalize in buildroot/Makefile), so it is the last word on the file.
# The greps are load-bearing: if upstream renames or drops a block the sed
# silently becomes a no-op, so fail the build instead of shipping the default.
MABURPLAY_DEFAULTS=$TARGET_DIR/usr/share/config-defaults/maburplay.toml
BOARD=$(basename $(basename $BR2_DEFCONFIG) _defconfig)

case "$BOARD" in
emax_wyvern-link)
	# Strip [input.rec]. Upstream puts the DVR record button on header pin 32,
	# but this board's defconfig sets BR2_FACTORY_RESET_GPIO_PIN_NAME="PIN_32"
	# -- holding that pin during boot wipes the overlay. The block's presence is
	# the whole knob (there is no enable key), so removing it is what disables
	# the button; with dvr.autostart false this board never records. Give it a
	# pin that is actually free on the Wyvern-Link and delete this case to get
	# the button back.
	if ! grep -q '^\[input\.rec\]' $MABURPLAY_DEFAULTS; then
		echo "post-build: no [input.rec] in $MABURPLAY_DEFAULTS -- did mabur drop it?" >&2
		exit 1
	fi
	# Delete the block's keys (everything from the header to the next section
	# that is not itself a section header), then comment out the empty header.
	# In this order: an [input.rec] with no keys still reads as "button on,
	# defaults" to maburplay.
	sed -i '/^\[input\.rec\]/,/^\[/{/^\[/!d}' $MABURPLAY_DEFAULTS
	sed -i 's|^\[input\.rec\]$|# [input.rec] removed by board/common/post-build-script.sh: header pin\n# 32 is the factory-reset GPIO on this board.\n|' $MABURPLAY_DEFAULTS
	if grep -q '^\[input\.rec\]' $MABURPLAY_DEFAULTS; then
		echo "post-build: failed to strip [input.rec] from $MABURPLAY_DEFAULTS" >&2
		exit 1
	fi
	;;
orangepi_zero2w)
	# Allwinner H618: maburplay decodes on Cedrus (backend "v4l2req"), which
	# has no encoder (no burned DVR); it does stream slice by slice, so
	# [decoder] stream keeps the bundle's default, and [colortrans] keeps the
	# bundle's true -- the display stage runs in the video plane's CSC
	# through kernel patch 0002's CTM property.
	# The H618's GPIO lines carry no names, so [input.rec]
	# can never resolve -- strip it as on the Wyvern-Link. Every edit is
	# checked: a silent no-op would ship a default that exits 2 at boot.
	sed -i 's|^backend *= *"mpp"|backend     = "v4l2req"|' $MABURPLAY_DEFAULTS
	sed -i '/^\[dvr\]/,/^\[/ s|^mode *= *"burned"|mode        = "raw"|' $MABURPLAY_DEFAULTS
	if ! grep -q '^\[input\.rec\]' $MABURPLAY_DEFAULTS; then
		echo "post-build: no [input.rec] in $MABURPLAY_DEFAULTS -- did mabur drop it?" >&2
		exit 1
	fi
	sed -i '/^\[input\.rec\]/,/^\[/{/^\[/!d}' $MABURPLAY_DEFAULTS
	sed -i 's|^\[input\.rec\]$|# [input.rec] removed by board/common/post-build-script.sh: the H618\n# GPIO lines have no names for the pin to resolve to.\n|' $MABURPLAY_DEFAULTS
	for want in '^backend *= *"v4l2req"' '^mode *= *"raw"' '^stream *= *true'; do
		grep -q "$want" $MABURPLAY_DEFAULTS || {
			echo "post-build: $MABURPLAY_DEFAULTS: expected $want after the orangepi edits" >&2; exit 1; }
	done
	if grep -q '^\[input\.rec\]' $MABURPLAY_DEFAULTS || grep -q '^mode *= *"burned"' $MABURPLAY_DEFAULTS; then
		echo "post-build: orangepi edits to $MABURPLAY_DEFAULTS did not take" >&2
		exit 1
	fi
	awk '/^\[colortrans\]/{f=1;next} /^\[/{f=0} f && /^enable *= *true/{ok=1} END{exit !ok}' $MABURPLAY_DEFAULTS || {
		echo "post-build: [colortrans] enable is not true in $MABURPLAY_DEFAULTS" >&2; exit 1; }
	;;
esac
