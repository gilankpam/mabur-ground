#!/bin/sh
# Host-side test for the drone verbs of board/common/overlay/usr/sbin/gs-upgrade.
#
#   sh firmware/tests/gs-upgrade-drone.test.sh
#
# Test mode (GS_UPGRADE_TEST=1). curl is a fake that serves $T/web/<last URL
# segment> (a missing file is a 404); maburup is a fake that logs its
# arguments, answers ident from FAKE_IDENT and prints the lines of
# FAKE_MABURUP_LINES FAKE_MABURUP_GAP seconds apart (0.05), then exits
# FAKE_MABURUP_RC (hanging first when FAKE_MABURUP_HANG=1).

cd "$(dirname "$0")/.." || exit 1
GSU=$PWD/board/common/overlay/usr/sbin/gs-upgrade

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/web"

cat >"$T/bin/curl" <<EOF
#!/bin/sh
echo "\$*" >>"$T/curl.log"
out= hdr= w= url=
while [ \$# -gt 0 ]; do
	case \$1 in
	-o) out=\$2; shift ;;
	-D) hdr=\$2; shift ;;
	-w) w=1; shift ;;
	--cacert|--connect-timeout|-m|-H|--speed-limit|--speed-time) shift ;;
	-*) ;;
	*) url=\$1 ;;
	esac
	shift
