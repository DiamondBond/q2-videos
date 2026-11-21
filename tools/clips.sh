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

# The same with twelve chapters, 0.8 s each: a list longer than the screen shows at once
{
    echo ';FFMETADATA1'
    for i in 0 1 2 3 4 5 6 7 8 9 10 11; do
        printf '[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=Chapter %d\n' $((i * 800)) $((i * 800 + 800)) $((i + 1))
    done
} >"$T/twelve"
ff -i "$OUT/two-audio-subs.mkv" -i "$T/twelve" -map 0 -map_metadata 1 -map_chapters 1 -c copy "$OUT/chapters.mkv"

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

# 1080p stored in 10 bits: twice the memory a frame
x264 tenbit "$(pic 1920x1080 25 1)" -pix_fmt yuv420p10le
ff -i "$T/tenbit.mkv" -f lavfi -i "$(tone 440 48000 1)" -c:v copy -c:a aac -ac 2 "$OUT/tenbit.mkv"

# Every picture a keyframe, at 24 Mbit/s: nothing a decoder that is behind can skip, and the
# reader's queue full in under two seconds
x264 intra "$(pic 640x360 25 4.5),noise=alls=40:allf=t+u" -g 1 -b:v 24000k -minrate 24000k \
    -maxrate 24000k -bufsize 24000k
ff -i "$T/intra.mkv" -f lavfi -i "$(tone 440 48000 4.5)" -c:v copy -c:a aac -ac 2 "$OUT/intra.mkv"

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

# H.265 in MP4
if "$SYS" -hide_banner -encoders 2>/dev/null | grep -q libx265; then
    sys -f lavfi -i "$(pic)" -c:v libx265 -preset ultrafast -pix_fmt yuv420p -x265-params log-level=error "$T/hevc.mkv"
    ff -i "$T/hevc.mkv" -f lavfi -i "$(tone)" -c:v copy -tag:v hvc1 -c:a aac -ac 2 "$OUT/hevc.mp4"
else
    echo "hevc.mp4 skipped: $SYS has no libx265" >&2
fi

# Two minutes, small, a keyframe every two seconds: to resume in and to seek about
x264 long "$(pic 320x180 25 120)" -g 50
ff -i "$T/long.mkv" -f lavfi -i "$(tone 440 48000 120)" -c:v copy -c:a aac -ac 2 -b:a 64k \
    -movflags +faststart "$OUT/long.mp4"

# A transport stream, whose timestamps start above zero; and two of them end to end, where the
# timestamps start again half way
ff -i "$T/silent.mkv" -f lavfi -i "$(tone)" -c:v copy -c:a mp2 -ac 2 -f mpegts "$OUT/offset.ts"
cat "$OUT/offset.ts" "$OUT/offset.ts" >"$OUT/jump.ts"
# The same of forty seconds each: twice as long as its timestamps say, with room to seek about
ff -t 40 -i "$T/long.mkv" -f lavfi -i "$(tone 440 48000 40)" -c:v copy -c:a mp2 -ac 2 \
    -f mpegts "$T/part.ts"
cat "$T/part.ts" "$T/part.ts" >"$OUT/twice.ts"
# And one that is small and then 1080p: a stream that grows on the way
ff -t 3 -i "$T/long.mkv" -f lavfi -i "$(tone 440 48000 3)" -c:v copy -c:a mp2 -ac 2 \
    -f mpegts "$T/small.ts"
ff -t 2 -i "$T/big.mkv" -f lavfi -i "$(tone 440 48000 2)" -c:v copy -c:a mp2 -ac 2 \
    -f mpegts "$T/large.ts"
cat "$T/small.ts" "$T/large.ts" >"$OUT/grow.ts"

# A transport stream's timestamps are 33 bits of a 90 kHz clock: this one's run out and start
# again from nothing under three seconds in
ff -i "$T/silent.mkv" -f lavfi -i "$(tone)" -c:v copy -c:a mp2 -ac 2 -output_ts_offset 95441 \
    -f mpegts "$OUT/wrap.ts"

# A sound that stops a second in under a picture that goes on for twenty, and a picture of noise
# that takes megabytes: what is read ahead of it is held to its limit by bytes alone
x264 noisy "$(pic 640x360 25 20),noise=alls=40:allf=t+u" -b:v 3600k
ff -i "$T/noisy.mkv" -f lavfi -i "$(tone 440 48000 1)" -c:v copy -c:a aac -ac 2 "$OUT/short-sound.mkv"

# Frames that come unevenly: 25 in one second, then 5 in the next, three times (Matroska keeps
# each frame's own time)
x264 vfr "$(pic 640x360 25 6),select='lt(mod(n\,50)\,25)+not(mod(n\,5))'"
ff -i "$T/vfr.mkv" -f lavfi -i "$(tone 440 48000 6)" -c:v copy -c:a aac -ac 2 "$OUT/vfr.mkv"

# AV1, which the Q2 has no decoder for: the player must say so
if "$SYS" -hide_banner -encoders 2>/dev/null | grep -q libaom-av1; then
    sys -f lavfi -i "$(pic 320x180 25 1)" -c:v libaom-av1 -cpu-used 8 -crf 45 -b:v 0 "$T/av1.mkv"
    ff -i "$T/av1.mkv" -f lavfi -i "$(tone 440 48000 1)" -c:v copy -c:a aac -ac 2 "$OUT/av1.mkv"
else
    echo "av1.mkv skipped: $SYS has no libaom-av1" >&2
fi

# The other subtitles a file can hold as text: MP4's own, and WebVTT
ff -i "$T/sidecar.mkv" -f lavfi -i "$(tone 440 48000 10)" -i "$T/subs.srt" -c:v copy -c:a aac -ac 2 \
    -c:s mov_text -metadata:s:s:0 language=rus "$OUT/movtext.mp4"
ff -i "$T/sidecar.mkv" -f lavfi -i "$(tone 440 48000 10)" -i "$T/subs.srt" -c:v copy -c:a aac -ac 2 \
    -c:s webvtt "$OUT/webvtt.mkv"

# A song with its cover: sound, and one still picture
ff -f lavfi -i "testsrc2=size=300x300:duration=1" -frames:v 1 "$T/cover.png"
ff -f lavfi -i "$(tone)" -i "$T/cover.png" -map 0 -map 1 -c:a aac -ac 2 -c:v png \
    -disposition:v:0 attached_pic "$OUT/cover.mp4"

# every clip through 4.2's decoders, whole (but the one it has no decoder for)
for clip in "$OUT"/*.mp4 "$OUT"/*.mkv "$OUT"/*.avi "$OUT"/*.webm "$OUT"/*.ts; do
    [ -f "$clip" ] && [ "${clip##*/}" != av1.mkv ] || continue
    said=$(ff -xerror -i "$clip" -vsync vfr -f null - 2>&1) && [ -z "$said" ] ||
        { echo "$clip: FFmpeg 4.2 can't decode it: $said" >&2; exit 1; }
    printf '%-20s %8d bytes  %s\n' "${clip##*/}" "$(stat -c %s "$clip")" \
        "$("$AV/host/bin/ffprobe" -v error -show_entries format=duration:stream=codec_name \
            -of csv=p=0 "$clip" | tr '\n' ' ')"
done
touch "$OUT/.made"
