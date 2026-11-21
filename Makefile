# Videos for the Shanling Q2. `make` builds the card app into dist/.videos/ and dist/videos.zip
# (needs the Q2 sysroot, FFmpeg 4.2's headers from tools/ffmpeg.sh and a Q2 Pod rootfs for the
# Q2's own FFmpeg libraries, see README); `make host` builds it for this computer against the
# FFmpeg 4.2 tools/ffmpeg.sh built, `make check` runs the checks there under ASan and UBSan, and
# `make check-q2` starts the Q2's binary under qemu against the Q2's libraries.
SYSROOT ?= $(HOME)/.cache/q2/sysroot
ROOTFS ?= $(lastword $(sort $(wildcard $(HOME)/git/q2/q2-pod/dist/*/rootfs.squashfs \
                                       $(HOME)/git/q2/q2-pod/dist/*/build/rootfs.squashfs)))
FFMPEG ?= $(HOME)/.cache/q2-videos/ffmpeg-4.2
VERSION := $(shell cat VERSION)
CC_Q2 = clang -D_GNU_SOURCE --target=mipsel-linux-gnu --sysroot=$(SYSROOT) -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing -DVERSION='"$(VERSION)"'
LFS = -D_FILE_OFFSET_BITS=64 # videos over 2 GiB; crt-q2.c is built without it, as it wraps both
LDFLAGS_Q2 = -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 --image-base=0x400000 -z noexecstack \
             --hash-style=sysv --build-id=none -s
HOST_CFLAGS = -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DHOST -fsanitize=address,undefined \
              -DVERSION='"$(VERSION)"' -I$(FFMPEG)/host/include
HOST_LIBS = -L$(FFMPEG)/host/lib -Wl,-rpath,$(FFMPEG)/host/lib -lavformat -lavcodec -lswscale -lswresample -lavutil -lm -lpthread

# The sources, in lists of their own so that the player and the server grow on different lines.
# What both use:
COMMON_SRC = src/main.c src/common.c src/store.c src/library.c

# The full-screen player (player_main):
PLAYER_SRC =

# The page's server (serve_main):
SERVE_SRC =

# The Q2's build alone (the start file, and what needs the Q2: its screen, ALSA):
Q2_ONLY_SRC = src/crt-q2.c

# The host build alone (what stands in for the Q2 there):
HOST_ONLY_SRC =

# Weak stand-ins for serve_main and player_main until both are written; then delete it and this.
STUB_SRC = src/stubs.c

# The rows' static covers, JPEGs: copied beside the binary and packed into the zip.
TILES =

