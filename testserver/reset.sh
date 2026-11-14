#!/bin/bash
# Removes the Nextcloud test container and its data volume, then sets it up
# again. The CA, certificate and passwords in testserver/local are kept.
#
# Usage: reset.sh [image-tag]    reset.sh --remove   (remove only)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
docker rm -f nc-test >/dev/null 2>&1 || true
docker volume rm nc-test-data >/dev/null 2>&1 || true
if [ "${1:-}" = "--remove" ]; then
	echo "Test server removed."
	exit 0
fi
exec "$HERE/setup.sh" "$@"
