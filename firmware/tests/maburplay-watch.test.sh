#!/bin/sh
# Host-side test for gs/player/bundle/S97maburplay's `watch` verb: it
# re-arms the respawn loop around a player that is still running, after
# `hold` took the loop down.
#
#   sh firmware/tests/maburplay-watch.test.sh
#
# Runs a copy with the player and log paths redirected into a temp dir,
# executed directly through a busybox sh shebang, as init.d scripts are on
# the GS: busybox pidof matches a script by its name, and the host's pidof
# may not.

cd "$(dirname "$0")/../.." || exit 1
SRC=$PWD/gs/player/bundle/S97maburplay
BB=$(command -v busybox) || { echo "SKIP: busybox not on PATH"; exit 0; }

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bb" "$T/bin"
for a in sh pidof killall sleep kill; do ln -s "$BB" "$T/bb/$a"; done
sed -e "1s|.*|#!$T/bb/sh|" -e "s|/usr/local/bin/maburplay|$T/bin/maburplay|" \
	-e "s|/tmp/maburplay.log|$T/mp.log|g" "$SRC" >"$T/S97maburplay"
# The player: HELD=1 plays the one gs-upgrade left running (it exits on its
# own); otherwise it logs a run and exits 2, which ends the loop.
cat >"$T/bin/maburplay" <<EOF
#!$T/bb/sh
if [ -n "\$HELD" ]; then sleep 1; echo held-exit >>"$T/runs"; exit 0; fi
echo run >>"$T/runs"
[ -n "\$BLOCK" ] && sleep 2
exit 2
EOF
chmod +x "$T/S97maburplay" "$T/bin/maburplay"
export PATH="$T/bb:$PATH"

pass=0
fail=0
ok() { pass=$((pass + 1)); }
ko() { fail=$((fail + 1)); echo "FAIL [$name] $*"; }

# runs_settle <n>: wait up to 6 s for n lines in the run log.
runs_settle() {
	i=0
	while [ "$(cat "$T/runs" 2>/dev/null | wc -l)" -lt "$1" ] && [ $i -lt 60 ]; do
		sleep 0.1
		i=$((i + 1))
	done
}

name="watch waits, then respawns"
rm -f "$T/runs"
HELD=1 "$T/bin/maburplay" &
sleep 0.2
"$T/S97maburplay" watch
sleep 0.3
[ ! -e "$T/runs" ] && ok || ko "respawned while the player ran: $(cat "$T/runs")"
runs_settle 2
[ "$(cat "$T/runs" 2>/dev/null | tr '\n' ' ')" = "held-exit run " ] && ok || ko "runs: $(cat "$T/runs" 2>&1)"
grep -q "maburplay exited: 2, skip restart" "$T/mp.log" && ok || ko "log: $(cat "$T/mp.log")"
wait

name="watch with a loop already running"
rm -f "$T/runs"
BLOCK=1 "$T/S97maburplay" start
sleep 0.3
"$T/S97maburplay" watch
sleep 3.5
[ "$(cat "$T/runs" 2>/dev/null | wc -l)" = 1 ] && ok || ko "a second loop ran: $(cat "$T/runs" 2>&1)"

echo "maburplay-watch: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
