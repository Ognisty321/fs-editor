#!/bin/sh
# Generate the synthetic media used by the fs-media tests (tests/media).
# Every video frame is a flat grey whose luma encodes its frame number
# (16 + (N*7 mod 220)), so tests can check frame-exact seeking by looking
# at a single pixel. Needs ffmpeg (with libx264) and mkvmerge.
set -eu
out=$(cd "$(dirname "$0")/.." && pwd)/tests/media
mkdir -p "$out"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# 240 frames at 24000/1001, keyframe every 48 frames, plus 10 s of a 1 kHz
# tone (left) / 500 Hz (right) as FLAC so audio is sample exact.
ffmpeg -v error -y \
    -f lavfi -i "color=c=black:s=320x240:r=24000/1001:d=11,format=gray,geq=lum='16+mod(N*7\,220)',format=yuv420p" \
    -f lavfi -i "sine=f=1000:r=48000:d=10.01[l];sine=f=500:r=48000:d=10.01[r];[l][r]amerge=inputs=2" \
    -frames:v 240 -c:v libx264 -preset veryfast -crf 4 -g 48 -keyint_min 48 -sc_threshold 0 -bf 2 \
    -c:a flac -t 10.01 "$out/cfr.mkv"

# Same picture, variable frame rate: 60 frames at 24 fps, 120 at 60 fps,
# 60 at 30 fps (timecodes v2 written below and kept for comparison).
ffmpeg -v error -y -i "$out/cfr.mkv" -map 0:v -c copy "$tmp/v.h264"
{
    echo "# timecode format v2"
    awk 'BEGIN { t = 0; for (i = 0; i < 240; i++) { printf "%.6f\n", t; t += (i < 60) ? 1000/24 : (i < 180) ? 1000/60 : 1000/30 } }'
} > "$out/vfr-timecodes.txt"
# native Windows tools need Windows paths under MSYS/Git Bash
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi; }
mkvmerge -q -o "$(native "$out/vfr.mkv")" --timestamps "0:$(native "$out/vfr-timecodes.txt")" "$(native "$tmp/v.h264")"
# Embedded subtitles: an ASS and an SRT stream (no video), for extract_subtitles.
cat > "$tmp/s.ass" <<'EOF'
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 1,0:00:02.00,0:00:03.50,Default,B,0,0,0,,{\i1}Druga, z przecinkiem{\i0}
Dialogue: 0,0:00:00.50,0:00:01.25,Default,A,0,0,0,,Pierwsza\Nlinia
EOF
printf '1\n00:00:00,500 --> 00:00:01,250\nFirst\n\n2\n00:00:02,000 --> 00:00:03,500\nSecond <i>line</i>\n\n' > "$tmp/s.srt"
ffmpeg -v error -y -i "$tmp/s.ass" -i "$tmp/s.srt" -map 0 -map 1 -c copy \
    -metadata:s:0 language=pol -metadata:s:1 language=eng "$out/subs.mkv"

ls -l "$out"
