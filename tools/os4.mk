# AmigaOS 4 build rules, run inside the cross-compiler image by
# tools/build_os4.sh. Built with clib4 (needs clib4.library at run time).

SDK      = /opt/ppc-amigaos/ppc-amigaos/SDK
CC       = ppc-amigaos-gcc
STRIP    = ppc-amigaos-strip
# -lcurl below is the cross-compiler image's own libcurl.a 8.21.0
# (SDK/local/clib4/lib, clib4 sockets, TLS through the AmiSSL stubs):
# -mcrt=clib4 searches that directory before any -L directory. Every
# release so far was built and tested with it. The curl from
# tools/build_curl_amissl.sh only supplies the (same version) headers;
# see that script for why its library is not used.
CURLDIR  = build/deps/curl-amissl
AMISSL   = build/deps/amissl-sdk/AmiSSL/Developer
# AmiSSL 6 SDK: only for the offsets of its interface and the stand-ins
# (amissl_off_v4.c, src/os4/amissl_compat.c)
AMISSL6  = build/deps/amissl6-sdk/AmiSSL/Developer
CFLAGS   = -O2 -g -Wall -Wextra -Werror -std=gnu11 -mcrt=clib4 \
           -Isrc/core -Isrc/os4 -I$(CURLDIR)/include -I$(AMISSL)/include \
           -I$(SDK)/local/common/include -DCURL_STATICLIB
LDFLAGS  = -mcrt=clib4 -athread=native -L$(CURLDIR)/lib -L$(AMISSL)/lib/AmigaOS4 \
           -L$(SDK)/local/clib4/lib
OBJCOPY  = ppc-amigaos-objcopy
# No -lamisslstubs: its stubs have fixed AmiSSL 5 offsets. The stubs of
# build/os4/amissl_glue.c (tools/gen_amissl_glue.py, run by
# tools/build_os4.sh) fit AmiSSL 5 and 6; a function missing from
# tools/amissl_functions.txt shows up as "undefined reference".
LIBS     = -lcurl -lz -lexpat -lpthread
LIBS_STUBS = -lcurl -lamisslstubs -lz -lexpat -lpthread
AMISSL_OBJ = build/os4/amissl_glue.o build/os4/amissl_off_v3.o build/os4/amissl_off_v4.o \
             build/os4/amissl_compat.o
STRIPFLAGS = -R.comment --strip-unneeded-rel-relocs

CORE_SRC = src/core/util.c src/core/charset.c src/core/davxml.c src/core/davclient.c src/core/ncfs.c \
           src/core/servers.c src/core/ncroot.c
OS4_SRC  = src/os4/main.c src/os4/platform_os4.c src/os4/requesters.c src/os4/amissl.c
# The filesysbox callbacks run in other tasks and must not call the C
# library, so keep the compiler from turning loops into library calls.
GLUE_SRC = src/os4/fbxglue.c
GLUE_FLAGS = -fno-builtin -fno-tree-loop-distribute-patterns

# wdvprefs tests connections with the handler's own WebDAV code
PREFS_SRC = src/prefs/wdvprefs.c src/core/util.c src/core/charset.c src/core/davxml.c \
            src/core/davclient.c src/core/servers.c \
            src/os4/platform_os4.c src/os4/requesters.c src/os4/amissl.c

.PHONY: handler prefs curltest curlspeed

handler: build/os4/webdav-handler

build/os4/fbxglue.o: $(GLUE_SRC) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) $(GLUE_FLAGS) -c -o $@ $(GLUE_SRC)

build/os4/amissl_glue.o: build/os4/amissl_glue.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/os4/amissl_off_v3.o: build/os4/amissl_off_v3.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/os4/amissl_off_v4.o: build/os4/amissl_off_v4.c
	$(CC) -I$(AMISSL6)/include $(CFLAGS) -c -o $@ $<

build/os4/amissl_compat.o: src/os4/amissl_compat.c
	$(CC) -I$(AMISSL6)/include $(CFLAGS) -c -o $@ $<

build/os4/webdav-handler: $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(AMISSL_OBJ) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) -o $@.link $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(AMISSL_OBJ) $(LDFLAGS) $(LIBS)
	# AmigaOS starts a program at the symbol _start. Make that our wdv_entry,
	# which runs before clib4's startup code (see src/os4/main.c).
	$(OBJCOPY) --redefine-sym _start=clib4_crt_start --redefine-sym wdv_entry=_start $@.link $@.debug
	rm -f $@.link
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

prefs: build/os4/wdvprefs

build/os4/wdvprefs: $(PREFS_SRC) $(AMISSL_OBJ) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) -o $@.debug $(PREFS_SRC) $(AMISSL_OBJ) $(LDFLAGS) $(LIBS)
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

curltest: build/os4/curltest

build/os4/curltest: tests/os4/curltest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lcurl -lssl -lcrypto -lz -lpthread

curlspeed: build/os4/curlspeed

build/os4/curlspeed: tests/os4/curlspeed.c src/os4/amissl.c $(AMISSL_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)
