#!/bin/sh
# Static test: nothing rewrites /etc/inittab on a normal boot. On a fresh card
# such a rewrite lands in the overlay as a new file, and a power cut before
# writeback leaves it empty -- an empty inittab hides the image's and init
# runs no init scripts at all (no WiFi, no maburgs, no maburplay).
#
#   sh firmware/tests/gadget-inittab.test.sh

cd "$(dirname "$0")/../board/common/overlay" || exit 1
fail=0
ko() { echo "FAIL: $*"; fail=1; }

g=usr/sbin/gadget
# The only inittab edit left is the guarded cleanup of an older image's line.
n=$(grep -c '/etc/inittab' $g)
[ "$n" = 2 ] || ko "$g names /etc/inittab $n times, want 2 (the guard and its sed)"
grep -q "^if grep -q '^ttyGS0' /etc/inittab; then" $g || ko "$g: inittab sed is not behind the ttyGS0 guard"
grep -q '>> */etc/inittab' $g && ko "$g still appends to /etc/inittab"
# The serial console's getty is the script's own, for as long as g1 exists.
grep -q 'while \[ -d /sys/kernel/config/usb_gadget/g1 \]' $g || ko "$g: no getty loop"
grep -q 'agetty ttyGS0' $g || ko "$g: no agetty on ttyGS0"
grep -q 'kill.*gadget-getty.pid' $g || ko "$g: teardown does not stop the getty loop"

# /init drops an empty overlay inittab so the image's applies again.
grep -q '\[ ! -s $overlay_rootdir/etc/inittab \]' init || ko "init: no empty-inittab recovery"

[ $fail = 0 ] && echo "gadget-inittab: ok"
exit $fail
