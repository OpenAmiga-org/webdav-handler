# nextcloud-handler
#
#   make linux      Linux FUSE test build  -> build/linux/nextcloud-fuse
#   make os4        AmigaOS 4 handler       -> build/os4/nextcloud-handler
#                   (cross-compiled in the walkero/amigagccondocker image)
#   make unit       unit tests (charset)
#   make test       Linux build, unit tests, tests against the Docker test server
#   make lha        release archive         -> dist/NextcloudHandler.lha

CORE_SRC  = src/core/util.c src/core/charset.c src/core/davxml.c src/core/davclient.c src/core/ncfs.c
CORE_HDR  = $(wildcard src/core/*.h)

CC        ?= gcc
CFLAGS    ?= -O2 -g
WARN       = -Wall -Wextra -Werror -std=gnu11
PKGS       = fuse3 libcurl expat

.PHONY: all linux os4 test unit lha clean

all: linux

linux: build/linux/nextcloud-fuse

build/linux/nextcloud-fuse: $(CORE_SRC) $(CORE_HDR) src/linux/main.c src/linux/platform_linux.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -Isrc/core $(shell pkg-config --cflags $(PKGS)) \
		-o $@ $(CORE_SRC) src/linux/main.c src/linux/platform_linux.c \
		$(shell pkg-config --libs $(PKGS))

build/unit/unit_charset: tests/unit_charset.c src/core/charset.c src/core/util.c $(CORE_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -Isrc/core -o $@ tests/unit_charset.c src/core/charset.c src/core/util.c

unit: build/unit/unit_charset
	build/unit/unit_charset

os4:
	./tools/build_os4.sh

test: linux unit
	python3 -I tests/run_tests.py

lha: os4
	./tools/make_lha.sh

clean:
	rm -rf build