done
body=$T/web/\${url##*/}
code=200
[ -f "\$body" ] || code=404
[ -n "\$hdr" ] && printf 'HTTP/1.1 %s\r\ncontent-length: %s\r\n\r\n' "\$code" "\$(wc -c <"\$body" 2>/dev/null)" >"\$hdr"
[ -n "\$out" ] && [ "\$code" = 200 ] && cp "\$body" "\$out"
[ -n "\$w" ] && printf '%s' "\$code"
exit 0
EOF
cat >"$T/bin/maburup" <<EOF
#!/bin/sh
echo "\$*" >>"$T/up.log"
if [ "\$1" = ident ]; then
	[ -n "\$FAKE_IDENT" ] || { echo "error no drone"; exit 2; }
	echo "\$FAKE_IDENT"; exit 0
fi
trap 'echo killed >>"$T/up.log"; exit 143' TERM
while read -r l; do echo "\$l"; sleep "\${FAKE_MABURUP_GAP:-0.05}"; done <"\$FAKE_MABURUP_LINES"
[ "\$FAKE_MABURUP_HANG" = 1 ] && { sleep 5 & wait; }
exit "\${FAKE_MABURUP_RC:-0}"
EOF
chmod +x "$T"/bin/*

export GS_UPGRADE_TEST=1 GS_UPGRADE_DIR="$T/dir" GS_UPGRADE_OS_RELEASE="$T/os-release" \
	GS_UPGRADE_DRONE_API=https://api.example.invalid/repos/o/d GS_UPGRADE_ROUTE="$T/route" \
	GS_UPGRADE_MABURUP="$T/bin/maburup" PATH="$T/bin:$PATH"

pass=0
fail=0
ok() { pass=$((pass + 1)); }
ko() { fail=$((fail + 1)); echo "FAIL [$name] $*"; }
gsu() { sh "$GSU" "$@"; }
st() { sed -n "s/^$1=//p" "$T/dir/status" 2>/dev/null; }

# reset <GS version>
reset() {
	rm -rf "$T/dir" "$T"/*.log "$T/web"/* "$T/lines"
	printf 'VERSION=%s\nBUILD_CONFIG=radxa_zero3\nBUILD_EPOCH=1000\n' "$1" >"$T/os-release"
	printf 'Iface\tDestination\tGateway\nwlan0\t00000000\t0100A8C0\n' >"$T/route"
	: >"$T/lines"
	export FAKE_IDENT="version=v0.4.0 board=b1 chip=x" FAKE_MABURUP_LINES="$T/lines"
	unset FAKE_MABURUP_RC FAKE_MABURUP_HANG FAKE_MABURUP_GAP
}

# rel <tag> [board]: a release record with that board's asset.
rel() {
	printf '{"tag_name": "%s", "assets": [{"browser_download_url": "https://x/%s/mabur-%s-%s-maburd"}, {"browser_download_url": "https://x/%s/mabur-%s-%s-maburd.sha256"}]}\n' \
		"$1" "$1" "$1" "${2:-b1}" "$1" "$1" "${2:-b1}" >"$T/web/$1"
}

list() {
	printf '[' >"$T/web/releases?per_page=100"
	for t; do printf '{"tag_name": "%s"},' "$t" >>"$T/web/releases?per_page=100"; done
	printf '{}]\n' >>"$T/web/releases?per_page=100"
}

# asset <tag> [board]: the file and its checksum.
asset() {
	echo "maburd $1" >"$T/web/mabur-$1-${2:-b1}-maburd"
	sha256sum "$T/web/mabur-$1-${2:-b1}-maburd" | awk '{ print $1 "  maburd" }' >"$T/web/mabur-$1-${2:-b1}-maburd.sha256"
}

wait_end() {
	i=0
	while [ $i -lt 150 ]; do
		case $(st state) in error | cancelled | done) return 0 ;; esac
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

lock_gone() { [ ! -e "$T/dir/lock" ]; }
check_is() {
	got=$(gsu drone check "$@" 2>&1)
	if [ "$got" = "$exp" ]; then ok; else ko "got '$got'"; fi
}

name=stable_picks_newest_same_minor
reset v0.4.1
list v0.5.0 v0.4.2 v0.4.0
rel v0.4.2
exp="ok v0.4.2"
check_is v0.4.0 b1
grep -q '"v0.4.2"' "$T/dir/drone/release.json" 2>/dev/null || ko "release.json"

name=drone_newer_minor_is_gsolder
reset v0.4.1
exp="error gsolder Ground station v0.4.1 is older than drone v0.5.0. Update the ground station first."
check_is v0.5.0 b1

name=uptodate_stable
reset v0.4.1
list v0.4.2
rel v0.4.2
exp="error uptodate Drone is up to date (v0.4.2)"
check_is v0.4.2 b1

name=snapshot_same_is_uptodate
reset snapshot-abc123
rel snapshot
printf '{"assets": [{"browser_download_url": "https://x/snapshot/mabur-snapshot-9f-b1-maburd"}]}\n' >"$T/web/snapshot"
exp="error uptodate Drone is up to date (snapshot-9f)"
check_is snapshot-9f b1
exp="ok snapshot-9f"
check_is snapshot-00 b1

name=noboard
reset v0.4.1
list v0.4.2
rel v0.4.2 other
exp="error noboard No drone build for b1"
check_is v0.4.0 b1

name=norelease
reset v0.4.1
list v0.5.0
exp="error norelease No drone release for v0.4"
check_is v0.4.0 b1

name=testbuild
reset pr-12
exp="error testbuild Test build — no drone channel"
check_is v0.4.0 b1

name=nodrone_without_args
reset v0.4.1
FAKE_IDENT=
export FAKE_IDENT
exp="error nodrone No drone found"
check_is

name=ident_from_maburup
reset v0.4.1
list v0.4.2
rel v0.4.2
exp="ok v0.4.2"
check_is

# start fixtures: GS v0.4.1, asset served, fake maburup lines.
URL=https://x/v0.4.2/mabur-v0.4.2-b1-maburd
startup() {
	reset v0.4.1
	asset v0.4.2
	printf 'send 10 100\nsend 100 100\ninstall\ntrial 31\ntrial 12\n%s\n' "$1" >"$T/lines"
}

name=start_commits
startup committed
FAKE_MABURUP_GAP=0.3
export FAKE_MABURUP_GAP
[ "$(gsu drone start "$URL" v0.4.2)" = started ] || ko "start"
seen=
i=0
while [ $i -lt 150 ]; do
	case $(st state) in done) break ;; esac
	seen="$seen $(st phase):$(st done)"
	sleep 0.05
	i=$((i + 1))
done
for p in download send install trial; do case $seen in *"$p:"*) ;; *) ko "no $p in $seen" ;; esac; done
case $seen in *send:[1-9]*) ;; *) ko "no send progress: $seen" ;; esac
# The trial counts up: seconds run of 40, not seconds left.
case $seen in *trial:9*|*trial:28*) ;; *) ko "trial not counting up: $seen" ;; esac
case $seen in *trial:31*|*trial:12*) ko "trial shows seconds left: $seen" ;; esac
[ "$(st msg)" = "Updated to v0.4.2" ] || ko "msg $(st msg)"
lock_gone || ko "lock"
grep -q "install .*maburd" "$T/up.log" || ko "maburup not run"
ok

name=start_rollback_text
startup "rolled_back no_link"
FAKE_MABURUP_RC=1
export FAKE_MABURUP_RC
gsu drone start "$URL" v0.4.2 >/dev/null
wait_end
[ "$(st state)" = error ] && [ "$(st msg)" = "Rolled back: the new build never linked" ] || ko "$(st state) $(st msg)"
lock_gone || ko lock
ok

name=start_error_capitalised
startup "error lost drone"
FAKE_MABURUP_RC=1
export FAKE_MABURUP_RC
gsu drone start "$URL" v0.4.2 >/dev/null
wait_end
[ "$(st msg)" = "Lost drone" ] || ko "$(st msg)"
ok

name=start_ftp_nak_text
startup "error ftp nak 3"
FAKE_MABURUP_RC=1
export FAKE_MABURUP_RC
gsu drone start "$URL" v0.4.2 >/dev/null
wait_end
[ "$(st msg)" = "Transfer refused by the drone (3)" ] || ko "$(st msg)"
ok

name=start_died_without_final
startup "trial 10"
FAKE_MABURUP_RC=1
export FAKE_MABURUP_RC
gsu drone start "$URL" v0.4.2 >/dev/null
wait_end
[ "$(st msg)" = "The drone update stopped" ] || ko "$(st msg)"
ok

name=start_bad_checksum
startup committed
echo 0000 >"$T/web/mabur-v0.4.2-b1-maburd.sha256"
gsu drone start "$URL" v0.4.2 >/dev/null
wait_end
[ "$(st state)" = error ] && [ "$(st msg)" = "Checksum mismatch" ] || ko "$(st state) $(st msg)"
[ ! -s "$T/up.log" ] || ko "maburup ran"
lock_gone || ko lock
ok

name=start_snapshot_404
startup committed
rm "$T/web/mabur-v0.4.2-b1-maburd"
gsu drone start "$URL" v0.4.2 >/dev/null
wait_end
[ "$(st msg)" = "Snapshot changed — check again" ] || ko "$(st msg)"
ok

name=start_busy_lock
startup committed
mkdir -p "$T/dir/lock"
echo $$ >"$T/dir/lock/pid"
[ "$(gsu drone start "$URL" v0.4.2)" = "error busy An update is already running" ] || ko "busy"
ok

name=cancel_during_send
reset v0.4.1
asset v0.4.2
printf 'send 10 100\nsend 50 100\n' >"$T/lines"
FAKE_MABURUP_HANG=1
export FAKE_MABURUP_HANG
gsu drone start "$URL" v0.4.2 >/dev/null
i=0
while [ "$(st phase)" != send ] || [ "$(st done)" = 0 ]; do
	sleep 0.05
	i=$((i + 1))
	[ $i -lt 100 ] || break
done
gsu cancel
wait_end
[ "$(st state)" = cancelled ] && [ "$(st phase)" = send ] || ko "state $(st state) phase $(st phase)"
sleep 0.2
grep -q killed "$T/up.log" || ko "maburup not killed"
lock_gone || ko lock
ok

name=cancel_during_trial_ignored
startup committed
FAKE_MABURUP_GAP=0.3
export FAKE_MABURUP_GAP
gsu drone start "$URL" v0.4.2 >/dev/null
i=0
while [ "$(st phase)" != trial ]; do
	sleep 0.05
	i=$((i + 1))
	[ $i -lt 200 ] || break
done
gsu cancel
wait_end
[ "$(st state)" = done ] && [ "$(st msg)" = "Updated to v0.4.2" ] || ko "state $(st state) $(st msg)"
lock_gone || ko lock
ok

name=cancel_after_sent_ignored
reset v0.4.1
asset v0.4.2
printf 'send 50 100\nsend 100 100\ntrial 30\ncommitted\n' >"$T/lines"
FAKE_MABURUP_GAP=0.6
export FAKE_MABURUP_GAP
gsu drone start "$URL" v0.4.2 >/dev/null
i=0
while [ "$(st done)" != 100 ]; do
	sleep 0.05
	i=$((i + 1))
	[ $i -lt 200 ] || break
done
gsu cancel
wait_end
[ "$(st state)" = done ] && [ "$(st msg)" = "Updated to v0.4.2" ] || ko "state $(st state) $(st msg)"
ok

echo "gs-upgrade-drone: $pass passed, $fail failed"
[ "$fail" = 0 ]
