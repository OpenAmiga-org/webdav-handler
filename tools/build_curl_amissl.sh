#!/bin/bash
# Builds a static libcurl for AmigaOS 4 (clib4) that uses AmiSSL for TLS,
# in the cross-compiler image. Result: build/deps/curl-amissl/{include,lib}.
#
# The handler opens AmiSSL itself (src/os4/amissl.c). curl's configure
# links with -lamisslauto, which the AmiSSL SDK only has for newlib and
# clib2, so a tiny replacement that only defines IAmiSSL is used.
#
# NOTE (2026-11-15): the handler does not link this libcurl.a, only its
# headers; tools/os4.mk links the cross-compiler image's libcurl.a. This
# build calls bsdsocket.library directly (the program must provide
# ISocket) but is compiled with clib4's headers, and that does not work
# yet: curl ORs clib4's SOCK_NONBLOCK (O_NONBLOCK) into the socket type,
# which Roadshow refuses (EPROTONOSUPPORT); clib4's AF_INET6 is 10; curl
# waits with clib4's poll() and makes its wakeup pipe with clib4's pipe(),
# neither of which knows bsdsocket descriptors (needs ac_cv_func_poll=no
# and --disable-socketpair). Found while chasing download stalls in QEMU,
# which turned out to be the emulated network (docs/deep_test_plan.md).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${OS4_IMAGE:-walkero/amigagccondocker:os4-gcc15-3.6.1}"
DEPS="$ROOT/build/deps"
CURL_VER=8.21.0
AMISSL_VER=5.27
LHA="${LHA:-$HOME/linux_archivers/lha/lha}"

mkdir -p "$DEPS"
cd "$DEPS"
if [ ! -d "curl-$CURL_VER" ]; then
	curl -s -L -o "curl-$CURL_VER.tar.xz" "https://curl.se/download/curl-$CURL_VER.tar.xz"
	tar xf "curl-$CURL_VER.tar.xz"
fi
if [ ! -d amissl-sdk/AmiSSL ]; then
	gh release download "$AMISSL_VER" -R jens-maus/amissl -p "AmiSSL-$AMISSL_VER-SDK.lha" --clobber
	mkdir -p amissl-sdk
	(cd amissl-sdk && "$LHA" xq "../AmiSSL-$AMISSL_VER-SDK.lha" >/dev/null)
fi

rm -rf curl-build curl-amissl amissl-clib4
mkdir -p curl-build amissl-clib4
cat > amissl-clib4/amisslauto.c <<'EOF'
/* stands in for the AmiSSL SDK's libamisslauto.a, which has no clib4 build */
struct AmiSSLIFace;
struct AmiSSLIFace *IAmiSSL;
EOF

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/src -w /src/build/deps "$IMAGE" sh -ec '
	SDK=/opt/ppc-amigaos/ppc-amigaos/SDK
	AM=/src/build/deps/amissl-sdk/AmiSSL/Developer
	ppc-amigaos-gcc -mcrt=clib4 -O2 -c -o amissl-clib4/amisslauto.o amissl-clib4/amisslauto.c
	ppc-amigaos-ar rcs amissl-clib4/libamisslauto.a amissl-clib4/amisslauto.o
	cd curl-build
	../curl-'"$CURL_VER"'/configure --host=ppc-amigaos --prefix=/src/build/deps/curl-amissl \
		CC=ppc-amigaos-gcc AR=ppc-amigaos-ar RANLIB=ppc-amigaos-ranlib \
		CPPFLAGS="-I$AM/include" \
		CFLAGS="-mcrt=clib4 -O2" \
		LDFLAGS="-mcrt=clib4 -L$AM/lib/AmigaOS4 -L/src/build/deps/amissl-clib4 -L$SDK/local/clib4/lib" \
		--with-amissl --with-zlib=$SDK/local/clib4 \
		--disable-shared --enable-static --disable-threaded-resolver \
		--without-libpsl --without-brotli --without-zstd --without-nghttp2 \
		--without-libidn2 --without-librtmp --without-libssh2 \
		--disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
		--disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
		--disable-mqtt --disable-smb --disable-ftp --disable-file --disable-ipfs \
		--disable-websockets --disable-unix-sockets --disable-docs \
		--without-ca-bundle --without-ca-path > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
	grep -E "SSL:|Resolver|libz|Protocols" configure.log | head
	make -C lib -j8 > make.log 2>&1 || { tail -30 make.log; exit 1; }
	make -C lib install > /dev/null && make -C include install > /dev/null
' < /dev/null
ls -la "$DEPS/curl-amissl/lib/libcurl.a"
