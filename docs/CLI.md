# `fsed`: the command-line tool

`fsed` works directly on the FS Editor core: the same document model and the same operations the GUI uses.

```bash
cargo run --release -p fs-cli -- <command> [options]
```

## Common rules

* **Input formats** are detected from the content and the extension: ASS, SSA, SRT, WebVTT, MPL2, MicroDVD, TMP and TTXT (MPEG-4 Timed Text).
* **Encoding** is detected in this order: BOM, UTF-16 without BOM, UTF-8, and finally chardetng with a hint for Polish.
* **Load options** are accepted by the read-only commands `info`, `check`, `lines`, `fonts`, `styles` and by `convert`:
  * `--format ass|ssa|srt|mpl2|microdvd|tmp|ttxt|vtt` forces the format;
  * `--encoding` forces the encoding, e.g. `windows-1250`;
  * `--input-fps` gives the frame rate for MicroDVD files without an FPS header; the default is 23.976 (24000/1001).

  The editing commands (`shift`, `sort`, `filter`, `replace`, `resample`, `change-fps`, `postprocess`, `tl …`, `lua`) and `roundtrip` always auto-detect format and encoding.
* **Output** of commands that change the file goes:
  * to stdout (the default);
  * to a file with `-o FILE` / `--output FILE`;
  * back to the input file with `--in-place` (cannot be combined with `-o`). The file is overwritten directly; only `convert` writes atomically (temporary file, then rename).

  The input file's format, encoding and BOM are kept. A MicroDVD file is written back at the frame rate its header declares, or at 23.976 if it has none.
  * If the extension of the `-o` file names a different format than the input (e.g. `-o out.srt` for an ASS file), the command fails and points to `fsed convert`. This check covers `.ass`, `.ssa`, `.srt`, `.sub` (MicroDVD), `.ttxt` and `.vtt`.
  * If the edited text can no longer be stored in the input encoding, the file is written in an encoding that can hold it (UTF-8) and a note is printed to stderr.
* **Lossless output**: lines the command did not change are written from their original text, together with line endings, comments and unknown sections.
* **Times** can be given as:
  * `1.5s`, `-250ms`, `300` (milliseconds), `2.5` (a bare number with a decimal point means seconds);
  * `0:00:02.10`, `+0:01.00`.
* **Frame rate** is given with `--fps 23.976` or `--fps 24000/1001`. The values 23.976 (and 23.98), 29.97, 47.952 and 59.94 are turned into the exact NTSC fractions.
* **VFR** is handled with `--timecodes file.txt` (v1 and v2 formats). `--timecodes` takes precedence over `--fps`.
* **Exit codes**: 0 on success, 1 when a checking command finds problems (`check`, `roundtrip`, `fonts`) or a Lua script cancels, 2 on an error (unreadable file, bad option value and so on).

## Commands

