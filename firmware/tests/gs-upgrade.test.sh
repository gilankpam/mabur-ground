#!/bin/sh
# Host-side test for board/common/overlay/usr/sbin/gs-upgrade.
#
#   sh firmware/tests/gs-upgrade.test.sh
#
# Runs the tool in its test mode (GS_UPGRADE_TEST=1: no setsid, no RAM
# staging, no sysrq) against temp media, status dir, os-release and init.d.
# The init.d stubs log "<name> <verb>"; curl is a fake driven by FAKE_CURL_*
# that logs its arguments; dd logs and then runs the real dd. The "disk" is
# a plain file and its p1 a 1 MiB file of zeros.
#
# The Orange Pi cases pin the bootloader rule: the U-Boot write goes to
# sector 64 (32 KiB), the Rockchip offset, which on the H618 runs over the
# SPL at 256 KiB, so on orangepi_zero2w no bootloader is ever written,
# whatever the package carries.

cd "$(dirname "$0")/.." || exit 1
GSU=$PWD/board/common/overlay/usr/sbin/gs-upgrade

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

REAL_DD=$(command -v dd)
mkdir -p "$T/bin" "$T/slow" "$T/initd"
printf '#!/bin/sh\necho "dd $*" >>"%s/dd.log"\nexec "%s" "$@"\n' "$T" "$REAL_DD" >"$T/bin/dd"
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
[ "\${FAKE_CURL_RC:-0}" = 0 ] || exit "\$FAKE_CURL_RC"
case \$url in "\$GS_UPGRADE_API"*) body=\$FAKE_CURL_JSON ;; *) body=\$FAKE_CURL_BODY ;; esac
code=\${FAKE_CURL_HTTP:-200}
[ -n "\$hdr" ] && printf 'HTTP/1.1 %s\r\ncontent-length: %s\r\n\r\n' "\$code" "\$(wc -c <"\$body")" >"\$hdr"
[ -n "\$out" ] && [ "\$code" = 200 ] && cp "\$body" "\$out"
[ -n "\$w" ] && printf '%s' "\$code"
exit 0
EOF
# A gunzip that hangs, so a cancel lands mid-unpack.
printf '#!/bin/sh\nsleep 3\nexit 1\n' >"$T/slow/gunzip"
for s in S97maburplay S96maburgs S50crond S02klogd S01syslogd; do
	printf '#!/bin/sh\necho "%s $1" >>"%s/initd.log"\n[ "$1" = stop ] && [ "$FAIL_STOP" = %s ] && exit 1\nexit 0\n' \
		"$s" "$T" "$s" >"$T/initd/$s"
