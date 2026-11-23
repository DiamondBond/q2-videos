# Videos

The video player for [Q2 Pod](https://github.com/DiamondBond/q2-pod): the videos on your card, listed with their thumbnails and played over the whole screen, with subtitles, sound tracks and chapters, each video picked up where you left it.

A card app for the Shanling Q2. Install it from Q2 Pod's App Store, or unzip `videos.zip` at the card's root (it holds one folder, `.videos`). It then appears in **Library → Apps**. It needs Q2 Pod 1.9.0 or later; on an older one, a row says so.

## Your videos

Copy videos into the `Videos` folder at the card's root, in folders of their own if you like (a series, a season). Q2 Pod's Wi-Fi transfer page takes them too.

```
Videos
├── Continue Watching    the video you left last, and how much of it is left
├── a folder             how many videos are in it
├── a video              its length, how much is left, or Watched
└── Settings
```

Centre plays a video from where you left it. A video you stopped part-way says how much is left; one played to its end says **Watched** and starts from the beginning next time. Hold **Play/Pause** on a video to mark it watched, or unwatched again.

Thumbnails are made in the background the first time Videos sees a video, while no video plays, and kept on the card. A video the Q2 can't decode has a plain tile. A folder shows its own picture when it has one, as Q2 Pod's folders do: `cover.jpg` or `folder.jpg` (or `.png`, `.webp`), any case; until it is made, and without one, the folder tile.

## Playing

| Key | Does |
| --- | --- |
| Wheel | The volume |
| Centre | Turns the wheel to seeking, and back. It goes back by itself 3 seconds after the last turn |
| Play/Pause | Pause and resume. Held: Options |
| Prev / Next | 10 seconds back or forward. Held: keeps going, faster. Next in a video's last 10 seconds ends it |
| Return | Back to the list, the place kept |

On the screen: a tap shows the controls or hides them. With them showing, a tap in the middle pauses, a tap or a drag on the bar seeks, and **⋯** opens Options. A double tap on the left or right third of the picture skips 10 seconds.

The controls show the video's name, the battery, and the bar with the time played and the time left; chapters are the gaps in the bar. Paused for a minute, the screen dims; paused for five, Videos closes and keeps the place, so the Q2 can sleep.

The sound goes where Q2 Pod's does: the headphone jack, Bluetooth or a USB DAC. If the output is lost (a headset switched off), the video pauses and says so; Play/Pause tries it again.

### Options

Hold **Play/Pause**, or tap **⋯**. The video pauses while Options is open.

| Row | Does |
| --- | --- |
| Audio | Chooses the sound track, when the video has more than one |
| Subtitles | Off, or one of the video's subtitles. Off is for every video, as the Subtitles setting is |
| Fit to Screen | On fills the screen and crops the picture's sides; Off shows all of it |
| Chapters | Jumps to a chapter |
| Start Over | Plays from the beginning |

Videos remembers the sound track and the subtitles you chose for each video.

### Subtitles

A video's own text subtitles (SubRip, ASS, MP4 text and WebVTT), and `.srt` files beside it with its name: `Film.srt`, or `Film.en.srt` and `Film.fr.srt` for several. Picture subtitles (PGS, DVD) are listed as Unavailable. A long line is made smaller rather than cut.

## Settings

| Row | Does |
| --- | --- |
| Autoplay | At a video's end, the next one in its folder starts |
| Subtitles | Shows a video's subtitles when it has them |
| Subtitle Size | Small, Medium or Large |
| Fit to Screen | On fills the screen and crops the picture's sides |
| Sort | By Name, or Recently Added: the videos copied to the card last, first |

## What plays

MP4, M4V, MOV, MKV, WebM, AVI, MPG, MPEG, WMV, FLV, TS, M2TS, 3GP and OGV files, with H.264, H.265, MPEG-4, MPEG-2, VP8, VP9, Theora, VC-1 or WMV pictures and AAC, MP3, AC-3, E-AC-3, DTS, FLAC, Opus, Vorbis or ALAC sound. Videos decodes with the FFmpeg the Q2 already has.

The Q2's processor is small and its screen is 375 by 320. A video made for it plays best, and leaves the card and the battery for music:

```
ffmpeg -i film.mkv -vf scale=-2:320 -c:v libx264 -crf 23 -c:a aac -b:a 128k film.mp4
```

- **AV1** can't be played: the Q2 has no decoder for it.
- **Large pictures** (1080p, most of all H.265 and 10-bit) may be more than the Q2 can decode in time. Videos then keeps the sound going and shows fewer pictures. A video that needs more memory than the Q2 has free isn't started, and says so.
- A list of 49 rows or more is fetched once when it opens: thumbnails made while it is on the screen show the next time you open it.
- A video whose thumbnail couldn't be made for a passing reason (too little memory free, a slow card) has the plain tile until Videos next starts, and is tried again then.
- A `.ts` made of recordings joined end to end, each starting its clock again, has every time in it twice. Videos finds a place in it by how far into the file it is, so skips in it are approximate, and its length is learned as it plays.
- Recently Added goes by the creation time the card's file system keeps; where there is none, by the file's own date, which a copied file brings with it.

## Files

On the card, in `.videos/`: `settings`, `marks` (where you are in each video and what you chose for it, for the 500 played last) and `cache/` (the thumbnails). Updates keep them. Removing the app removes `.videos/`; your videos are not touched.

## Building

`make` builds `dist/videos.zip` with clang and ld.lld for the Q2 (mipsel, glibc 2.28). The app decodes with the Q2's own FFmpeg 4.2 libraries and bundles none, so the build needs three things besides make, zip and unsquashfs:

- **The Q2 sysroot** in `~/.cache/q2/sysroot` (`make SYSROOT=…`).
- **FFmpeg 4.2**, the release the Q2 has (libavcodec 58.54.100). `tools/ffmpeg.sh` downloads the 4.2.11 source from ffmpeg.org, checks it against the sha256 pinned in the script, and builds it for this computer with only its own codecs and no assembly, as the Q2's is built. It leaves `~/.cache/q2-videos/ffmpeg-4.2/host` (`bin/ffmpeg`, `bin/ffprobe`, `lib/`, `include/`) for the host build and the checks, and `~/.cache/q2-videos/ffmpeg-4.2/include-q2`, the same headers set for the Q2, for the device build. It needs curl, a C compiler and zlib's headers, and does nothing once both are there. `tools/ffmpeg.sh DIR` and `make FFMPEG=DIR` use another place.
- **A Q2 Pod build's `rootfs.squashfs`** (`make ROOTFS=…`; by default the one under `~/git/q2/q2-pod/dist` that sorts last by version: the highest release's, or a dev build's when there is no release). The Makefile takes the Q2's FFmpeg libraries, and the libraries those need, from it into `build/rootfs/`, and links the app against them.

| Command | Does |
| --- | --- |
| `make` | Builds `dist/.videos/` and `dist/videos.zip`, the card app |
| `make host` | Builds `build/host/q2-videos`, the same program for this computer, against the FFmpeg 4.2 above |
| `make check` | Runs every `tests/check*.c` on this computer under ASan and UBSan, and checks `run`'s syntax |
| `make check-q2` | Starts the Q2's binary, and runs `tests/check.c` built for the Q2, under `qemu-mipsel-static` with the Q2's libraries |
| `make qemu-check` | Starts the Q2's `q2-videos serve` under `qemu-mipsel-static` on a card of test clips, asks it for its lists over its socket, and checks the covers the Q2's own FFmpeg made; then has the Q2's binary play clips by script there, which must show the frames the host build shows |
| `make clean` | Removes `build/` and `dist/` |

`tools/clips.sh` makes the test clips in `~/.cache/q2-videos/clips` from ffmpeg's own test picture and tones: H.264, MPEG-4 and VP9 pictures, several sounds, subtitles inside the file and beside it, chapters, a turned picture, pixels that aren't square, 1080p and 60 frames a second. FFmpeg 4.2 has no H.264, VP9 or Opus encoder of its own, so the system's `ffmpeg` (with libx264; libvpx-vp9 and libopus for the WebM clip) encodes those and 4.2 writes the files; each clip is then decoded whole by 4.2.

The code is C11, formatted with clang-format (`.clang-format`). `src/crt-q2.c` and `src/stb_truetype.h` are kept as they came.

## Licence

GPL-3.0-only. `src/crt-q2.c` is GPL-2.0-or-later, from q2-groovebox. `src/stb_truetype.h` is stb_truetype v1.28 from [nothings/stb](https://github.com/nothings/stb) at commit `6b5799bae8a1fe3792691ef4b72ae26434c3e799`, unchanged; it is public domain or MIT, as its last lines say.
