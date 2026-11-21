#!/bin/sh
# The test clips: tools/clips.sh [DIR] [FFMPEG], DIR by default ~/.cache/q2-videos/clips and
# FFMPEG tools/ffmpeg.sh's ~/.cache/q2-videos/ffmpeg-4.2. They are made from ffmpeg's own test
# picture and tones, so nothing is downloaded. FFmpeg 4.2 decodes H.264, VP9 and Opus but has no
# encoder for them without the libraries the Q2's build leaves out, so the system's ffmpeg
# (libx264; libvpx-vp9 and libopus, or the WebM clip is skipped) encodes those and 4.2 writes
# every file. Each clip is then decoded whole by 4.2, the Q2's decoders: one it can't read fails
# here. Does nothing while DIR is newer than this script.
set -eu
OUT=${1:-$HOME/.cache/q2-videos/clips}
AV=${2:-$HOME/.cache/q2-videos/ffmpeg-4.2}
SYS=${SYS_FFMPEG:-ffmpeg}
[ "$OUT/.made" -nt "$0" ] && exit 0
[ -x "$AV/host/bin/ffmpeg" ] || { echo "No FFmpeg 4.2 in $AV: run tools/ffmpeg.sh" >&2; exit 1; }
"$SYS" -hide_banner -encoders 2>/dev/null | grep -q libx264 ||
    { echo "$SYS has no libx264: the H.264 clips need it (SYS_FFMPEG=...)" >&2; exit 1; }

mkdir -p "$OUT"
rm -f "$OUT/.made"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
ff() { "$AV/host/bin/ffmpeg" -hide_banner -v error -y "$@"; }
sys() { "$SYS" -hide_banner -v error -y "$@"; }
# x264 NAME SOURCE [ARGS]: $T/NAME.mkv, the picture alone, of ffmpeg's source SOURCE
x264() {
    name=$1 source=$2
    shift 2
    sys -f lavfi -i "$source" -c:v libx264 -preset veryfast -pix_fmt yuv420p "$@" "$T/$name.mkv"
}
pic() { echo "testsrc2=size=${1:-640x360}:rate=${2:-25}:duration=${3:-5}"; }
tone() { echo "sine=frequency=${1:-440}:sample_rate=${2:-48000}:duration=${3:-5}"; }

# H.264 and AAC (44.1 kHz) in MP4, 30 s, three chapters
cat >"$T/chapters" <<'EOF'
;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=0
END=10000
title=Opening
[CHAPTER]
TIMEBASE=1/1000
START=10000
END=20000
title=The Middle
[CHAPTER]
TIMEBASE=1/1000
START=20000
END=30000
title=Конец
EOF
x264 h264 "$(pic 640x360 25 30)"
ff -i "$T/h264.mkv" -f lavfi -i "$(tone 440 44100 30)" -i "$T/chapters" -map 0:v -map 1:a \
    -map_metadata 2 -map_chapters 2 -c:v copy -c:a aac -ac 2 -movflags +faststart "$OUT/h264-aac.mp4"

# MPEG-4 in Matroska: two sounds (eng stereo 440 Hz, jpn mono 880 Hz at 44.1 kHz), SubRip inside
cat >"$T/subs.srt" <<'EOF'
1
00:00:01,000 --> 00:00:03,000
Hello

2
00:00:04,000 --> 00:00:06,000
Привет

3
00:00:07,000 --> 00:00:09,000
こんにちは
Two lines
EOF
ff -f lavfi -i "$(pic 640x360 25 10)" -f lavfi -i "$(tone 440 48000 10)" \
    -f lavfi -i "$(tone 880 44100 10)" -i "$T/subs.srt" -map 0 -map 1 -map 2 -map 3 \
    -c:v mpeg4 -q:v 5 -c:a aac -ac:a:0 2 -ac:a:1 1 -c:s srt \
    -metadata:s:a:0 language=eng -metadata:s:a:0 title=English \
    -metadata:s:a:1 language=jpn -metadata:s:a:1 title=日本語 \
    -metadata:s:s:0 language=eng "$OUT/two-audio-subs.mkv"

# A video with its subtitles beside it. sidecar.srt is UTF-8 with a cue in Latin-1 (é and à as
# one byte each) and SubRip's tags; sidecar-bom.srt is all UTF-8, a BOM first, CRLF line ends.
x264 sidecar "$(pic 640x360 25 10)"
ff -i "$T/sidecar.mkv" -f lavfi -i "$(tone 440 48000 10)" -c:v copy -c:a aac -ac 2 "$OUT/sidecar.mp4"
printf '1\n00:00:01,000 --> 00:00:03,000\nFirst line\nSecond line\n\n' >"$OUT/sidecar.srt"
printf '2\n00:00:04,000 --> 00:00:06,000\n<i>Привет</i>, мир\n\n' >>"$OUT/sidecar.srt"
printf '3\n00:00:07,000 --> 00:00:09,000\nCaf\351 d\351j\340 vu\n' >>"$OUT/sidecar.srt"
printf '\357\273\2771\r\n00:00:01,000 --> 00:00:03,000\r\nCafé déjà vu\r\n\r\n' >"$OUT/sidecar-bom.srt"
printf '2\r\n00:00:04,000 --> 00:00:06,000\r\nこんにちは\r\n' >>"$OUT/sidecar-bom.srt"

