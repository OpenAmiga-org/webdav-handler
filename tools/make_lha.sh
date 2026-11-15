#!/bin/bash
# Puts the release together and packs it: dist/NextcloudHandler.lha
#
#   tools/make_lha.sh      (after tools/build_os4.sh)
#
# Layout:
#   NextcloudHandler/                 drawer (+ icon)
#     Install                         Installation Utility script
#     NextcloudHandler.guide          documentation
#     LICENSE.txt, Third-party.txt
#     L/nextcloud-handler
#     Devs/DOSDrivers/NC0             example DOSDriver
#   NextcloudHandler.readme           OS4Depot readme
#
# The LHA and the readme are also copied to $RELEASES (default: the shared
# folder's releases/), when that folder exists.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LHA="${LHA:-$HOME/linux_archivers/lha/lha}"
RELEASES="${RELEASES:-/media/sf_claude_folder/releases}"
STAGE="$ROOT/build/stage"
OUT="$ROOT/dist/NextcloudHandler.lha"
LIC="$ROOT/build/deps/licenses"
VH="$ROOT/src/os4/version.h"

ver=$(sed -n 's/^#define NCH_VERSION  *\([0-9]*\)/\1/p' "$VH").$(sed -n 's/^#define NCH_REVISION \([0-9]*\)/\1/p' "$VH")
amigadate=$(sed -n 's/^#define NCH_DATE *"\(.*\)"/\1/p' "$VH")
isodate=$(date +%Y%m%d)
bin="$ROOT/build/os4/nextcloud-handler"

if ! strings "$bin" | grep -c "nextcloud-handler $ver " > /dev/null; then
	echo "build/os4/nextcloud-handler is not version $ver; run tools/build_os4.sh" >&2
	exit 1
fi

rm -rf "$STAGE"
D="$STAGE/NextcloudHandler"
mkdir -p "$D/L" "$D/Devs/DOSDrivers" "$ROOT/dist"

cp "$bin" "$D/L/nextcloud-handler"
cp "$ROOT/package/NextcloudHandler/Install" "$D/"
cp "$ROOT/package/NextcloudHandler/Devs/DOSDrivers/NC0" "$D/Devs/DOSDrivers/"
sed -e "s/^@\$VER: NextcloudHandler.guide [0-9.]* ([0-9.]*)/@\$VER: NextcloudHandler.guide $ver ($amigadate)/" \
	"$ROOT/package/NextcloudHandler/NextcloudHandler.guide" > "$D/NextcloudHandler.guide"
cp "$ROOT/UNLICENSE" "$D/LICENSE.txt"
sed -e "s/@VERSION@/$ver/" -e "s/@DATE@/$isodate/" \
	"$ROOT/package/NextcloudHandler.readme" > "$STAGE/NextcloudHandler.readme"

{
	echo "nextcloud-handler is public domain (LICENSE.txt). The handler binary"
	echo "contains code from the following libraries, which are distributed under"
	echo "their own licences, reproduced here."
	echo
	echo "======================================================================"
	echo "libcurl $(sed -n 's/#define LIBCURL_VERSION "\(.*\)"/\1/p' "$ROOT/build/deps/curl-amissl/include/curl/curlver.h") - https://curl.se/"
	echo "======================================================================"
	cat "$ROOT/build/deps/curl-8.21.0/COPYING"
	echo
	echo "======================================================================"
	echo "Expat - https://libexpat.github.io/"
	echo "======================================================================"
	cat "$LIC/expat-COPYING"
	echo
	echo "======================================================================"
	echo "zlib - https://zlib.net/"
	echo "======================================================================"
	cat "$LIC/zlib-license.txt"
	echo
	echo "======================================================================"
	echo "clib4 (C library, linked parts and clib4.library) - https://github.com/AmigaLabs/clib4"
	echo "======================================================================"
	cat "$LIC/clib4-LICENSE"
	echo
	echo "======================================================================"
	echo "AmiSSL link library (libamisslstubs) - https://github.com/jens-maus/amissl"
	echo "Licensed under the Apache License, Version 2.0:"
	echo "======================================================================"
	cat "$LIC/apache-2.0.txt"
} > "$D/Third-party.txt"

# icons made by icon.library itself, see package/icons/README.md
I="$ROOT/package/icons"
cp "$I/NextcloudHandler.info" "$STAGE/"
for f in Install NextcloudHandler.guide LICENSE.txt Third-party.txt L Devs; do
	cp "$I/$f.info" "$D/$f.info"
done
cp "$I/Devs_DOSDrivers.info" "$D/Devs/DOSDrivers.info"
cp "$I/Devs_DOSDrivers_NC0.info" "$D/Devs/DOSDrivers/NC0.info"

rm -f "$OUT"
(cd "$STAGE" && "$LHA" aq2o5 "$OUT" NextcloudHandler.info NextcloudHandler.readme NextcloudHandler)
"$LHA" l "$OUT" | tail -n +1
echo "version $ver -> $OUT"
if [ -d "$RELEASES" ]; then
	cp -p "$OUT" "$STAGE/NextcloudHandler.readme" "$RELEASES/"
	echo "copied LHA and readme to $RELEASES"
else
	echo "note: $RELEASES not found, nothing copied" >&2
fi
