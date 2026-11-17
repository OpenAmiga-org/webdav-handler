#!/bin/bash
# Cross-compiles the AmigaOS 4 parts in the walkero/amigagccondocker image.
#
#   tools/build_os4.sh            handler   -> build/os4/webdav-handler
#                                 and prefs -> build/os4/wdvprefs
#   tools/build_os4.sh curltest   test tool -> build/os4/curltest
#
# Every handler build raises WDV_REVISION in src/os4/version.h and sets
# today's date.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${OS4_IMAGE:-walkero/amigagccondocker:os4-gcc15-3.6.1}"
WHAT="${1:-handler}"
VH="$ROOT/src/os4/version.h"

if [ "$WHAT" = handler ] && [ "${WDV_NO_BUMP:-}" = "" ]; then
	rev=$(sed -n 's/^#define WDV_REVISION \([0-9]*\)/\1/p' "$VH")
	sed -i "s/^#define WDV_REVISION .*/#define WDV_REVISION $((rev + 1))/" "$VH"
	sed -i "s/^#define WDV_DATE .*/#define WDV_DATE     \"$(date +%-d.%-m.%Y)\"/" "$VH"
fi

mkdir -p "$ROOT/build/os4"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/src -w /src "$IMAGE" \
	make -f tools/os4.mk "$WHAT" $([ "$WHAT" = handler ] && echo prefs) < /dev/null
if [ "$WHAT" = handler ]; then
	# (no pipe: with pipefail, strings | grep -m1 fails on SIGPIPE)
	LC_ALL=C grep -a -o -m1 '\$VER: [[:print:]]*' "$ROOT/build/os4/webdav-handler"
	LC_ALL=C grep -a -o -m1 '\$VER: [[:print:]]*' "$ROOT/build/os4/wdvprefs"
fi
