#!/bin/bash
# Mounts the test server (user from testserver/local/env) with the Linux
# FUSE build, runs a command with $MNT set to the mount point, unmounts.
# Used by the deep tests to check from the host what the Amiga wrote:
#
#   tests/hostmount.sh 'build/gentree check "$MNT/tree" BIG'
#
# Public domain.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/testserver/local/env"
WORK=${WDV_TESTDIR:-/tmp/nch-hostmount}
export MNT=$WORK/mnt
mkdir -p "$MNT" "$WORK/tmp" "$WORK/kept"
WDV_PASSWORD=$NC_APP_PASS "$ROOT/build/linux/webdav-fuse" --url "$NC_HTTPS" --user "$NC_USER" \
	--cafile "$ROOT/testserver/local/ca.crt" --cache-ms 0 --tempdir "$WORK/tmp" \
	--keepdir "$WORK/kept" --verbose "$MNT" -f < /dev/null > "$WORK/fuse.log" 2>&1 &
PID=$!
for _ in $(seq 100); do
	mountpoint -q "$MNT" && break
	kill -0 $PID 2> /dev/null || { echo "mount failed, see $WORK/fuse.log"; exit 1; }
	sleep 0.1
done
set +e
bash -c "$1"
RC=$?
fusermount3 -u "$MNT"
wait $PID
exit $RC
