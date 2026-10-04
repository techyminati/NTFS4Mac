# NTFS4Mac
#
#   make          build everything into build/
#   make test     run the test suite against throwaway NTFS images
#   make clean    remove our objects (keeps the libntfs-3g build)

CC       ?= clang
ARCHS    ?= arm64 x86_64
MIN_MACOS = 15.4
NTFS3G    = build/ntfs-3g

ARCHFLAGS = $(foreach a,$(ARCHS),-arch $(a))
CFLAGS   += $(ARCHFLAGS) -mmacosx-version-min=$(MIN_MACOS) -std=gnu11 -O2 -g \
            -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
            -DHAVE_CONFIG_H -I$(NTFS3G)/include/ntfs-3g -Isrc/core -Isrc/nfs \
            -Isrc/cli
LDFLAGS  += $(ARCHFLAGS) -mmacosx-version-min=$(MIN_MACOS)
LIBS      = $(NTFS3G)/lib/libntfs-3g.a -framework CoreFoundation

CORE_SRC  = $(wildcard src/core/*.c)
CORE_OBJ  = $(patsubst src/%.c,build/obj/%.o,$(CORE_SRC))
CORE_HDR  = $(wildcard src/core/*.h)

TEST_OBJ  = build/obj/tests/enginetest.o

APP_SRC   = $(wildcard src/nfs/*.c) $(wildcard src/cli/*.c)
APP_OBJ   = $(patsubst src/%.c,build/obj/%.o,$(APP_SRC))
APP_HDR   = $(wildcard src/nfs/*.h) $(wildcard src/cli/*.h)
APP_LIBS  = $(LIBS) -framework DiskArbitration -framework IOKit

.PHONY: all test clean ntfs3g dist install uninstall

all: build/libn4m.a build/enginetest build/ntfs4mac

$(NTFS3G)/lib/libntfs-3g.a: $(wildcard patches/ntfs-3g/*.patch)
	./scripts/build-ntfs3g.sh

ntfs3g: $(NTFS3G)/lib/libntfs-3g.a

build/obj/%.o: src/%.c $(CORE_HDR) $(APP_HDR) | $(NTFS3G)/lib/libntfs-3g.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/obj/tests/%.o: tests/%.c $(CORE_HDR) | $(NTFS3G)/lib/libntfs-3g.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/libn4m.a: $(CORE_OBJ)
	@rm -f $@
	libtool -static -o $@ $^

build/enginetest: $(TEST_OBJ) build/libn4m.a $(NTFS3G)/lib/libntfs-3g.a
	$(CC) $(LDFLAGS) -o $@ $(TEST_OBJ) build/libn4m.a $(LIBS)

build/ntfs4mac: $(APP_OBJ) build/libn4m.a $(NTFS3G)/lib/libntfs-3g.a
	$(CC) $(LDFLAGS) -o $@ $(APP_OBJ) build/libn4m.a $(APP_LIBS)

test: all
	./tests/run.sh

# builds, shows the disclaimer and license, then sets up plug and play
install: build/ntfs4mac
	NTFS4MAC_FROM_SOURCE=1 ./install.sh

uninstall:
	sudo build/ntfs4mac uninstall

# release tarball for install.sh: dist/ntfs4mac-macos.tar.gz (+ .sha256)
dist: build/ntfs4mac
	@rm -rf dist/ntfs4mac
	@mkdir -p dist/ntfs4mac
	cp build/ntfs4mac README.md LICENSE dist/ntfs4mac/
	cp vendor/ntfs-3g/COPYING dist/ntfs4mac/COPYING.ntfs-3g
	tar -czf dist/ntfs4mac-macos.tar.gz -C dist ntfs4mac
	cd dist && shasum -a 256 ntfs4mac-macos.tar.gz > ntfs4mac-macos.tar.gz.sha256
	@rm -rf dist/ntfs4mac
	@echo "upload dist/ntfs4mac-macos.tar.gz and its .sha256 to a GitHub release"

clean:
	rm -rf build/obj build/libn4m.a build/enginetest build/ntfs4mac