# MPEG-4 and AC-3 in AVI
ff -f lavfi -i "$(pic)" -f lavfi -i "$(tone)" -c:v mpeg4 -vtag xvid -q:v 5 -c:a ac3 -ac 2 "$OUT/mpeg4.avi"

# VP9 and Opus in WebM
if "$SYS" -hide_banner -encoders 2>/dev/null | grep -q libvpx-vp9 &&
    "$SYS" -hide_banner -encoders 2>/dev/null | grep -q libopus; then
    sys -f lavfi -i "$(pic)" -f lavfi -i "$(tone)" -c:v libvpx-vp9 -b:v 400k -deadline realtime \
        -cpu-used 8 -c:a libopus -b:a 64k "$T/vp9.webm"
    ff -i "$T/vp9.webm" -c copy "$OUT/vp9-opus.webm"
else
    echo "vp9-opus.webm skipped: $SYS has no libvpx-vp9 or libopus" >&2
fi

# No sound; no picture
x264 silent "$(pic)"
ff -i "$T/silent.mkv" -c copy "$OUT/no-audio.mp4"
ff -f lavfi -i "$(tone)" -c:a aac -ac 2 "$OUT/audio-only.mp4"

# A phone's video held upright: the picture is stored on its side, 640 x 360, with the tag that
# turns it 90 degrees clockwise to be shown, 360 x 640 and the right way up. 4.2's ffmpeg writes
# the turn the other way round from the tag it reads back, so 270 is asked for here.
x264 rotated "$(pic 360x640),transpose=2"
ff -i "$T/rotated.mkv" -f lavfi -i "$(tone)" -c:v copy -c:a aac -ac 2 -metadata:s:v:0 rotate=270 "$OUT/rotated.mp4"
[ "$("$AV/host/bin/ffprobe" -v error -select_streams v -show_entries stream_tags=rotate -of csv=p=0 \
    "$OUT/rotated.mp4")" = 90 ] || { echo "rotated.mp4: its rotate tag is not 90" >&2; exit 1; }

# Pixels that aren't square: 720 x 576 at 16:15, shown 4:3
x264 anamorphic "$(pic 720x576),setsar=16/15"
ff -i "$T/anamorphic.mkv" -f lavfi -i "$(tone)" -c:v copy -c:a aac -ac 2 "$OUT/anamorphic.mp4"

# 1080p at 30 frames a second; 360p at 60
x264 big "$(pic 1920x1080 30)"
ff -i "$T/big.mkv" -f lavfi -i "$(tone)" -c:v copy -c:a aac -ac 2 "$OUT/big-1080p.mp4"
x264 fast "$(pic 640x360 60)"
ff -i "$T/fast.mkv" -f lavfi -i "$(tone)" -c:v copy -c:a aac -ac 2 "$OUT/60fps.mp4"

# ASS subtitles with override tags, a line break and a hard space
cat >"$T/subs.ass" <<'EOF'
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\i1}Italic{\i0} and plain
Dialogue: 0,0:00:04.00,0:00:06.00,Default,,0,0,0,,First line\NSecond line
Dialogue: 0,0:00:07.00,0:00:09.00,Default,,0,0,0,,{\an8\pos(320,40)\b1}Top{\b0}, with a comma\hand a hard space
EOF
ff -f lavfi -i "$(pic 640x360 25 10)" -f lavfi -i "$(tone 440 48000 10)" -i "$T/subs.ass" \
    -c:v mpeg4 -q:v 5 -c:a aac -ac 2 -c:s ass "$OUT/ass.mkv"

# every clip through 4.2's decoders, whole
for clip in "$OUT"/*.mp4 "$OUT"/*.mkv "$OUT"/*.avi "$OUT"/*.webm; do
    [ -f "$clip" ] || continue
    said=$(ff -xerror -i "$clip" -f null - 2>&1) && [ -z "$said" ] ||
        { echo "$clip: FFmpeg 4.2 can't decode it: $said" >&2; exit 1; }
    printf '%-20s %8d bytes  %s\n' "${clip##*/}" "$(stat -c %s "$clip")" \
        "$("$AV/host/bin/ffprobe" -v error -show_entries format=duration:stream=codec_name \
            -of csv=p=0 "$clip" | tr '\n' ' ')"
done
touch "$OUT/.made"