SRC = $(COMMON_SRC) $(PLAYER_SRC) $(SERVE_SRC)
Q2_OBJ = $(patsubst src/%.c,build/q2/%.o,$(SRC) $(Q2_ONLY_SRC) $(STUB_SRC))
HOST_OBJ = $(patsubst src/%.c,build/host/%.o,$(SRC) $(HOST_ONLY_SRC) $(STUB_SRC))
HDR = $(wildcard src/*.h) VERSION
DIST = dist/.videos
# The Q2's own FFmpeg, linked as it has it, and what that needs in turn.
AVLIBS = $(addprefix build/rootfs/usr/lib/,libavformat.so.58 libavcodec.so.58 libswscale.so.5 libswresample.so.3 libavutil.so.56)
AVDEPS = $(addprefix build/rootfs/usr/lib/,libz.so.1 libssl.so.1.1 libcrypto.so.1.1)
# The libraries after --as-needed are the Q2's FFmpeg's own: named so the link is checked whole,
# and left out of the binary's NEEDED, since the app calls none of them.
Q2_LIBS = $(AVLIBS) $(SYSROOT)/lib/libasound.so.2 $(SYSROOT)/lib/libc.so.6 $(SYSROOT)/lib/libm.so.6 \
          $(SYSROOT)/lib/libpthread.so.0 $(SYSROOT)/lib/libgcc_s.so.1 --as-needed $(AVDEPS) $(SYSROOT)/lib/libdl.so.2 \
          --no-as-needed $(SYSROOT)/lib/ld-linux-mipsn8.so.1
# Each tests/check*.c is a program of its own, linked with everything but main.c.
CHECKS = $(patsubst tests/%.c,build/host/%,$(wildcard tests/check*.c))
# The Q2's binary on this computer: qemu's user mode with a CPU that has the Q2's NaNs and FPU,
# over the Q2's libraries, every symbol bound at the start so that one the Q2 lacks fails here.
QEMU = qemu-mipsel-static -cpu P5600 -L $(CURDIR)/build/q2root -E LD_BIND_NOW=1

all: $(DIST)/q2-videos $(DIST)/q2app $(DIST)/run dist/videos.zip

$(AVLIBS) $(AVDEPS) &:
	@test -f "$(ROOTFS)" || { echo "No rootfs: make ROOTFS=path/to/rootfs.squashfs (a Q2 Pod build's)"; exit 1; }
	unsquashfs -q -f -d build/rootfs "$(ROOTFS)" usr/lib/libavformat.so.58 usr/lib/libavformat.so.58.29.100 \
	    usr/lib/libavcodec.so.58 usr/lib/libavcodec.so.58.54.100 usr/lib/libavutil.so.56 usr/lib/libavutil.so.56.31.100 \
	    usr/lib/libswscale.so.5 usr/lib/libswscale.so.5.5.100 usr/lib/libswresample.so.3 usr/lib/libswresample.so.3.5.100 \
	    usr/lib/libz.so.1 usr/lib/libz.so.1.2.11 usr/lib/libssl.so.1.1 usr/lib/libcrypto.so.1.1 >/dev/null
$(FFMPEG)/include-q2/libavcodec/avcodec.h $(FFMPEG)/host/lib/libavcodec.so:
	@echo "No FFmpeg 4.2 in $(FFMPEG): run tools/ffmpeg.sh"; exit 1

build/q2/crt-q2.o: src/crt-q2.c
	@mkdir -p build/q2
	$(CC_Q2) $(CFLAGS) -c $< -o $@
build/q2/%.o: src/%.c $(HDR) | $(FFMPEG)/include-q2/libavcodec/avcodec.h
	@mkdir -p build/q2
	$(CC_Q2) $(CFLAGS) $(LFS) -I$(FFMPEG)/include-q2 -c $< -o $@
$(DIST)/q2-videos: $(Q2_OBJ) $(AVLIBS) $(AVDEPS)
	@mkdir -p $(DIST)
	ld.lld $(LDFLAGS_Q2) -o $@ $(Q2_OBJ) $(Q2_LIBS)
$(DIST)/q2app: q2app
	@mkdir -p $(DIST)
	cp $< $@
$(DIST)/run: run
	@mkdir -p $(DIST)
	cp $< $@
	chmod 755 $@
dist/videos.zip: $(DIST)/q2-videos $(DIST)/q2app $(DIST)/run $(TILES)
	$(if $(TILES),cp $(TILES) $(DIST)/)
	cd dist && rm -f videos.zip && zip -qX videos.zip $(addprefix .videos/,q2app run q2-videos $(notdir $(TILES)))
	@ls -l $@

# The host build: the app itself, and the checks.
host: build/host/q2-videos
build/host/%.o: src/%.c $(HDR) | $(FFMPEG)/host/lib/libavcodec.so
	@mkdir -p build/host
	cc $(HOST_CFLAGS) -c $< -o $@
build/host/q2-videos: $(HOST_OBJ)
	cc $(HOST_CFLAGS) -o $@ $(HOST_OBJ) $(HOST_LIBS)
build/host/%: tests/%.c $(filter-out build/host/main.o,$(HOST_OBJ)) $(HDR)
	cc $(HOST_CFLAGS) -Isrc -o $@ $< $(filter-out build/host/main.o,$(HOST_OBJ)) $(HOST_LIBS)
check: $(CHECKS) build/host/q2-videos
	sh -n run
	@set -e; for c in $(CHECKS); do echo ./$$c; ./$$c; done

# Under $(QEMU): the Q2's binary loads the Q2's FFmpeg and ALSA and says its version, and
# tests/check.c, the shared code's checks, runs as the Q2 runs it (32 bits, its libraries).
build/q2root: $(AVLIBS) $(AVDEPS)
	@mkdir -p build/q2root/usr
	ln -sfn $(SYSROOT)/lib build/q2root/lib
	ln -sfn $(CURDIR)/build/rootfs/usr/lib build/q2root/usr/lib
build/q2/check: tests/check.c $(filter-out build/q2/main.o,$(Q2_OBJ)) $(AVLIBS) $(AVDEPS)
	$(CC_Q2) $(CFLAGS) $(LFS) -I$(FFMPEG)/include-q2 -Isrc -c $< -o $@.o
	ld.lld $(LDFLAGS_Q2) -o $@ $@.o $(filter-out build/q2/main.o,$(Q2_OBJ)) $(Q2_LIBS)
check-q2: $(DIST)/q2-videos build/q2/check build/q2root
	$(QEMU) $(DIST)/q2-videos --version
	cd build/q2 && $(QEMU) ./check # in there: its build/check-card is not the host check's

clean:
	rm -rf build dist
.PHONY: all host check check-q2 clean