| Command | Description |
|---|---|
| `info FILE` | Format, encoding, declared FPS, version, PlayRes, title, linked video/audio/keyframes files, sections, translation-mode progress, number of styles, dialogue lines and comments, duration, characters, max CPS, overlaps, missing styles, parser warnings. `--cps N` sets the CPS limit used for the statistics (default 15). |
| `check FILE…` | Parser warnings, missing styles, end before start, CPS above `--cps` (default 15), and overlaps between lines with the same style and layer. Exit code 1 if errors were found. With `--strict`, CPS and overlaps count as errors too. |
| `roundtrip FILE…` | Checks that loading and saving gives identical bytes. Only ASS and SSA are checked; other formats are skipped as lossy by design. Exit code 1 on any difference. |
| `lines FILE` | Table of lines, as in the grid. Takes the same filters as `filter`; `--strip` removes override tags from the displayed text. |
| `styles FILE` | List of styles. |
| `fonts FILE` | Font collector as in Aegisub: the faces used by the lines (with `\fn`, `\b`, `\i`, `\r`), the font file, missing glyphs and synthetic bold/italic (`FAUX`). `--dir DIR` searches an extra folder besides the system fonts (repeatable), `--collect DIR` copies the found files there. Exit code 1 when anything is missing. |
| `convert IN -o OUT` | Conversion. The output format comes from `--to` (same values as `--format`) or from the extension of `OUT`. Options: `--out-encoding` (default UTF-8), `--no-bom`, `--output-fps` (MicroDVD; defaults to the FPS declared by the input, else 23.976). |
| `shift FILE` | Shifts times by `--by TIME` or by `--frames N`; the latter needs `--fps` or `--timecodes`. |
| `sort FILE` | Sorts by `--by KEY`: `start` (default), `end`, `style`, `actor`, `effect`, `layer` or `text`. |
| `filter FILE` | Keeps only the matching lines and deletes the rest. |
| `replace FILE --find X --replace Y` | Find and replace. |
| `resample FILE --to 1920x1080` | Changes the script resolution, scaling styles, `\pos`, `\move`, `\org`, `\clip`, drawings, `\fs`, `\bord` and so on. The source resolution defaults to PlayResX/Y and can be set with `--from WxH`. When the aspect ratio changes, borders are added by default; `--stretch` stretches the picture instead. |
| `change-fps FILE --from A --to B` | Converts times between two constant frame rates. |
| `postprocess FILE` | Aegisub-style timing post-processor. |
| `tl status`, `tl enable`, `tl disable` | Kainote-compatible translation mode: progress (rows, translated, doubtful), turning it on and off. `disable` keeps the translations and removes the originals of translated lines. |
| `tl paste FILE TXT` | Pastes translations from a text file, one line per subtitle, turning translation mode on if needed. `--start N` sets the first row to fill (1-based, default 1). |
| `tl extract-original FILE` | Prints the original texts, one per line, for translation outside the program. |
| `frame VALUE` | Converts a time into a frame or the other way round; needs `--fps` or `--timecodes`. A frame number is prefixed with `f`, e.g. `f1500`. Prints the exact, start and end times and SMPTE for a frame, or the exact, line-start and line-end frames for a time. |
| `timecodes IN` | Converts v1 or v2 timecodes to v2, to stdout or to `-o OUT`. `--frames N` extends the output to N frames, extrapolating past the end of the file (default 0: only the frames the file lists). |
| `lua SCRIPT [FILE]` | Automation compatible with Aegisub Automation 4 (Lua and MoonScript). Without a file it lists the script's macros and export filters. With a file it runs `--macro NAME` or `--filter NAME` (a name or a number from the list) and writes the result. Built by default (feature `lua`). |
| `media …` | Video and audio commands, see [below](#media-commands). |

Command options that need more detail:

* **`shift`**:
  * `--start-only` / `--end-only` shift only one of the two times;
  * `--from-line N` limits the change to lines from number N on (1-based);
  * `--after TIME` limits it to lines starting at or after the given time (cannot be combined with `--from-line`);
  * `--styles A,B` limits it to the given styles.
* **`filter`** (and `lines`):
  * `--style`, `--actor` and `--effect` filter by those fields (comma-separated lists);
  * `--text REGEX` matches the text (case-sensitive regular expression);
  * `--comments` and `--dialogue` pick the line type;
  * `--overlapping` keeps lines that overlap another line;
  * `--min-cps N` keeps lines above N characters per second;
  * `--invert` inverts the result.
* **`replace`**:
  * `--regex` turns on regular expressions;
  * `-i` / `--ignore-case` ignores letter case;
  * `--skip-tags` replaces nothing inside `{…}`;
  * `--field text|style|actor|effect` picks the field (default `text`).
* **`postprocess`**:
  * `--lead-in` and `--lead-out` add time before and after each line;
  * `--adjacent TIME` links neighbouring lines with a gap smaller than TIME;
  * `--overlap TIME` links neighbouring lines that overlap by no more than TIME;
  * `--bias 0..1` sets where linked lines meet within the gap (0 = end of the previous line, 1 = start of the next; default 0.5);
  * `--keyframes FILE` with `--fps` or `--timecodes` snaps times to keyframes within `--snap TIME` (default 250ms) before and after each start and end.
* **`lua`**:
  * `--rows 1,4-7` selects rows (all by default), `--active N` sets the active row (by default the first selected one);
  * a macro only runs when its `validate` returns true;
  * a filter gets the default values from its configuration panel, and macro dialogs are accepted with their default button;
  * `--fps` or `--timecodes` give the script `aegisub.frame_from_ms` and `ms_from_frame`;
  * `--user DIR` points to the user directory with `automation/{autoload,include}` (default `%APPDATA%\fs-editor`, or `$XDG_CONFIG_HOME/fs-editor`, or `~/.config/fs-editor`), `--data DIR` to the directory with the bundled `automation/` tree (default: next to the executable, or the source tree in development builds);
  * `--trace-level 0..5` sets the threshold for `aegisub.debug.out` (default 3); script messages go to stderr;
  * a script cancelled with `aegisub.cancel()` changes nothing and exits with code 1;
  * scripts using DependencyControl behave as in Aegisub: missing modules from trusted DepCtrl feeds are installed into the `--user` directory (native libraries too, e.g. SubInspector), and feeds are cached in a temporary directory. A separate `--user` is a good idea for experiments.

## Examples

Delay the subtitles by 1.2 s from minute 10 on:

```bash
fsed shift ep01.ass --by 1.2s --after 0:10:00.00 --in-place
```

Shift by 3 frames for a VFR video:

```bash
fsed shift ep01.ass --frames 3 --timecodes tc.txt -o out.ass
```

Convert to SRT in CP1250:

```bash
fsed convert ep01.ass -o ep01.srt --out-encoding windows-1250 --no-bom
```

Lines above 20 CPS, without tags:

```bash
fsed lines ep01.ass --dialogue --min-cps 20 --strip
```

Translating outside the program:

```bash
fsed tl extract-original en.ass > en.txt
```

```bash
fsed tl paste en.ass pl.txt -o pl.ass
```

Karaoke templates (kara-templater) on the whole file:

```bash
fsed lua crates/fs-automation/automation/autoload/kara-templater.lua kara.ass --macro "Apply karaoke template" -o kara-fx.ass
```

## `media` commands

The `media` commands need a build with the `media` feature: `cargo build -p fs-cli --features media` (dependencies from `tools/deps`). BestSource does the decoding and libass renders the subtitles. Each file's index is stored in `%LOCALAPPDATA%\fs-editor\bsindex` or `~/.cache/fs-editor/bsindex`, so later opens are instant.

Instead of a video or audio file you can pass an Avisynth (`.avs`) or VapourSynth (`.vpy`) script: it is run by the AviSynth+ or VapourSynth installed on the computer, with no index. `media info` then shows the video and audio tracks according to what the script returns. When the library is missing, the command fails with the name of what to install. The `VSSCRIPT_PATH` variable points to a different VapourSynth installation (the path of the VSScript library).

| Command | Description |
|---|---|
| `media info FILE` | Stream list (with language, title and default flag) and video properties: resolution, pixel format, SAR, colour matrix, frame count, FPS, whether the file is VFR, duration, number of keyframes. Also the audio properties and the fonts attached to an MKV. |
| `media frame VIDEO --frame N -o OUT.png` | Saves frame N as PNG. Instead of `--frame` you can give `--time TIME`, and the frame shown at that time is saved. `--subs FILE` renders subtitles on top with libass, using the fonts attached to the MKV. |
| `media keyframes VIDEO` | Keyframe list in the Aegisub format, to stdout or to `-o FILE`. |
| `media timecodes VIDEO` | v2 timecodes from the actual frame timestamps, to stdout or to `-o FILE`. |
| `media audio-clip FILE --start A --end B -o clip.wav` | Audio excerpt as a 16-bit WAV (the equivalent of "Save audio clip" in Aegisub). `--mono` downmixes the channels to mono. |
