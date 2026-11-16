#!/bin/sh
# Host-side tests for board/common/maburplay-defaults.sh, the per-board edits
# post-build-script.sh applies to mabur's shipped maburplay.default.toml.
#
#   sh tests/maburplay-defaults.test.sh
#
# Each case runs the script on a copy of the real bundle file, so a block
# upstream renames or drops fails here before it ships as a silent no-op.

cd "$(dirname "$0")/.." || exit 1
EDIT=$PWD/board/common/maburplay-defaults.sh
BUNDLE=$PWD/../gs/player/bundle/maburplay.default.toml

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL [$name] $*"; }

# run <board>: edit a fresh copy of the bundle into $T/out.toml.
run() {
	cp "$BUNDLE" "$T/out.toml"
	sh "$EDIT" "$1" "$T/out.toml" >"$T/log" 2>&1
}

# key <section> <key>: the value of key in [section] of $T/out.toml.
key() {
	awk -v s="[$1]" -v k="$2" '
		$0 == s { f = 1; next }
		/^\[/ { f = 0 }
		f && $1 == k { sub(/^[^=]*= */, ""); sub(/ *#.*$/, ""); print; exit }
	' "$T/out.toml"
}

name="runcam_wifilink wires the four menu buttons, active-high"
run runcam_wifilink || bad "script failed: $(cat "$T/log")"
for b in up:16 down:18 left:13 right:38; do
	n=${b%%:*}
	p=${b##*:}
	[ "$(key "input.$n" pin)" = "$p" ] || bad "input.$n pin is '$(key "input.$n" pin)', want $p"
	[ "$(key "input.$n" active_low)" = "false" ] || bad "input.$n active_low is not false"
	[ "$(key "input.$n" bias)" = '"pull-down"' ] || bad "input.$n bias is not pull-down"
done
grep -q '^\[input\.sel\]' "$T/out.toml" && bad "input.sel was enabled; the WiFiLink has no select button"
[ "$(key input.rec pin)" = "32" ] || bad "input.rec was changed"
[ "$fail" -eq 0 ] && ok

name="radxa_zero3 ships the bundle untouched"
run radxa_zero3 || bad "script failed: $(cat "$T/log")"
cmp -s "$BUNDLE" "$T/out.toml" && ok || bad "file differs from the bundle"

name="emax_wyvern-link strips the record button, menu stays off"
before=$fail
run emax_wyvern-link || bad "script failed: $(cat "$T/log")"
grep -q '^\[input\.rec\]' "$T/out.toml" && bad "input.rec still present"
grep -q '^\[input\.up\]' "$T/out.toml" && bad "menu buttons enabled"
[ "$fail" -eq "$before" ] && ok

name="orangepi_zero2w switches to v4l2req and raw, strips the record button"
before=$fail
run orangepi_zero2w || bad "script failed: $(cat "$T/log")"
grep -q '^backend *= *"v4l2req"' "$T/out.toml" || bad "backend is not v4l2req"
grep -q '^\[input\.rec\]' "$T/out.toml" && bad "input.rec still present"
[ "$fail" -eq "$before" ] && ok

name="a bundle without the commented menu blocks fails the runcam edit"
before=$fail
sed '/^# \[input\.up\]/,/^$/d' "$BUNDLE" >"$T/out.toml"
if sh "$EDIT" runcam_wifilink "$T/out.toml" >"$T/log" 2>&1; then
	bad "succeeded on a bundle with no [input.up] block"
fi
[ "$fail" -eq "$before" ] && ok

echo "maburplay-defaults: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
