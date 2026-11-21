#!/bin/sh
# FFmpeg 4.2 for building and checking Videos on a computer: tools/ffmpeg.sh [DIR], DIR by default
# ~/.cache/q2-videos/ffmpeg-4.2. The Q2 has FFmpeg 4.2's shared libraries (libavcodec 58.54.100)
# and the app links those; a computer's FFmpeg is years newer and its API is another, so the host
# build and the checks get the Q2's release, built here:
#   DIR/host        bin/ffmpeg and bin/ffprobe, lib/, include/
#   DIR/include-q2  the same headers for the device build: avconfig.h as a little-endian MIPS has it
# Native codecs only and no assembly, as the Q2's build (no SIMD there either, so the checks decode
# through the same C the Q2 runs). Does nothing once both are there. Needs curl, cc, make and
# zlib's headers.
set -eu
VER=4.2.11
SHA256=d53f0ec8ebb8b03a9ca1efcb93f38ba3f1137b3dcc4ad3239809d511553c97f8
OUT=${1:-$HOME/.cache/q2-videos/ffmpeg-4.2}
[ -x "$OUT/host/bin/ffmpeg" ] && [ -f "$OUT/include-q2/libavutil/avconfig.h" ] && exit 0

tar=$OUT/dl/ffmpeg-$VER.tar.xz
mkdir -p "$OUT/dl"
if [ ! -f "$tar" ]; then
    curl -fL -o "$tar.part" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
    mv "$tar.part" "$tar"
fi
echo "$SHA256  $tar" | sha256sum -c - >/dev/null || { echo "$tar: not the pinned release" >&2; exit 1; }

# the source into a folder of its own, the objects into another: nothing is built in the tree
rm -rf "$OUT/src" "$OUT/build" "$OUT/host" "$OUT/include-q2"
mkdir -p "$OUT/src" "$OUT/build"
tar -xJf "$tar" -C "$OUT/src"
src=$OUT/src/ffmpeg-$VER
grep -q 'LIBAVCODEC_VERSION_MINOR *54$' "$src/libavcodec/version.h" ||
    { echo "$src: not libavcodec 58.54" >&2; exit 1; }

# -Wno-error=…: a compiler of this decade refuses what 2019's C let through
(cd "$OUT/build" && "$src/configure" --prefix="$OUT/host" --disable-static --enable-shared \
    --enable-rpath --disable-autodetect --enable-pthreads --enable-zlib --disable-gpl \
    --disable-nonfree --disable-doc --disable-ffplay --disable-x86asm --disable-inline-asm \
    --extra-cflags="-Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration" &&
    make -j"$(nproc)" && make install) >"$OUT/build.log" 2>&1 ||
    { tail -n 30 "$OUT/build.log" >&2; echo "FFmpeg $VER did not build: $OUT/build.log" >&2; exit 1; }
rm -rf "$OUT/build"

cp -r "$OUT/host/include" "$OUT/include-q2"
sed -i -e 's/^#define AV_HAVE_BIGENDIAN .*/#define AV_HAVE_BIGENDIAN 0/' \
    -e 's/^#define AV_HAVE_FAST_UNALIGNED .*/#define AV_HAVE_FAST_UNALIGNED 0/' \
    "$OUT/include-q2/libavutil/avconfig.h"
"$OUT/host/bin/ffmpeg" -hide_banner -version | head -n 1
