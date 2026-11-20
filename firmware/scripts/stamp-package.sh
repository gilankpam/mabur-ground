#!/bin/sh
# stamp-package.sh <package.tar.gz> <metadir>
#
# Rewrite an upgrade package in place with VERSION and CHANGELOG from
# <metadir> as its first two members, so a reader can stop early. The
# other members keep their names and order: an old image's sysupgrade
# still installs a stamped package. Stamping again replaces both files.
set -eu

pkg=$(readlink -f "$1")
meta=$(readlink -f "$2")
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT

mkdir "$t/x"
# Order of the existing members, minus any earlier stamp.
tar tzf "$pkg" | sed 's|^\./||' | grep -vx -e VERSION -e CHANGELOG >"$t/members"
tar xzf "$pkg" -C "$t/x"
cp "$meta/VERSION" "$meta/CHANGELOG" "$t/x/"
(cd "$t/x" && tar czf "$t/new.tar.gz" VERSION CHANGELOG $(cat "$t/members"))
mv "$t/new.tar.gz" "$pkg"
