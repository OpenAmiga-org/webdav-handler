#!/bin/bash
# Puts the release together and packs it: dist/WebDAVHandler.lha
#
#   tools/make_lha.sh      (after tools/build_os4.sh)
#
# Layout:
#   WebDAVHandler/                 drawer (+ icon)
#     Install                         Installation Utility script
#     WebDAVHandler.guide          documentation
#     LICENSE.txt, Third-party.txt
#     L/webdav-handler
#     Prefs/wdvprefs                  prefs program (the server list)
#     Prefs/Env-Archive/Sys/def_WDV.info   icon of the WDV: volume (Install
#                                     copies it to ENVARC:Sys and ENV:Sys)
#     Devs/DOSDrivers/WDV             DOSDriver
#   WebDAVHandler.readme           OS4Depot readme
#
# The LHA and the readme are also copied to $RELEASES (default: the shared
# folder's releases/), when that folder exists.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LHA="${LHA:-$HOME/linux_archivers/lha/lha}"
RELEASES="${RELEASES:-/media/sf_claude_folder/releases}"
STAGE="$ROOT/build/stage"
OUT="$ROOT/dist/WebDAVHandler.lha"
LIC="$ROOT/build/deps/licenses"
VH="$ROOT/src/os4/version.h"

ver=$(sed -n 's/^#define WDV_VERSION  *\([0-9]*\)/\1/p' "$VH").$(sed -n 's/^#define WDV_REVISION \([0-9]*\)/\1/p' "$VH")
amigadate=$(sed -n 's/^#define WDV_DATE *"\(.*\)"/\1/p' "$VH")
isodate=$(date +%d.%m.%Y)   # the readme date, DD.MM.YYYY
bin="$ROOT/build/os4/webdav-handler"
prefs="$ROOT/build/os4/wdvprefs"

if ! strings "$bin" | grep -c "webdav-handler $ver " > /dev/null; then
	echo "build/os4/webdav-handler is not version $ver; run tools/build_os4.sh" >&2
	exit 1
fi
if ! strings "$prefs" | grep -c "wdvprefs $ver " > /dev/null; then
	echo "build/os4/wdvprefs is not version $ver; run tools/build_os4.sh" >&2
	exit 1
fi

# every release says what changed: the readme and the guide need an entry
# for this version
if ! grep -q "^### $ver[ (]" "$ROOT/package/WebDAVHandler.readme"; then
	echo "package/WebDAVHandler.readme has no History entry \"### $ver (...)\"" >&2
	exit 1
fi
if ! grep -q "^@{b}$ver (" "$ROOT/package/WebDAVHandler/WebDAVHandler.guide"; then
	echo "WebDAVHandler.guide has no History entry for $ver" >&2
	exit 1
fi

# README.md's Changelog comes from the readme's History
python3 -I "$ROOT/tools/update_readme.py"

rm -rf "$STAGE"
D="$STAGE/WebDAVHandler"
mkdir -p "$D/L" "$D/Prefs/Env-Archive/Sys" "$D/Devs/DOSDrivers" "$ROOT/dist"

cp "$bin" "$D/L/webdav-handler"
cp "$prefs" "$D/Prefs/wdvprefs"
cp "$ROOT/package/WebDAVHandler/Install" "$D/"
cp "$ROOT/package/WebDAVHandler/Devs/DOSDrivers/WDV" "$D/Devs/DOSDrivers/"
sed -e "s/^@\$VER: WebDAVHandler.guide [0-9.]* ([0-9.]*)/@\$VER: WebDAVHandler.guide $ver ($amigadate)/" \
	"$ROOT/package/WebDAVHandler/WebDAVHandler.guide" > "$D/WebDAVHandler.guide"
cp "$ROOT/UNLICENSE" "$D/LICENSE.txt"
# the archive copy in ISO-8859-1 (OS4Depot and Amiga programs)
sed -e "s/@VERSION@/$ver/" -e "s/@DATE@/$isodate/" \
	"$ROOT/package/WebDAVHandler.readme" | iconv -f UTF-8 -t ISO-8859-1 > "$STAGE/WebDAVHandler.readme"

{
	echo "webdav-handler is public domain (LICENSE.txt). The handler binary"
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
cp "$I/WebDAVHandler.info" "$STAGE/"
for f in Install WebDAVHandler.guide LICENSE.txt Third-party.txt L Prefs Devs; do
	cp "$I/$f.info" "$D/$f.info"
done
cp "$I/Devs_DOSDrivers.info" "$D/Devs/DOSDrivers.info"
cp "$I/Devs_DOSDrivers_WDV.info" "$D/Devs/DOSDrivers/WDV.info"
cp "$I/Prefs_wdvprefs.info" "$D/Prefs/wdvprefs.info"
cp "$I/def_WDV.info" "$D/Prefs/Env-Archive/Sys/def_WDV.info"

rm -f "$OUT"
(cd "$STAGE" && "$LHA" aq2o5 "$OUT" WebDAVHandler.info WebDAVHandler.readme WebDAVHandler)
"$LHA" l "$OUT" | tail -n +1
echo "version $ver -> $OUT"
if [ -d "$RELEASES" ]; then
	cp -p "$OUT" "$STAGE/WebDAVHandler.readme" "$RELEASES/"
	echo "copied LHA and readme to $RELEASES"
else
	echo "note: $RELEASES not found, nothing copied" >&2
fi
