# AmigaOS 4 build rules, run inside the cross-compiler image by
# tools/build_os4.sh. Built with clib4 (needs clib4.library at run time).

SDK      = /opt/ppc-amigaos/ppc-amigaos/SDK
CC       = ppc-amigaos-gcc
STRIP    = ppc-amigaos-strip
CFLAGS   = -O2 -g -Wall -Wextra -Werror -std=gnu11 -mcrt=clib4 \
           -Isrc/core -Isrc/os4 -I$(SDK)/local/common/include -DCURL_STATICLIB
LDFLAGS  = -mcrt=clib4 -athread=native -L$(SDK)/local/clib4/lib
OBJCOPY  = ppc-amigaos-objcopy
LIBS     = -lcurl -lssl -lcrypto -lz -lexpat -lpthread
STRIPFLAGS = -R.comment --strip-unneeded-rel-relocs

CORE_SRC = src/core/util.c src/core/charset.c src/core/davxml.c src/core/davclient.c src/core/ncfs.c
OS4_SRC  = src/os4/main.c src/os4/platform_os4.c src/os4/requesters.c
# The filesysbox callbacks run in other tasks and must not call the C
# library, so keep the compiler from turning loops into library calls.
GLUE_SRC = src/os4/fbxglue.c
GLUE_FLAGS = -fno-builtin -fno-tree-loop-distribute-patterns

.PHONY: handler curltest

handler: build/os4/nextcloud-handler

build/os4/fbxglue.o: $(GLUE_SRC) $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) $(GLUE_FLAGS) -c -o $@ $(GLUE_SRC)

build/os4/nextcloud-handler: $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(wildcard src/core/*.h src/os4/*.h)
	$(CC) $(CFLAGS) -o $@.link $(CORE_SRC) $(OS4_SRC) build/os4/fbxglue.o $(LDFLAGS) $(LIBS)
	# AmigaOS starts a program at the symbol _start. Make that our nch_entry,
	# which runs before clib4's startup code (see src/os4/main.c).
	$(OBJCOPY) --redefine-sym _start=clib4_crt_start --redefine-sym nch_entry=_start $@.link $@.debug
	rm -f $@.link
	$(STRIP) $(STRIPFLAGS) -o $@ $@.debug

curltest: build/os4/curltest

build/os4/curltest: tests/os4/curltest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)
