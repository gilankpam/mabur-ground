#!/bin/sh
# Stand-in for gs-upgrade: answers each verb from env vars and writes the
# files the real tool does under $GS_UPGRADE_DIR.
#   FAKE_SLEEP        seconds to sleep before find/check answer
#   FAKE_FIND_N       packages to report (each with VERSION and CHANGELOG)
#   FAKE_CHECK_ERR    "<code> <text>": check fails with that error line
#   FAKE_RELEASE      file copied to release.json on a good check
#   FAKE_START_ERR    "<code> <text>": start fails with that error line;
#                     ECHO_DIR: the error text is the GS_UPGRADE_DIR it was given
D=$GS_UPGRADE_DIR
mkdir -p "$D"
[ -z "$FAKE_SLEEP" ] || sleep "$FAKE_SLEEP"
case $1 in
find)
	n=0
	while [ "$n" -lt "${FAKE_FIND_N:-0}" ]; do
		n=$((n + 1))
		mkdir -p "$D/pkg/$n"
		printf 'version=v0.%s.0\nboard=radxa_zero3\ndrone=v0.%s.0\n' "$n" "$n" >"$D/pkg/$n/VERSION"
		printf -- '- change one of %s\n- change two\n' "$n" >"$D/pkg/$n/CHANGELOG"
		echo "pkg $n /media/dvr/pkg$n.tar.gz"
	done ;;
check)
	if [ -n "$FAKE_CHECK_ERR" ]; then echo "error $FAKE_CHECK_ERR"; exit 1; fi
	cp "$FAKE_RELEASE" "$D/release.json" ;;
start)
	if [ "$FAKE_START_ERR" = ECHO_DIR ]; then echo "error busy $GS_UPGRADE_DIR"; exit 1; fi
	if [ -n "$FAKE_START_ERR" ]; then echo "error $FAKE_START_ERR"; exit 1; fi
	printf 'state=running\nphase=unpack\ndone=0\ntotal=0\nmsg=\n' >"$D/status"
	echo started ;;
cancel) touch "$D/cancel" ;;
esac
