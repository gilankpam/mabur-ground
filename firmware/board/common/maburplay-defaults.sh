#!/bin/sh
# maburplay-defaults.sh <board> <maburplay.toml>: the per-board edits to
# mabur's shipped maburplay.default.toml, applied in place. Run by
# post-build-script.sh on the image's /usr/share/config-defaults copy, and by
# tests/maburplay-defaults.test.sh on the bundle itself. Exits non-zero when
# an edit does not take, so a renamed upstream block fails the build instead
# of shipping the default.
#
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

BOARD=$1
MABURPLAY_DEFAULTS=$2
[ -f "$MABURPLAY_DEFAULTS" ] || { echo "maburplay-defaults: no file $2" >&2; exit 1; }

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
runcam_wifilink)
	# The WiFiLink's four navigation buttons, as the menu's up/down/left/right
	# (the bundle ships them commented out, so the menu stays off). They read
	# high while pressed. Right (PIN_38) is also BR2_FACTORY_RESET_GPIO_PIN_NAME
	# and Left (PIN_13) BR2_GADGET_MODE_GPIO_PIN_NAME: those act only while
	# held during boot, the menu only once maburplay runs, so they coexist.
	# No select button: [input.sel] stays commented out (it is optional).
	for b in up:16 down:18 left:13 right:38; do
		n=${b%%:*}
		p=${b##*:}
		if ! grep -q "^# \[input\.$n\]\$" $MABURPLAY_DEFAULTS; then
			echo "post-build: no commented [input.$n] in $MABURPLAY_DEFAULTS -- did mabur change it?" >&2
			exit 1
		fi
		# The header and its three commented keys become the live block.
		sed -i "/^# \[input\.$n\]\$/{N;N;N;s/.*/[input.$n]\npin        = $p            # pin number on the 40-pin header\nactive_low = false         # the button reads high while pressed\nbias       = \"pull-down\"/}" $MABURPLAY_DEFAULTS
		grep -q "^\[input\.$n\]\$" $MABURPLAY_DEFAULTS || {
			echo "post-build: failed to enable [input.$n] in $MABURPLAY_DEFAULTS" >&2; exit 1; }
	done
	;;
esac
