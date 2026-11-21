# Videos

The video player for [Q2 Pod](https://github.com/DiamondBond/q2-pod): the videos in the card's `Videos` folder, listed in the Library and played over the whole screen.

A card app for the Shanling Q2. Unzipped at the card's root, `videos.zip` holds one folder, `.videos`.

## Building

`make` builds `dist/videos.zip` with clang and ld.lld for the Q2 (mipsel, glibc 2.28). The app decodes with the Q2's own FFmpeg 4.2 libraries and bundles none, so the build needs three things besides make, zip and unsquashfs:

- **The Q2 sysroot** in `~/.cache/q2/sysroot` (`make SYSROOT=…`).
- **FFmpeg 4.2**, the release the Q2 has (libavcodec 58.54.100). `tools/ffmpeg.sh` downloads the 4.2.11 source from ffmpeg.org, checks it against the sha256 pinned in the script, and builds it for this computer with only its own codecs and no assembly, as the Q2's is built. It leaves `~/.cache/q2-videos/ffmpeg-4.2/host` (`bin/ffmpeg`, `bin/ffprobe`, `lib/`, `include/`) for the host build and the checks, and `~/.cache/q2-videos/ffmpeg-4.2/include-q2`, the same headers set for the Q2, for the device build. It needs curl, a C compiler and zlib's headers, and does nothing once both are there. `tools/ffmpeg.sh DIR` and `make FFMPEG=DIR` use another place.
- **A Q2 Pod build's `rootfs.squashfs`** (`make ROOTFS=…`; by default the newest under `~/git/q2/q2-pod/dist`). The Makefile takes the Q2's FFmpeg libraries, and the libraries those need, from it into `build/rootfs/`, and links the app against them.

| Command | Does |
| --- | --- |
| `make` | Builds `dist/.videos/` and `dist/videos.zip`, the card app |
| `make host` | Builds `build/host/q2-videos`, the same program for this computer, against the FFmpeg 4.2 above |
| `make check` | Runs every `tests/check*.c` on this computer under ASan and UBSan, and checks `run`'s syntax |
| `make check-q2` | Starts the Q2's binary, and runs `tests/check.c` built for the Q2, under `qemu-mipsel-static` with the Q2's libraries |
| `make clean` | Removes `build/` and `dist/` |

`tools/clips.sh` makes the test clips in `~/.cache/q2-videos/clips` from ffmpeg's own test picture and tones: H.264, MPEG-4 and VP9 pictures, several sounds, subtitles inside the file and beside it, chapters, a turned picture, pixels that aren't square, 1080p and 60 frames a second. FFmpeg 4.2 has no H.264, VP9 or Opus encoder of its own, so the system's `ffmpeg` (with libx264; libvpx-vp9 and libopus for the WebM clip) encodes those and 4.2 writes the files; each clip is then decoded whole by 4.2.

The code is C11, formatted with clang-format (`.clang-format`). `src/crt-q2.c` and `src/stb_truetype.h` are kept as they came.

## Licence

GPL-3.0-only. `src/crt-q2.c` is GPL-2.0-or-later, from q2-groovebox. `src/stb_truetype.h` is stb_truetype v1.28 from [nothings/stb](https://github.com/nothings/stb) at commit `6b5799bae8a1fe3792691ef4b72ae26434c3e799`, unchanged; it is public domain or MIT, as its last lines say.