done
chmod +x "$T"/bin/* "$T"/slow/* "$T"/initd/*

export GS_UPGRADE_TEST=1 GS_UPGRADE_DIR="$T/dir" GS_UPGRADE_MEDIA="$T/media" \
	GS_UPGRADE_OS_RELEASE="$T/os-release" GS_UPGRADE_INITD="$T/initd" \
	GS_UPGRADE_API=https://api.example.invalid/repos/o/r BOOT_DISK="$T/disk" \
	PATH="$T/bin:$PATH"

pass=0
fail=0
ok() { pass=$((pass + 1)); }
ko() { fail=$((fail + 1)); echo "FAIL [$name] $*"; }

# Assignments go before "sh", never before gsu: on a function call POSIX leaves
# whether they outlive it unspecified, and bash keeps them.
gsu() { sh "$GSU" "$@"; }
st() { sed -n "s/^$1=//p" "$T/dir/status" 2>/dev/null; }

# reset <board> [build epoch]: a fresh GS with nothing run yet.
reset() {
	rm -rf "$T/dir" "$T/media" "$T"/*.log "$T/disk" "$T/zeros"
	mkdir -p "$T/media"
	printf 'VERSION=v0.1.0\nBUILD_CONFIG=%s\nBUILD_EPOCH=%s\n' "$1" "${2:-1000}" >"$T/os-release"
	head -c 1048576 /dev/zero >"$T/zeros"
	cp "$T/zeros" "$T/diskp1"
	: >"$T/disk"
	unset FAIL_STOP FAKE_CURL_RC FAKE_CURL_HTTP FAKE_CURL_BODY FAKE_CURL_JSON GS_UPGRADE_FREE_KB
}

# mkpkg <out> <board|none> <version> <uboot yes|no> [rootfs bytes] [bad]:
# board "none" builds an old package without VERSION/CHANGELOG; "bad" breaks
# the rootfs checksum.
mkpkg() {
	rm -rf "$T/p" && mkdir -p "$T/p"
	head -c "${5:-4096}" /dev/urandom >"$T/p/rootfs.squashfs"
	(cd "$T/p" && md5sum rootfs.squashfs >rootfs.squashfs.md5sum)
	[ "$6" = bad ] && echo "00000000000000000000000000000000  rootfs.squashfs" >"$T/p/rootfs.squashfs.md5sum"
	m="rootfs.squashfs rootfs.squashfs.md5sum"
	if [ "$4" = yes ]; then
		echo uboot >"$T/p/u-boot.bin"
		(cd "$T/p" && md5sum u-boot.bin >u-boot.bin.md5sum)
		m="$m u-boot.bin u-boot.bin.md5sum"
	fi
	if [ "$2" != none ]; then
		printf 'version=%s\nboard=%s\ndrone=\n' "$3" "$2" >"$T/p/VERSION"
		echo "first change" >"$T/p/CHANGELOG"
		m="VERSION CHANGELOG $m"
	fi
	# shellcheck disable=SC2086
	tar czf "$1" -C "$T/p" $m
}

# The run reached its end: a final state, written after the lock went.
wait_end() {
	i=0
	while [ $i -lt 150 ]; do
		case $(st state) in error|cancelled|rebooting) return 0 ;; esac
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

wait_phase() {
	i=0
	while [ $i -lt 100 ]; do
		[ "$(st phase)" = "$1" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

rootfs_written() { cmp -s -n "$(wc -c <"$T/p/rootfs.squashfs")" "$T/p/rootfs.squashfs" "$T/diskp1"; }
nothing_stopped() { [ ! -s "$T/initd.log" ]; }
lock_gone() { [ ! -e "$T/dir/lock" ]; }

# 1. find lists this board's packages, unversioned name first, and extracts
#    VERSION; another board's package is not listed.
name="find"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.1.5 yes
mkpkg "$T/media/mabur-ground-v0.2.0-radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
mkpkg "$T/media/orangepi_zero2w.tar.gz" orangepi_zero2w v0.2.0 no
out=$(gsu find); rc=$?
[ $rc = 0 ] && ok || ko "exit $rc"
[ "$(echo "$out" | sed -n 1p)" = "pkg 1 $T/media/radxa_zero3.tar.gz" ] && ok || ko "line 1: $out"
[ "$(echo "$out" | sed -n 2p)" = "pkg 2 $T/media/mabur-ground-v0.2.0-radxa_zero3.tar.gz" ] && ok || ko "line 2: $out"
[ "$(echo "$out" | wc -l)" = 2 ] && ok || ko "listed another board: $out"
grep -qx version=v0.1.5 "$T/dir/pkg/1/VERSION" 2>/dev/null && ok || ko "pkg 1 VERSION"
grep -qx version=v0.2.0 "$T/dir/pkg/2/VERSION" 2>/dev/null && ok || ko "pkg 2 VERSION"
grep -qx "first change" "$T/dir/pkg/2/CHANGELOG" 2>/dev/null && ok || ko "pkg 2 CHANGELOG"
[ ! -e "$T/dir/pkg/1/rootfs.squashfs" ] && ok || ko "extracted the rootfs"

name="find old package"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" none "" yes
out=$(gsu find)
[ "$out" = "pkg 1 $T/media/radxa_zero3.tar.gz" ] && ok || ko "listing: $out"
[ -d "$T/dir/pkg/1" ] && [ -z "$(ls -A "$T/dir/pkg/1")" ] && ok || ko "pkg/1 not empty: $(ls -A "$T/dir/pkg/1" 2>&1)"

name="find nothing"
reset radxa_zero3
out=$(gsu find); rc=$?
[ $rc = 0 ] && [ -z "$out" ] && ok || ko "exit $rc, out: $out"

# 2. A full install on the Radxa: U-Boot at sector 64, the rootfs on p1,
#    the package kept.
name="install radxa"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
gsu install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = rebooting ] && ok || ko "state $(st state): $(tail -3 "$T/out")"
rootfs_written && ok || ko "p1 does not hold the rootfs"
grep "of=$T/disk " "$T/dd.log" 2>/dev/null | grep -q "seek=64" && ok || ko "no u-boot dd at seek=64: $(cat "$T/dd.log" 2>&1)"
[ -f "$T/media/radxa_zero3.tar.gz" ] && ok || ko "package deleted"
grep -q "S97maburplay stop" "$T/initd.log" && grep -q "S96maburgs stop" "$T/initd.log" && ok || ko "services: $(cat "$T/initd.log")"
grep -q "^write " "$T/out" && ok || ko "no progress lines: $(tail -3 "$T/out")"

# 3. The Orange Pi never writes a bootloader, even from a package that has one.
name="opi install"
reset orangepi_zero2w
mkpkg "$T/media/orangepi_zero2w.tar.gz" orangepi_zero2w v0.2.0 yes
gsu install "$T/media/orangepi_zero2w.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = rebooting ] && ok || ko "state $(st state): $(tail -3 "$T/out")"
rootfs_written && ok || ko "p1 does not hold the rootfs"
grep -q "of=$T/disk " "$T/dd.log" 2>/dev/null && ko "bootloader written: $(cat "$T/dd.log")" || ok

name="opi start file"
reset orangepi_zero2w
mkpkg "$T/media/orangepi_zero2w.tar.gz" orangepi_zero2w v0.2.0 yes
out=$(gsu start file "$T/media/orangepi_zero2w.tar.gz")
[ "$out" = started ] && ok || ko "start said: $out"
wait_end && ok || ko "never ended: $(cat "$T/dir/status")"
[ "$(st state)" = rebooting ] && ok || ko "state $(st state) $(st msg)"
rootfs_written && ok || ko "p1 does not hold the rootfs"
grep -q "of=$T/disk " "$T/dd.log" 2>/dev/null && ko "bootloader written: $(cat "$T/dd.log")" || ok
grep -q "S97maburplay hold" "$T/initd.log" && ok || ko "maburplay not held: $(cat "$T/initd.log")"
grep -q "S97maburplay stop" "$T/initd.log" && ko "menu mode stopped maburplay" || ok

# 4. A checksum mismatch stops nothing and releases the lock.
name="md5 mismatch"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes 4096 bad
gsu start file "$T/media/radxa_zero3.tar.gz" >/dev/null
wait_end && ok || ko "never ended"
[ "$(st state)" = error ] && ok || ko "state $(st state)"
st msg | grep -qi checksum && ok || ko "msg: $(st msg)"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"
lock_gone && ok || ko "lock left"
cmp -s "$T/zeros" "$T/diskp1" && ok || ko "p1 written"

# 5. A truncated package (a copy cut short) is damaged, and stops nothing.
name="truncated"
reset radxa_zero3
mkpkg "$T/full.tar.gz" radxa_zero3 v0.2.0 yes 65536
head -c $(($(wc -c <"$T/full.tar.gz") / 2)) "$T/full.tar.gz" >"$T/media/radxa_zero3.tar.gz"
gsu install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = error ] && ok || ko "state $(st state)"
[ "$(st msg)" = "Package damaged" ] && ok || ko "msg: $(st msg)"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"
lock_gone && ok || ko "lock left"

# 6. Too little free space is refused before anything is unpacked.
name="free space"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes 65536
GS_UPGRADE_FREE_KB=10 sh "$GSU" install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = error ] && ok || ko "state $(st state)"
st msg | grep -qi space && ok || ko "msg: $(st msg)"
[ ! -e "$T/dir/unpack" ] && ok || ko "unpacked anyway"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"
lock_gone && ok || ko "lock left"

# 7. A second start while one runs is refused and leaves its status alone.
name="busy"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
mkdir -p "$T/dir/lock"
printf 'state=running\nphase=unpack\ndone=5\ntotal=10\nmsg=\n' >"$T/dir/status"
cp "$T/dir/status" "$T/status.before"
out=$(gsu start file "$T/media/radxa_zero3.tar.gz"); rc=$?
[ $rc = 1 ] && ok || ko "exit $rc"
[ "$out" = "error busy An update is already running" ] && ok || ko "out: $out"
cmp -s "$T/status.before" "$T/dir/status" && ok || ko "status changed: $(cat "$T/dir/status")"
out=$(gsu install "$T/media/radxa_zero3.tar.gz" -y); rc=$?
[ $rc = 1 ] && [ "$out" = "error busy An update is already running" ] && ok || ko "install: exit $rc, $out"

# 8. A package whose VERSION names another board is refused.
name="wrong board"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" orangepi_zero2w v0.2.0 no
gsu install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = error ] && ok || ko "state $(st state)"
[ "$(st msg)" = "Built for orangepi_zero2w" ] && ok || ko "msg: $(st msg)"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"

# 9. A rootfs that does not fit p1 is refused, and p1 is untouched.
name="rootfs too big"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes 2097152
gsu install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = error ] && ok || ko "state $(st state)"
cmp -s "$T/zeros" "$T/diskp1" && ok || ko "p1 written"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"
lock_gone && ok || ko "lock left"

# 10. check: the clock, curl failures and HTTP codes map to error codes;
#     TLS is always verified against the image's CA bundle.
name="check clock"
reset radxa_zero3 $(($(date +%s) + 86400))
out=$(gsu check stable); rc=$?
[ $rc = 1 ] && ok || ko "exit $rc"
case $out in "error clock "*) ok ;; *) ko "out: $out" ;; esac

name="check curl exit 7"
reset radxa_zero3
out=$(FAKE_CURL_RC=7 sh "$GSU" check stable); rc=$?
[ $rc = 1 ] && ok || ko "exit $rc"
case $out in "error network"*) ok ;; *) ko "out: $out" ;; esac

name="check curl exit 60"
out=$(FAKE_CURL_RC=60 sh "$GSU" check stable); rc=$?
[ $rc = 1 ] && ok || ko "exit $rc"
case $out in "error tls"*) ok ;; *) ko "out: $out" ;; esac

name="check 404"
out=$(FAKE_CURL_HTTP=404 sh "$GSU" check snapshot); rc=$?
[ $rc = 1 ] && ok || ko "exit $rc"
case $out in "error notfound"*) ok ;; *) ko "out: $out" ;; esac

name="check 403"
out=$(FAKE_CURL_HTTP=403 sh "$GSU" check stable); rc=$?
[ $rc = 1 ] && ok || ko "exit $rc"
case $out in "error http "*403*) ok ;; *) ko "out: $out" ;; esac

name="check 200"
echo '{"tag_name":"v0.2.0"}' >"$T/release.json"
out=$(FAKE_CURL_JSON="$T/release.json" sh "$GSU" check stable); rc=$?
[ $rc = 0 ] && ok || ko "exit $rc: $out"
cmp -s "$T/release.json" "$T/dir/release.json" && ok || ko "release.json not written"
grep -q "/releases/latest" "$T/curl.log" && ok || ko "stable URL: $(tail -1 "$T/curl.log")"
FAKE_CURL_JSON="$T/release.json" sh "$GSU" check snapshot >/dev/null
tail -1 "$T/curl.log" | grep -q "/releases/tags/snapshot" && ok || ko "snapshot URL: $(tail -1 "$T/curl.log")"

name="curl flags"
[ -s "$T/curl.log" ] && ok || ko "curl never called"
grep -E -e '(^| )-k( |$)' -e '--insecure' "$T/curl.log" && ko "insecure curl" || ok
[ "$(grep -c -- "--cacert /etc/ssl/certs/ca-certificates.crt" "$T/curl.log")" = "$(wc -l <"$T/curl.log")" ] &&
	ok || ko "a call without --cacert: $(cat "$T/curl.log")"

# 11. cancel during unpack: nothing stopped, lock released.
name="cancel"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
PATH="$T/slow:$PATH" sh "$GSU" start file "$T/media/radxa_zero3.tar.gz" >/dev/null
wait_phase unpack && ok || ko "never unpacked: $(cat "$T/dir/status")"
gsu cancel
wait_end && ok || ko "never ended"
[ "$(st state)" = cancelled ] && ok || ko "state $(st state)"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"
lock_gone && ok || ko "lock left"

# 12. maburgs failing to stop gives up and starts again what was stopped; a
#     logging service or crond failing to stop (its daemon already gone) does
#     not block the upgrade.
name="crond stop fails"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
FAIL_STOP=S50crond sh "$GSU" install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = rebooting ] && ok || ko "state $(st state) $(st msg)"
grep -q "S02klogd stop" "$T/initd.log" && ok || ko "stopped after crond: $(cat "$T/initd.log")"
grep -q " start" "$T/initd.log" && ko "restarted: $(cat "$T/initd.log")" || ok
rootfs_written && ok || ko "p1 does not hold the rootfs"

name="maburgs stop fails"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
FAIL_STOP=S96maburgs sh "$GSU" install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = error ] && ok || ko "state $(st state)"
grep -q "S97maburplay start" "$T/initd.log" && ok || ko "not restarted: $(cat "$T/initd.log")"
grep -q "S50crond" "$T/initd.log" && ko "kept stopping after the failure" || ok
grep -q "S97maburplay watch" "$T/initd.log" && ko "watch from ssh" || ok
cmp -s "$T/zeros" "$T/diskp1" && ok || ko "p1 written"
lock_gone && ok || ko "lock left"

name="menu maburgs stop fails"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
FAIL_STOP=S96maburgs sh "$GSU" start file "$T/media/radxa_zero3.tar.gz" >/dev/null
wait_end && ok || ko "never ended"
[ "$(st state)" = error ] && ok || ko "state $(st state)"
[ "$(tail -1 "$T/initd.log")" = "S97maburplay watch" ] && ok || ko "respawn loop not re-armed: $(cat "$T/initd.log")"
grep -q "S97maburplay start" "$T/initd.log" && ko "second player started" || ok
lock_gone && ok || ko "lock left"

# A RAM copy of busybox that cannot run is caught before the exec, with
# nothing stopped and the lock back. Outside test mode, so the staging runs;
# the probe failing is what stops it short of the write and the sysrq.
name="staged busybox broken"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" radxa_zero3 v0.2.0 yes
printf '#!/bin/sh\nexit 1\n' >"$T/badbb" && chmod +x "$T/badbb"
GS_UPGRADE_TEST='' GS_UPGRADE_BUSYBOX="$T/badbb" sh "$GSU" install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = error ] && ok || ko "state $(st state): $(tail -3 "$T/out")"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"
lock_gone && ok || ko "lock left"
[ ! -e "$T/dir/ram" ] && ok || ko "RAM copy left"
cmp -s "$T/zeros" "$T/diskp1" && ok || ko "p1 written"

# 13. Online: a package whose VERSION is not the one checked, or a 404 on
#     the download, means the snapshot moved on.
name="online version changed"
reset radxa_zero3
mkpkg "$T/dl.tar.gz" radxa_zero3 snapshot-bbbbbbbb yes
url=https://dl.example.invalid/mabur-ground-snapshot-aaaaaaaa-radxa_zero3.tar.gz
FAKE_CURL_BODY="$T/dl.tar.gz" sh "$GSU" start online "$url" snapshot-aaaaaaaa >/dev/null
wait_end && ok || ko "never ended"
[ "$(st state)" = error ] && ok || ko "state $(st state)"
[ "$(st msg)" = "Snapshot changed — check again" ] && ok || ko "msg: $(st msg)"
nothing_stopped && ok || ko "stopped: $(cat "$T/initd.log")"
grep -q -- "--cacert" "$T/curl.log" && ok || ko "download without --cacert"

name="online 404"
reset radxa_zero3
FAKE_CURL_HTTP=404 FAKE_CURL_BODY="$T/dl.tar.gz" sh "$GSU" start online "$url" snapshot-aaaaaaaa >/dev/null
wait_end && ok || ko "never ended"
[ "$(st msg)" = "Snapshot changed — check again" ] && ok || ko "msg: $(st msg)"
lock_gone && ok || ko "lock left"

# 14. An old package without VERSION still installs.
name="old package"
reset radxa_zero3
mkpkg "$T/media/radxa_zero3.tar.gz" none "" yes
gsu install "$T/media/radxa_zero3.tar.gz" -y >"$T/out" 2>&1
[ "$(st state)" = rebooting ] && ok || ko "state $(st state) $(st msg)"
rootfs_written && ok || ko "p1 does not hold the rootfs"

# install --online takes the versioned asset for this board from the release.
name="install online"
reset radxa_zero3
mkpkg "$T/dl.tar.gz" radxa_zero3 v0.2.0 yes
cat >"$T/release.json" <<'EOF'
{"tag_name":"v0.2.0","assets":[
 {"name":"radxa_zero3.tar.gz","browser_download_url":"https://dl.example.invalid/v0.2.0/radxa_zero3.tar.gz"},
 {"name":"mabur-ground-v0.2.0-orangepi_zero2w.tar.gz","browser_download_url": "https://dl.example.invalid/v0.2.0/mabur-ground-v0.2.0-orangepi_zero2w.tar.gz"},
 {"name":"mabur-ground-v0.2.0-radxa_zero3.tar.gz","browser_download_url": "https://dl.example.invalid/v0.2.0/mabur-ground-v0.2.0-radxa_zero3.tar.gz"}]}
EOF
FAKE_CURL_JSON="$T/release.json" FAKE_CURL_BODY="$T/dl.tar.gz" sh "$GSU" install --online stable -y >"$T/out" 2>&1
[ "$(st state)" = rebooting ] && ok || ko "state $(st state) $(st msg): $(tail -3 "$T/out")"
tail -1 "$T/curl.log" | grep -q "/mabur-ground-v0.2.0-radxa_zero3.tar.gz" && ok || ko "downloaded: $(tail -1 "$T/curl.log")"
rootfs_written && ok || ko "p1 does not hold the rootfs"
[ ! -e "$T/dir/download.tar.gz" ] && ok || ko "download left in RAM"

echo "gs-upgrade: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
