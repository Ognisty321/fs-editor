# FS Editor architecture

FS Editor is a cross-platform ASS/SSA subtitle editor for Windows and Linux; the code is written with macOS in mind, but it is not built there yet.

* **The core** is written in Rust (2024 edition): the document model, the lossless ASS parser and writer, formats, undo, VFR timecodes and operations.
* **The interface** is Qt 6 Widgets (C++23, Qt ≥ 6.8). Video and audio graphs are drawn on the GPU through QRhi (`QRhiWidget`).
* **The bridge** between Rust and C++ uses `cxx`. Rust builds a static library, and CMake links it through Corrosion.
* **Media**: BestSource decodes video and audio frame-accurately, libass renders subtitles, `cpal` plays audio, and `realfft` computes the spectrogram.
* **Automation** is LuaJIT through `mlua`, with an API compatible with Aegisub Automation 4 (karaskel, DependencyControl, MoonScript).
* **The UI layout** follows Kainote, while **the API and features** are modelled on Aegisub.
* **License**: BSD-3-Clause.

## 1. Principles

1. **The core knows nothing about the GUI.** All editing logic (line operations, time shifting, filtering, undo) lives in Rust and is testable from the CLI and from tests. The GUI only displays state and sends commands.
2. **Losslessness.** Opening a file and saving it unchanged produces identical bytes: the BOM, line endings, `;` comments, unknown sections, `[Fonts]`/`[Graphics]`, `[Aegisub Project Garbage]`, `[Aegisub Extradata]`, the order of keys and of `Format:` fields, and whitespace are all preserved. A line nobody touched is written from its original text.
3. **Time is an integer.** Time is an `i64` in milliseconds; ASS stores centiseconds. VFR video is handled through a table of frame timestamps with Aegisub semantics (`frame_at_time`, `time_at_frame` with Start/End/Exact modes).
4. **FFI boundaries are narrow.** `cxx` exports a few opaque types (`Project`, `Document`, `AudioEngine`, `VideoSource`, `Automation`) and plain DTO structs, not the whole model.
5. **Dependencies are pinned.** Rust uses `Cargo.lock` (committed), C++ uses `vcpkg.json` with the registry baseline and overlay ports in `vcpkg-configuration.json`, and the Qt version used for releases is pinned in `.github/workflows/ci.yml` (`app/CMakeLists.txt` requires at least 6.8).

## 2. Repository layout

```
fs-editor/
├─ Cargo.toml                 # workspace (edition 2024, resolver 3)
├─ Cargo.lock                 # pinned crate versions
├─ rust-toolchain.toml        # stable, pinned version
├─ LICENSE                    # BSD-3-Clause
├─ THIRD_PARTY.md             # licences of dependencies and bundled files (karaskel etc.)
├─ docs/
│  ├─ ARCHITECTURE.md         # this document
│  ├─ CLI.md                  # `fsed` manual
│  ├─ images/                 # README screenshot
│  ├─ releases/               # release notes (one file per version), template and release procedure
│  └─ research/               # notes: Kainote layout, Automation 4, dependency versions
├─ crates/
│  ├─ fs-core/                # document model, ASS parser/writer, tags, time, colours, undo, VFR, operations
│  ├─ fs-formats/             # SRT, WebVTT, MPL2, MicroDVD, TMP, export formats (EBU STL, Encore, TranStation, TTXT), encoding detection
│  ├─ fs-cli/                 # the `fsed` binary (info, check, roundtrip, convert, shift, sort, filter, …)
│  ├─ fs-audio/               # decoded PCM → peak cache (waveform), spectrogram (realfft), playback (cpal)
│  ├─ fs-media/               # FFI to BestSource (video/audio) and libass (rendering) — C++ in crates/fs-media/cpp
│  ├─ fs-automation/          # LuaJIT (mlua): `aegisub` API, subtitles object, dialogs, includes, MoonScript, lfs, FFI exports
│  └─ fs-bridge/              # cxx bridge: staticlib + headers for Qt
│     └─ src/
│        ├─ lib.rs            # #[cxx::bridge]: types and functions visible from C++
│        ├─ session/          # one tab: a document with its undo history; each file is the `impl Session` of one area plus its tests
│        │  ├─ mod.rs         # Session: open, save, autosave, project data, undo/redo, selection, row mapping
│        │  ├─ rows.rs        # grid rows: reading and writing fields, insert, delete, move, sort, clipboard, file comparison
│        │  ├─ joinsplit.rs   # joining, splitting and recombining lines (translation pairs by halves)
│        │  ├─ trees.rs       # Kainote trees
│        │  ├─ timing.rs      # time shifting, continuity, snapping to video, post-processor, FPS change
│        │  ├─ search.rs      # grid filter, find/replace, replacing selected results, multireplacer rules
│        │  ├─ convert.rs     # format conversion (Kainote), export (Aegisub), resampling
│        │  ├─ styles.rs      # script styles, renames that follow the lines, storage files
│        │  ├─ attachments.rs # attachments and the font collector
│        │  ├─ render.rs      # script for libass: whole, lines of one frame, a single line for measuring
│        │  ├─ translation.rs # translation mode
│        │  ├─ overrides.rs   # tags written by visual tools, "all tags" and Hydra
│        │  ├─ karaoke.rs     # karaoke bar syllables
│        │  └─ fixtures.rs    # script and session the module tests start from
│        ├─ automation.rs     # running Lua scripts on a session, export through filters
│        ├─ media.rs          # video, audio and subtitle renderer for the window
│        ├─ visual.rs         # line values edited by visual tools (\pos, \move, \clip, \p, scale, rotation)
│        ├─ split.rs          # Kainote splits that preserve appearance (characters, words, \move frames)
│        └─ kanji.rs, spell.rs  # kanji timer, spelling and thesaurus
├─ app/                       # Qt 6 Widgets GUI (C++23, CMake + Corrosion)
│  ├─ CMakeLists.txt, CMakePresets.json
│  ├─ src/
│  │  ├─ main.cpp             # startup, smoke test (FSED_SMOKE*), crash report on Windows
│  │  ├─ bridge.h             # Qt ↔ cxx conversions (QString ↔ rust::Str/String)
│  │  ├─ mainwindow.h         # MainWindow: the only window header the rest of the program sees; the class spans several translation units:
│  │  ├─ mainwindow.cpp       #   window, tabs at the bottom, two-tab view and comparison, command registry (add), closing
│  │  ├─ mainwindow_menus.cpp #   menu bar: build order, Audio, View and Help menus, Kainote menu layout (layOutMenus table)
│  │  ├─ mainwindow_filemenu.cpp, _editmenu.cpp, _videomenu.cpp, _subsmenu.cpp  #   File (with export and sessions), Edit, Video and Subtitles menu commands
│  │  ├─ mainwindow_files.cpp #   opening by file type and pairing, saving, conversion, sessions, recent files, on-disk changes, autosave
│  │  ├─ mainwindow_hotkeys.cpp  #   command shortcuts, hotkey dialog, event filter that hands panels their own keys
│  │  ├─ mainwindow_bars.cpp  #   toolbar and status bar
│  │  ├─ mainwindow_automation.cpp  #   autoload, Automation menu and script manager, running macros
│  │  ├─ mainwindow_corrector.cpp   #   minor-error corrector (multireplacer rules)
│  │  ├─ mainwindow_p.h       #   shared by these units only: struct Menus, file extensions, default toolbar, icon tints
│  │  ├─ tabpanel.*           # one tab = document + video + audio + edit box + grid + timing panel
│  │  ├─ grid.*               # custom grid (QAbstractScrollArea), filtering, shifting times with the mouse
│  │  ├─ edit box.*            # line fields, tag buttons, translation mode (2 fields)
│  │  ├─ shiftpanel.*         # "Shift times" panel and post-processor
│  │  ├─ videobox.*           # VideoView (QRhiWidget), decoding thread, zoom, player mode
│  │  ├─ visualtools.*        # \pos \move \org \clip \iclip \p, scale, rotation
│  │  ├─ colorpicker.*        # Aegisub's colour dialog: spectrum in 5 modes, RGB/HSL/HSV, ASS/HTML, alpha, recent colours, eyedropper
│  │  ├─ theme.*              # colour themes: colour roles, Dark/Light, user themes (.ini), window palette
│  │  ├─ hydra.*              # tag list of the "all tags" tool (Kainote's Hydra) and its editor
│  │  ├─ associations.*       # subtitle file type associations (per-user registry on Windows)
│  │  ├─ icons.*              # SVG icons from resources drawn at the requested size: tinted toolbar icons (icon), program icon (appIcon)
│  │  ├─ audiobox.*           # AudioView (QRhiWidget): waveform/spectrogram, karaoke, buttons
│  │  ├─ rhiquad.*            # a single textured QRhi quad used by video and audio
│  │  ├─ findreplace.*        # FindDialog: find/replace, "find all", find and replace in files, results window
│  │  ├─ selectlines.*        # SelectLinesDialog: Aegisub's "Select Lines"
│  │  ├─ styleeditor.*        # StyleEditor: style editor with preview, Kainote font catalogues
│  │  ├─ stylemanager.*       # StyleManager: file styles next to storage styles, editor in the window or docked
│  │  ├─ propertiesdialog.*   # PropertiesDialog: script properties ([Script Info])
│  │  ├─ historydialog.*      # HistoryDialog and undo step names (undoStepName)
│  │  ├─ resampledialog.*     # ResampleDialog: resolution resampling
│  │  ├─ fontcollector.*      # FontCollectorDialog: checking, copying, zipping and attaching fonts
│  │  ├─ settingsdialog.*     # SettingsDialog: settings, themes, conversion and default script properties pages
│  │  ├─ autosavedialog.*     # AutosaveDialog: "Open autosave file"
│  │  ├─ spellcheckdialog.*   # SpellCheckDialog: spell-check window
│  │  ├─ attachmentsdialog.*  # AttachmentsDialog: attachments
│  │  ├─ kanjitimer.*         # KanjiTimerDialog: Aegisub's kanji timer
│  │  ├─ assistants.*         # TranslationAssistant and StylingAssistant
│  │  ├─ pasteover.*          # chooseFields: fields for "Paste over" and column copy
│  │  ├─ tagbuttondialog.*    # TagButtonDialog: edit box tag button setup
│  │  ├─ dialogutil.*         # shared by dialogs and the rest of the program: autosave folder, style storages, ASS colour, media settings, index cache
│  │  └─ automationhost.*     # aegisub.dialog dialogs, file pickers, progress
│  ├─ icons/                  # toolbar and menu icons (Tabler Icons, SVGs tinted in icons.cpp), Qt resources
│  ├─ appicon/                # the program's own icon: fsed.svg and fsed-small.svg (below 32 px) as Qt resources for windows;
│  │                          #   tools/appicon.py turns them into fsed.ico (exe resource via fsed.rc) and fsed.icns (macOS bundle)
│  └─ shaders/                # .vert/.frag → .qsb (qt_add_shaders)
├─ tests/
│  ├─ corpus/                 # real .ass/.srt/… files (under licences that allow redistribution)
│  ├─ corpus-local/           # your own test files (git-ignored)
│  └─ media/                  # generated test video and audio (tools/gen-test-media.sh)
├─ tools/                     # deps.ps1/deps.sh (vcpkg media deps), smoke.sh, release-check.sh, gen-test-media.sh, appicon.py, dictionaries.py, vcpkg-ports/
├─ vcpkg.json                 # ffmpeg, libass, bestsource (overlay port), xxhash
├─ vcpkg-configuration.json   # vcpkg registry baseline and overlay ports
└─ .github/workflows/ci.yml  # tests (Windows, Linux), GUI build, smoke-tested ZIP packages, draft releases from v* tags
```

## 3. Core (`fs-core`)

### 3.1 Document model

```text
Document
├─ source: SourceInfo        # BOM, encoding, dominant line ending, whether the file ended with a newline
├─ sections: Vec<Section>     # in file order
│   ├─ ScriptInfo  { header_raw, entries: Vec<InfoEntry> }      # key:value, ';' comments, blank lines
│   ├─ Styles      { header_raw, format: FieldFormat, entries: Vec<StyleEntry> }
│   ├─ Events      { header_raw, format: FieldFormat, entries: Vec<EventEntry> }
│   └─ Other       { header_raw, lines: Vec<RawLine> }           # [Fonts], [Graphics], [Aegisub Project Garbage], unknown
└─ (auxiliary indexes: style→position, line id)
```

Each entry (`StyleEntry`, `EventEntry`, `InfoEntry`) stores:

* `raw: Option<RawLine>`: the original text of the line and its terminator (`\r\n`, `\n` or none), for as long as the entry is "clean";
* the parsed fields: `Style`, `Event` or `(key, value)`.

Changing a field clears `raw`, and the writer then serializes the entry canonically, following the section's `Format:` and the dominant line ending. Fields unknown to `Format:` are kept in `extra_fields` so they are not lost.

`Event`: `kind (Dialogue|Comment|Picture|Sound|Movie|Command)`, `layer`, `marked` (SSA), `start`, `end`, `style`, `actor`, `margin_l/r/v` (+ `margin_t/b` for v4++), `effect`, `text`, plus `id` (stable, used for undo and GUI selection).

`Style`: all v4+ fields (name, font, size, 4 colours `&HAABBGGRR`, bold, italic, underline, strikeout, scale_x/y, spacing, angle, border_style, outline, shadow, alignment, margins, alpha_level, encoding), with v4 (SSA) ↔ v4+ (ASS) conversion, i.e. numpad ↔ legacy alignment.

### 3.2 ASS parser

* Lines are split with their terminators preserved, and the BOM is detected (UTF-8 and UTF-16 via `fs-formats::encoding`).
* Section names are matched case-insensitively. The `Format:` line sets the field order; the default formats are v4+/v4/v4++.
* The last field (`Text`) may contain commas; the number of splits follows from the number of fields in `Format:`.
* The parser is as error-tolerant as libass/Aegisub: missing fields, spaces, negative times, `0:00:00.0`, hours > 9. Unparseable lines are kept as `RawLine` with a warning in `ParseReport`.

### 3.3 Override tags (`fs-core::tags`)

The `{…}` block tokenizer produces `Text | Override(Vec<Tag>) | Comment | Drawing`. It recognizes the full libass/VSFilter tag list (`\pos \move \org \clip \iclip \fad \fade \t \k \kf \K \ko \kt \p \pbo \an \a \fn \fs \fsc \fscx \fscy \fsp \frx \fry \frz \fr \fax \fay \bord \xbord \ybord \shad \xshad \yshad \be \blur \c \1c-\4c \alpha \1a-\4a \b \i \u \s \q \r …`) and parses arguments leniently (e.g. `\pos(1,2` without the closing parenthesis). The tokenizer is used by the visual tools, karaoke, tag cleaning, CPS calculation and value substitution (e.g. shifting `\pos`). Tag writing is minimally invasive: only the affected part of the text changes.

Syntax highlighting (`fs-core::syntax`) is a port of Aegisub's lexer (`TokenizeDialogueBody`, `MarkDrawings`, `SyntaxHighlight`) as a hand-written state machine with the same longest-match rule. It distinguishes braces, backslashes and parentheses, tag names, parameters, comments, line breaks, and karaoke template code and variables (only in template lines). Drawings (`\p` > 0 and vector `\clip`) are split into commands, X/Y coordinates and curve end points, and errors are flagged (no `m` at the start, an odd number of coordinates, a command glued to a number). The result is a list of runs (style, length). The bridge returns them in UTF-16 units for `QSyntaxHighlighter`. Aegisub's tests are ported 1:1.

### 3.4 Time and VFR

* `Time(i64 ms)` with formatting and parsing for ASS (`H:MM:SS.cc`), SRT (`HH:MM:SS,mmm`) and frames.
* `Framerate { numerator, denominator, timecodes }`: CFR keeps a single `[0]` in `timecodes`; VFR keeps every frame's start time in ms, from v1/v2 timecodes or from BestSource.
  * Operations: `frame_at_time(ms, Start|End|Exact)` and `time_at_frame(frame, Start|End|Exact)` with Aegisub semantics (a line's start time is the midpoint between frames), writing v2 timecodes, and extrapolation past the last frame.
* Keyframe files: Aegisub (`# keyframe format v1`), XviD pass, x264 stats, SCXviD/WWXD.

### 3.5 Undo

Undo is transaction-based: `doc.edit("Description", |tx| { … })`. A transaction collects a list of reversible changes (`InsertEvent`, `RemoveEvent`, `ModifyEvent{old,new}`, and the equivalents for styles, `ScriptInfo` and whole sections). The history has two stacks, a step limit and a memory budget (512 MB). A step costs only the lines it does not share with the state after it (the document is made of `Arc`-shared, copy-on-write lines), so sorting 50,000 lines is almost free, while changing every line costs the whole file. The oldest steps are dropped above the budget, but the last 20 are always kept. "Amend" merges consecutive changes of the same kind, such as typing in one field. Every transaction returns a `ChangeSet` (the range of affected rows), so the GUI refreshes only what it has to. The undo stack is per document (per tab).

### 3.6 Operations (`fs-core::ops`)

All operations can be called from the CLI, the GUI and Lua:

* time shifting (by time or by frames; all/selected/from a line onwards; start/end/both; optionally restricted to styles); in the grid, times are shifted by dragging or with shortcuts, as in Kainote;
* sorting (start, end, style, actor, effect, layer);
* filtering (style, actor, effect, text/regex, comments, selected, collisions); a filter is a view, not a modification. Lines hidden manually and lines the filter keeps hidden ("do not reset") are remembered by their identifiers, so inserting or deleting lines before them does not shift them;
* find/replace (plain text/regex, fields, with/without tags). Regex replacement reads back-references the way Aegisub does (Perl format from boost): `$1`, `${1}`, `\1`–`\9`, and `\\` as a single backslash. The exceptions are colour and alpha tags (`\1c`, `\3a`), which stay tags, and `\N`, which stays a line break (Aegisub reduces it to a bare "N");
* format conversion is an undo step together with the format and the file name: undoing it restores the lines, the format and the path;
* timing post-processor (lead-in/out, linking adjacent lines, snapping to keyframes);
* resolution resampling (`PlayResX/Y`, `\pos`, `\clip`, drawings, margins, sizes, outline);
* FPS change (CFR→CFR, VFR→CFR);
* collisions and overlaps, CPS/line length (the CPS cell in the grid turns gradually redder from the warning threshold to the error threshold, as in Aegisub), tag cleaning, joining and splitting lines;
* karaoke: splitting `\k` into syllables and writing them back;
* changing one tag across many lines (`tag_adjust`, Kainote's Hydra and the "change all tags" mode of the scale and rotation tools): add, set, multiply by the line number, gradients, insert (at the cursor or at the start of the line; for `\t`, around the selected tags), a step from the active line, and scaling every occurrence. A tag has 1–4 values: alpha one, a colour three (R, G, B), `\pos`, `\fad` and `\t` their leading numbers. Relative changes also cover tags inside `\t`, and a line with no tag at its start gets one there (from the style value) so that the whole line changes. Changes are always computed from the text as it was before the drag, so nothing accumulates;
* "keep proportions" (`transform_place`): positions, both `\move` points, `\org` and clips transformed by the same scale or rotation around one point. When rotating, a line with `\org` moves only `\org`, because libass already rotates the line around it;
* translation mode: a (original, translation) pair per line, read and written in a Kainote-compatible format (details in `docs/research/kainote-ui.md`). Joining merges originals with originals and translations with translations (a row with no translation contributes empty text) and takes the times from the translations. Splitting cuts the half the cursor is in, while the other half stays whole in the first part. Trees insert the description before the pair's original, and both halves get the marker, so it survives switching the mode off. "Recombine" and karaoke splitting work only outside the mode, because they depend on text shared between lines, which the original and the translation do not have.

### 3.7 Fonts and spelling

* `fs-core::fonts` collects the faces used in lines (`\fn`, `\b`, `\i`, `\r`) together with their characters. With the `fontdb` feature it finds font files, including by GDI names (IDs 1/4/6), and detects missing glyphs. This is used by `fsed fonts` and by the GUI font collector, which also searches MKV attachments.
* external font folder (Kainote): libass renders fonts from it (`ass_set_fonts_dir`, for video opened after the folder is set), they appear in the font lists (Qt), and the collector searches it like the system fonts;
* `fs-bridge::spell` wraps `spellbook` (Hunspell dictionaries, including encodings such as ISO8859-2). The word scanner skips `{…}` blocks, `\N`/`\h` and `\p` drawings, and returns UTF-16 positions for Qt.

## 4. Formats (`fs-formats`)

| Format | Read | Write | Notes |
|---|---|---|---|
| ASS (v4+, v4++) | ✔ | ✔ | lossless (fs-core) |
| SSA (v4) | ✔ | ✔ | style/alignment conversion |
| SRT | ✔ | ✔ | `<i> <b> <u> <s> <font color>` tags ↔ overrides |
| WebVTT | ✔ | ✔ | `NOTE`/`STYLE`/`REGION` blocks skipped; `<i> <b> <u>` ↔ overrides, entities, `line`/`align` ↔ `\an` |
| MPL2 | ✔ | ✔ | `[ds][ds]text`, `/` = italics, `|` = new line |
| MicroDVD | ✔ | ✔ | `{start}{end}`, `{1}{1}fps`, `{y:i}` etc., requires FPS |
| TMP | ✔ | ✔ | `hh:mm:ss:text` (end time computed) |
| MPEG-4 Timed Text (TTXT) | ✔ | ✔ | GPAC XML, as in Aegisub |
| Adobe Encore | – | ✔ | export, SMPTE times at the chosen FPS |
| TranStation | – | ✔ | export, SMPTE times at the chosen FPS |
| EBU STL (tech 3264) | – | ✔ | binary export with Aegisub's settings (TV standard, encoding, wrapping, TC offset) |

Each imported format (SRT, MicroDVD, MPL2, TMP, TTXT, plain text, and also an MKV track) has its own style manager storage in the settings, as in Aegisub: loaded subtitles get its styles (the "Default" storage by default), so that is how they look on video and after conversion to ASS. Media referenced in the script open with it always, never, or after a prompt listing the files (Aegisub's App/Auto/Load Linked Files); whatever is already open stays open.

Encoding: UTF-8/UTF-16 with a BOM is detected deterministically; otherwise `chardetng` is used, with a preference for Windows-1250 for Polish files. Files are saved as UTF-8 with a BOM by default, as in Aegisub, or in the source encoding.

## 5. Media and audio

* **`fs-media` (C++ + cxx)**
  * `VideoSource`: BestSource's `BestVideoSource`, with an RGBA frame for an exact frame number, a timestamp table (VFR), keyframes and resolution/SAR. A Matroska or MP4 file without an index opens from its packets alone, like FFMS2 in Aegisub; BestSource indexes it in the background on half the cores and takes over the frames once its index has the same ones. Until then, recently decoded frames stay in memory (256 MB, the same as the BestSource cache), so stepping back does not decode everything from the keyframe. A frame comes out in two ways. `frame_rgba` converts it with swscale on the CPU (bicubic, full horizontal chroma interpolation) using the matrix and range from the script's `YCbCr Matrix` header, as Aegisub does (None: those of the video; no header: TV.601; untagged video: guessed), without gamut or transfer-curve conversion. It feeds frame saving and copying, libass `blend`, the visual tools' measurements and `fsed media frame`. `frame_planes` hands over the decoder's planes as they are (planar YUV 4:2:0, 4:2:2 and 4:4:4, 8 to 16 bits) along with their description (`FramePlanes`): sizes, bytes per sample, a 3×4 matrix to RGB built from the same coefficients swscale uses (`sws_getCoefficients`, range scaling, `yuvj*` as full range), the position of the first chroma sample, and the ceiling of swscale's 15-bit row. Playback takes the planes, and the graphics card turns them into RGB (see the GUI below). A frame without such planes goes the RGBA path: a packed format, RGB, alpha or palette, interlaced, an unsupported matrix, and also a frame with mastering display metadata (HDR10), whose colours swscale maps its own way and which should look as it did before. Measurement (`decode_speed`, 4K HEVC 10-bit, 60 fps, 61 Mbit/s): 14.3 ms per frame to RGBA, 6.3 ms to planes (70 and 160 frames per second).
  * `AudioSource`: `BestAudioSource`, with `f32` PCM samples for a sample range and ready-made `i16` mono straight from the decoder's frames (`fill_mono`), each frame decoded once. BestSource's frame cache is a list searched from the start for every frame, so here it is sized to the track's two longest frames rather than a fixed amount: with TrueHD's 40-sample frames, a 16 MB cache held 40,000 of them and ten minutes took two to decode. `seeks()` reports whether BestSource can seek within the track (in AC-3 and DTS-HD MA it cannot, and every seek decodes from the start of the file), and `canceller()` aborts a read from another thread. A file with several audio tracks loads the first one matching the preferred languages or names from the settings (Kainote, `fs-bridge::media::preferred_track`), and with no match asks which one to load, as Aegisub does.
  * AviSynth (`.avs`) and VapourSynth (`.vpy`) scripts as video and audio (`cpp/script.cpp`), behind the same `VideoSource` and `AudioSource`, so the window and `fsed media` open them like any file. Neither library is linked: each is loaded on the first script of its kind, and if it is missing, opening fails with an error saying what to install.
    * AviSynth: `avisynth.dll` from the program folder or the system one (where Aegisub looks for it), `libavisynth` outside Windows, through the C interface (`avs_invoke` and the other functions looked up by name; structs and constants declared on our side, without the AviSynth header). The script is loaded with `Import`, as in Aegisub. Each source has its own environment, and calls are made one at a time under a single mutex.
    * VapourSynth: the VSScript library set in the settings, otherwise the one from `VSSCRIPT_PATH`, from the registry (`SOFTWARE\VapourSynth`, `VSScriptDLL`), from the library path, and finally the one reported by `python -m vapoursynth get-vsscript` (a pip install without registration; entries left by an old installation that point to a missing file are skipped). The API headers (`cpp/vapoursynth/`, LGPL) describe only the pointer structs VSScript hands out. Video is the first video node among the script's outputs, audio the first audio node.
    * The frame stays in the clip's format: its planes go into an `AVFrame` and through the same `convert` as frames from a file, so the matrix, range and `YCbCr Matrix` header work the same way. The frame's colour comes from its properties (`_Matrix`, `_ColorRange`, `_Transfer`, `_Primaries`, `_SARNum`/`_SARDen`; AviSynth+ uses the same names); without them the matrix is guessed from the size, as in Aegisub. A format swscale cannot read (float YUV, half float) is converted by the script core to 16 bits of the same family, without changing the matrix. Only AviSynth older than 2.6 (an Aegisub option, off by default) produces RGB32 itself through `ConvertToRGB32` with the matrix the editor asks for.
    * The frame count and FPS come from the clip, with constant timestamps. A VapourSynth script can supply its own in `__aegi_timecodes` and keyframes in `__aegi_keyframes` (arrays of numbers, as in Aegisub with VapourSynth); a clip without FPS takes the duration of its first frame. AviSynth does not provide keyframes.
    * AviSynth audio follows Aegisub's path: the down-mixer from the settings, `ConvertAudioTo16bit`, then `ResampleAudio` to the forced sample rate, or to 44100 Hz when the script has less than 32000. VapourSynth audio is 16-, 24- or 32-bit or float samples from frames of 3072 samples. The script is evaluated once per source, so the track is decoded by a single decoder, without splitting into parts. `list_tracks` reports the video and audio track according to what the script outputs, from the file's last evaluation at this version.
  * `SubtitleRenderer`: libass (`ass_read_memory` from the current document), `render(time) → Vec<AssImage>`. For the view it draws only the subtitles, as premultiplied-alpha RGBA cropped to the rectangle they occupy (`overlay_box`), and answers "same as before" when libass sees no change, so stationary subtitles cost nothing; the graphics card composites them onto the frame. `blend` composites them onto an RGBA frame on the CPU (frame saved with subtitles). The GUI hands the render thread an O(1) copy of the document (`RenderSubs`). Writing the script for libass and registering fonts from `[Fonts]` happen on that thread, not on the GUI thread.
  * embedded subtitles (`extract_subtitles`): Matroska has a block index (Cues), and FFmpeg and mkvmerge record every block of a subtitle track there together with its position in the cluster, so the track is read by seeking, a few bytes per line (`MatroskaIndex`: 0.6 s instead of 21 s for 124 kB of subtitles in a 23 GB film). The index is trusted only when it is known to be complete (the Lavf muxer, or mkvmerge's `NUMBER_OF_FRAMES` tag equal to the number of entries), and every block must be where the index says, belong to the track and have the time from the index; FFmpeg counts a block's position from the cluster ID rather than from its content, so the first block decides which of the two conventions applies. Everything else (no index, compression, lacing, WebVTT, another container) is read packet by packet through FFmpeg, i.e. the whole file: progress and cancellation then come from the file reads alone (`interrupt_callback`), because FFmpeg discards the other tracks internally and only returns control at the next subtitle. The window does both off its own thread (`read_embedded`), with a progress dialog after 300 ms, as Aegisub does with Matroska.
* **`fs-audio` (Rust)**
  * a per-pixel min/max peak cache at several zoom levels (waveform). A level's sums of |x| are divided by 4^level, so they fit in a `u32` at every level. Decoded PCM goes straight into a shared buffer, without a second copy of the track. Audio is available as soon as the index has been read and decodes in the background, like Aegisub's RAM cache (`PcmBuffer::load_in_background`): the buffer has its full length from the start, `ready()` reports how many samples from the beginning are decoded, and the rest is silence. Peaks are added incrementally (`PeakCache::refresh`), spectrum columns that reach into the undecoded part are not cached, and the window polls progress every 100 ms and redraws the view when something it shows has arrived. A track longer than a minute decodes in parts (up to four): each subsequent part has its own decoder, which first decodes the part's beginning on the side and takes over the part only once it got there with a two-second seek. A decoder that cannot seek is stopped, and its part is decoded by the previous one, in sequence. Dummy audio (2h30m, as in Aegisub) is 1 kHz: 18 MB instead of 800;
  * spectrogram: STFT via `realfft` (Hann window, size 2^n depending on zoom), with the result in dB as column textures. Sampled columns lie on multiples of a single step for the whole view, so a scrolled view reads columns from the cache. Missing ones are computed on all cores, and the cache (64 MB) evicts the least recently read;
  * playback: `cpal` (WASAPI, ALSA/Pulse/PipeWire, CoreAudio), ring buffer, playhead position reported to the GUI, resampling to the device rate; the output device can be chosen in the settings (a missing one falls back to the default). Audio delay (Kainote) shifts the samples once on load (the buffer starts that much earlier or later than the track): silence at the start or a trimmed beginning, so the waveform, spectrum and playback agree without further recalculation.
* **GUI**: `VideoView` and `AudioView` derive from `QRhiWidget`. This works on D3D11/D3D12, Vulkan and Metal, with OpenGL as the fallback.
  * The video view composes the frame and subtitles in a frame-sized texture, and only when the frame or the subtitles change; tools, zoom and panning draw that same texture. The planes (R8 or R16 textures, pooled buffers, uploaded without a copy) go through two passes, in the same order as in swscale: `chroma.frag` stretches chroma rows to luma width (swscale bicubic: B = 0, C = 0.6, edge sample repeated, row truncated like swscale's 15-bit row) into an RGBA16F texture, and `yuv.frag` interpolates vertically, adds luma and multiplies by the matrix from `FramePlanes`. A frame that arrived as RGBA is copied into that texture. The subtitles go on top. Only the result is scaled to the view with a linear filter, as the CPU frame was before, so the image matches the previous one at every zoom level: the test `planes_shade_to_the_frame_swscale_makes` holds this arithmetic to the swscale frame (greys identical, colour within 1 level; 601 and 709, limited and full range, 8 and 10 bits), and window screenshots from before and after the change differ by at most 2 levels. One deviation: with centre-sited chroma, swscale drops one filter tap in luma row and column 2, and the shader does not reproduce that (up to 6 levels there).
  * Without R16 textures, frames deeper than 8 bits arrive as RGBA, and without a renderable RGBA16F, all of them do. `video/planes=false` in the settings forces RGBA (for comparisons).
  * Textures created in `initialize` are not filled with an image there: the upload would wait in the batch for the first `render`, which, with a frame already available, swaps the texture for another one, and the upload would land in a deleted texture.

## 6. Automation (`fs-automation`)

* `mlua` with the `luajit` and `vendored` features; lpeg is compiled through `cc` and registered as `lpeg`. luabins (C, from the Aegisub subproject) is also compiled through `cc`; `require "luabins"` first shows Aegisub's deprecated-library warning and then returns the real module.
* A global `aegisub` table with the full Automation 4 API: `register_macro`, `register_filter`, `progress`, `debug`, `log`, `dialog`, `cancel`, `text_extents`, `frame_from_ms`, `ms_from_frame`, `video_size`, `keyframes`, `decode_path`, `file_name`, `project_properties`, `get_audio_selection`, `set_undo_point`, `gettext`, `lua_automation_version = 4`.
* The `subtitles` object as userdata indexed 1..n over all lines (info, styles, events, format), `insert`, `append`, `delete`, `deleterange` and `[0]=`. Mapped onto a document undo transaction.
* Include files from Aegisub (karaskel, utils, unicode, cleantags, `aegisub.re`/`aegisub.unicode`/`aegisub.util` etc.), redistributed under their licences (BSD/ISC) and listed in `THIRD_PARTY.md`.
* Export of the C symbols required by FFI modules (regex, clipboard) and the `lfs` module.
* The progress dialog behaves like Aegisub's `DialogProgress`: `aegisub.debug.out` and script errors appear in it as they happen, and if the script wrote anything, the dialog stays open with a "Close" button. Text produced outside a run (e.g. an error in a `validate` function) goes straight to the log window.
* When opened, the Automation menu fetches the state of all macros in a single call: the selection and the project context are computed once. `validate` can replace the description in the status bar, and a macro with `is_active` is a checkable item (Aegisub's COMMAND_TOGGLE).
* `aegisub.video_size` returns the aspect ratio and its mode as Aegisub does (the project's Video AR Mode/Value; video with non-square pixels is a custom mode). `text_extents` keeps the DC and fonts per thread instead of creating them on every call.
* Macros and export filters run on a worker thread; the progress dialog marshals dialogs to the GUI. The first "Cancel" sets `is_cancelled` (as in Aegisub); the second ("Stop script") aborts the script through a LuaJIT hook even if it does not check for cancellation. A loop compiled entirely by the JIT, with no calls, cannot be interrupted by the hook.
* Export works like Aegisub's `DialogExport`. The filter chain consists of the built-in "Transform Framerate" (priority 1000) and "Fix Styles" (−5000) plus script filters in load order, sorted by descending priority; a repeated name gets " (N)". The dialog receives the configuration panels of all filters at once (built on the worker thread). The list of checked filters (in list order) is written to the file as "Export Filters", even after cancelling, and the encoding as "Export Encoding". Script panel values are remembered in the document session in Aegisub's format (`name:value|…`, encoded like `inline_string_encode`), not in the file. Filters run on a copy of the document in the chosen order.
* File-local scripts ("Automation Scripts", Aegisub's `LocalScriptManager`): the `~` (file folder), `$` (automation folder) and `/` (absolute path) prefixes. When a file is opened, the program asks whether you trust its authors. An unknown prefix and a missing file produce Aegisub's warnings. A manually added script goes onto the file's list under the shortest of these forms, and one removed in the manager disappears from it.
* Per-file scripts, like Aegisub's windows: autoload scripts serve all tabs, and the others only the files that have them on their "Automation Scripts" list. A tab's menu, export and manager show only its scripts. A macro from another file's script cannot be run. Closing a tab unloads the scripts no open file uses any more.
* Autoload loads scripts in parallel, as Aegisub does (`std::async`): one Lua state per script, a thread pool sized to `available_parallelism`. Menu order stays file order, and a script that panics is reported and skipped.
* `Automation/Trace Level` (Settings → Automation) is a shared atomic read on every `aegisub.debug.out`, so changing it also affects scripts that are already loaded.
* MoonScript: the bundled `moonscript` compiler (Lua), which requires `lpeg`.
* DependencyControl: the `?user/automation/include` and `?user/automation/autoload` paths, `lfs`, `ffi`, `json`.
* Specification: `docs/research/automation4.md`.

## 7. GUI (Qt 6 Widgets) — Kainote layout

Details are in `docs/research/kainote-ui.md`. In short:

* menu, toolbar and tab bar (many files at once; each tab has its own document, video, audio and undo). Several files opened together (dropped, from the command line or from the open dialog) are combined as in Kainote: subtitles and videos in name order ("ep2" before "ep10") form pairs, each in its own tab, while audio and keyframes go to the last one;
* a tab contains the video (top left) with the line editor beside it; audio sits above the editor and the grid at the bottom;
* Edit-box behaviour from Kainote: double-clicking a word highlights its other occurrences in the line with a background (whole word, case-insensitive, also right after `\N` and `\h`; colour set in the theme) while that word stays selected; the tag list suggested after `\` says what each tag does (`TagListDelegate`);
* style editor inside the style manager window (Kainote, the "Edit styles in this window" checkbox; a separate window by default, as in Aegisub): it sits next to the lists and stays open while further styles are selected; on every "Apply", the edited style and the other selected ones are looked up by name (`fileApply` / `storageApply`), so changes to the lists under an open editor confuse nothing, and switching tab or storage closes it;
* update check (Aegisub, Kainote; `updates.*`): Help → "Check for updates" asks GitHub for the latest release and compares version numbers; a newer one is offered with its release notes and buttons for the download page, skipping this version, and reminding later. The startup check (once a day) is off until the user enables it in the settings, because it is the only place where the program connects to the network on its own. Only the project's releases page is ever opened, whatever the response contained;
* Kainote: Shift+click on the audio, edit box and video buttons assigns a shortcut to the button's command, and the tooltip shows its key. The multireplacer (Fix minor errors) is modeless: rules have "only in tags" and case handling (as found, as typed, lower, upper); the lines are all, selected, from the selected one, or from given styles. Find errors (in the tab or in all tabs) gives a result list to uncheck and replace in place (core: `replace_rule_matches`, one undo step per tab; each result is replaced as it was found in the line text, a result overlapping one already replaced is skipped, and lines hidden by the grid filter are not searched), and Replace also works across all tabs. The Video menu lists the audio streams (with several tracks, the loaded one checked; choosing one reloads audio) and the file's subtitle streams (loaded into the tab after a save prompt, with the tab's track checked until another file replaces it). Names taken from data (files, styles, tracks, chapters, macros, tabs) keep "&" in menus and on tabs (`fs::menuText`/`fs::plainText`), and a save prompt dismissed without an answer saves nothing. Options: counting spaces and punctuation in line breaks, active line after Ctrl+click, merging "n" in karaoke auto-split, moving a syllable boundary by clicking, initial video zoom, program font, the text field status bar in the menu, a word filter for autosaves, removing several dictionary words at once, comparison by selected styles, linked resolutions in properties, a temporary file cleanup dialog (selected, all, older than a date), and the "hide original" shortcut wraps the line's original in braces;
* Kainote: splitting a line into characters, words and line breaks gives each piece the line's full times and its own \pos or \move at the place where it was (from text sizes in the style after tags, the line's \an, tags preceding the piece kept, \org when rotated); splitting into frames turns \move into the \pos of the given frame. Assignable: the font button, pasting the whole text and the selection into the translation, hiding the original, tag buttons 1–20, Stop, the progress bar, aspect ratio and video removal, removing unused styles. The style editor has a font filter by characters (Polish letters by default, configurable in the settings);
* Kainote: opening a video offers subtitles with the same name (into the current tab, with a save prompt), and opening subtitles without a video that reference none offers a video with the same name from the folder; files opened together are still paired by openFiles without prompts. "Open video" from the menu takes the same route. The video settings have offsets (ms) for times inserted from video, never before zero;
* Aegisub: with video position display enabled, audio shades the range of the previous and current frame (colours in the theme). "Show original" in the edit box shows the line's text from the moment it was selected (read-only), with Restore, Clear, Clear text (keeps tags, comments and drawings) and Insert original buttons; the two new commands get shortcuts. The styling assistant has its own assignable keys (Enter accepts, F8 previews, PgUp/PgDown lines, Home video, End audio) and a "Keys" frame, and opens only once. The crosshair with Shift gives coordinates from the right and bottom edges, with three decimal places when the video is larger than the script resolution; double-clicking moves the active line there and the other selected lines by the same amount (\pos, \move, \org). In full screen, double-click still exits it, and without a line it toggles full screen;
* Aegisub: right-clicking the syllable bar changes the karaoke tag of the whole line to \k, \kf or \ko (one undo step; the current one is checked). Undo and Redo name the step they will take ("Undo: Line edit", "Nothing to undo"), and step names are translated in History as well. Commands with nothing to act on are greyed out, as in Aegisub: closing and saving an audio clip, saving and closing keyframes and timecodes, video details, revert to saved. State is checked when a menu opens and after every change;
* Kainote's colour sampler: right-clicking a colour button in the edit box opens a small window at the mouse position (eyedropper, screen magnifier, colour code); the colour shows in the line immediately, and Cancel reverts it; a setting swaps the clicks with the colour dialog;
* Kainote's font catalogues: named lists of font families in the settings (a font can be in several); in the style editor the font list is narrowed to a catalogue (or to fonts in no catalogue), "+" adds the current font to or removes it from the catalogue, and a catalogue window (add / rename / delete, fonts with preview to check, filter);
* editing several styles at once (Kainote): Edit with several styles selected — after a prompt, the others receive whatever changed in the edited one (except the name), in the file as one undo step (`style_with_changes` / `change_styles`), in a storage with the catalogue saved;
* searching the web for the selected phrase from the edit box menu (Kainote): ling.pl, bab.la, Google, synonim.net;
* visual tools toolbar in full screen (Kainote): a "Show toolbar" toggle in the full-screen panel, on by default when editing subtitles, remembered;
* replacing selected search results (Kainote): checkboxes next to matches and files, Check / Uncheck all, custom replacement text; each checked match is replaced where it was found, one undo step per file (`replace_matches`), lines changed since the search are skipped; files from a folder are first copied to ReplaceBackup, then saved in their own format; after replacing, the button waits for a new search;
* Kainote's default script properties (Title, Author, Translation, Proofreading, Timing, Editing): a settings page with a value and a checkbox for each; when saving to ASS, the checked ones that differ from the file are set in one undo step, after confirmation (Change / Keep current / Change and stop asking / Cancel save) if enabled;
* Kainote's autoload script loading modes: at startup (the default, as in Aegisub) or in the background, or on first opening the Automation menu (in the background or immediately); background loading is a separate thread (`start_autoload` / `finish_autoload`), a menu opened meanwhile shows a loading row and rebuilds itself when loading ends; the manager, export and macro shortcuts of not-yet-loaded scripts finish loading first;
* sessions in files (Kainote's session saving and loading): our own `.fses` format (JSON: the tab files — subtitles, or the video when there are none — and the active tab index up front), also opened from the command line and by dragging; a prompt about unsaved changes before saving, missing files skipped on load; no `.kls` import;
* BestSource index file limit (Aegisub's CleanCache for FFMS2 indexes, 20 files / 42 MB; Kainote's cache file limit): after every video or audio open, on a separate thread; the open file's indexes get the current date, so the least recently used ones go, not the ones in use (`fs_media::clean_cache`);
* Kainote's conversion settings (`Session::convert_with`, one undo step): for ASS, the video resolution or the new-file resolution (fixed 1280×720 in Kainote), a style from the storage in place of Default, tags at the start of lines; new end times from the character count without tags (min. 1 s, clipped to the next line; always for TMP); MicroDVD FPS without video; a settings dialog before converting;
* further Kainote options: full screen for video alone at startup, no visual tool warnings, the line seen in full screen and the line at the previous tab's time as active (the line at that time or the nearest start), sorting on open (an undo step only when the order changes; an already sorted file gets no empty step), time shift values carried between tabs;
* Kainote behaviours (options, Aegisub-like by default): subtitles in the current tab, Enter in the time fields without moving to the next line, spelling suggestions on double-click, centring the active line on jumps (not on clicks and arrow keys), originals selected after duplicating, pause on left click, video from the active line, a custom character for simplified tags, tab title length and the number of autosave files;
* Aegisub behaviour: visual tools drawn only while the mouse is over the video, keyframes in audio separately in karaoke mode, clicking in the grid without taking focus from the edit box;
* Aegisub limits: undo levels (per session, kept after opening another file; still alongside the memory budget) and the number of find and replace history entries;
* Aegisub display options: waveform style (maximum only, or maximum with average), karaoke syllable font (the syllable bar grows with it), keyframes on the scroll bar (can be turned off), mouse wheel actions on video separately for plain, Ctrl and Shift (window size, zoom, pan, each also reversed, or nothing), and where frames are saved (next to the video, next to the subtitles, or ask for a folder);
* automation: configurable autoload and include folders as in Aegisub (a `|`-separated list, `?user`/`?data` tokens; empty means the standard layout), automatic reloading of changed scripts (none / local / global / all; a `QFileSystemWatcher` refreshed from the Automation menu) and an "Edit" button in the manager as in Kainote (the editor from the settings or the system program);
* edit box: tag parameter calltips as in Aegisub (`fs_core::calltip` on tokens from `syntax`; its off-by-one bug, which lost a comma right after the cursor when choosing an overload, is fixed; the tooltip bolds the parameter under the cursor) and `{}`/`()` bracket matching as in Kainote (background colour in the theme); the smoke test screenshot paints visible tooltips and popups;
* spectrum as in Aegisub: quality (four levels, FFT size and step as in the FFTW version), frequency scale (five curves between linear and logarithmic, with 1 kHz at a set height; the speech boost button still gives its own logarithmic scale), the Icy Blue and Green colour schemes (HSL computed in the shader) and a column cache limit;
* settings dialog with "Apply" and "Restore defaults" as in Aegisub (it restores the options of its own pages and reopens; themes, shortcuts and file associations have their own reset), and the font collector also copies to the subtitle folder;
* backups as in Aegisub: a choosable autosave folder, a .ORIGINAL copy of the opened file (can be turned off), and saving the file itself after every change (changes within 300 ms of each other are saved once; only for a file that saves without prompts);
* minor Aegisub options: saving the interface state in the file (active line, scroll position, video position; disabling it removes them from the file), syntax highlighting in the edit box, Shift+Enter as `\n` instead of `\N`, the resolution of new files (from the first video or fixed) and toolbar icon size;
* audio options from Aegisub and Kainote: what is drawn (keyframes, time under the cursor, video position, selection background, second boundaries), line boundary thickness, snapping distance, drag sensitivity, and the time played around the marker;
* translation mode with two text fields in the editor (original and translation) and an original column in the grid;
* time shifting in the grid, the time shift panel and line filtering;
* video zoom (wheel, rectangle, Aegisub commands: zoom in/out Ctrl+Num+/−, 50/100/200% of video pixels, "Ctrl+Space" to the slider) and player mode (video only, no editing, full screen). Zooming out below fit draws the whole frame smaller (down to 10%) in the middle of the view, and "Center zoomed video" (Aegisub's video/reset_pan) moves the zoomed portion to the middle of the frame without changing the zoom. The fast-jump step (Alt+←/→) is in the video settings, 10 frames by default;
* video colours as in Aegisub: frames are decoded with the matrix from the script's `YCbCr Matrix` header (None means the video's own matrix; a missing or invalid value means VSFilter's TV.601). When a video is opened, the program warns about video with no tagged matrix and about HDR/WCG video, then sets or offers the script's matrix and layout resolution (`LayoutResX/Y`) according to the video settings, with the last choice remembered. A new, untouched file gets them without a prompt;
* Aegisub commands with their own keys in the hotkey editor: moving lines (Alt+↑/↓; in the text field Alt+↓ still marks the line as unconfirmed), audio spectrum or waveform view, Ctrl+Enter in the text field (commits times from audio and closes the undo step without changing the line), copying, cutting and pasting lines, showing or hiding the toolbar, and every vector clip mode. A command can have any number of keys (as in Aegisub); a key taken over from another command disappears only from that command's list;
* Medusa mode (Aegisub's app/toggle/global_hotkeys, Audio menu): timing keys that type nothing (numeric keypad, with Ctrl or Alt, F keys) work across the whole window, except video and audio, which have their own;
* smoothed freehand drawing (Aegisub): a point every 60 pixels, and on releasing the button the shape becomes Bézier curves (`fs-bridge::visual::smooth_figure`, Shemanarev's algorithm from `Spline::Smooth`, closed shape);
* translation assistant (Aegisub, Subtitles menu): plain-text blocks of lines in turn from the active one (without tags, comments, drawings and whitespace-only blocks, `fs-core::tags::translatable_ranges`), the original with the current block highlighted at the top, and the typed translation replaces the block. Enter accepts and moves on, F8 accepts and stays, PgUp/PgDown move between blocks, Insert inserts the original, Home and End play video and audio, and Shift+Enter inserts `\N`. The keys can be changed in the hotkey editor. A line selected elsewhere or changed outside the assistant starts from its first block;
* Aegisub's colour dialog (`colorpicker.*`) for colour tags, styles, Lua dialogs and dummy video. It has a spectrum in RGB/R, G, B, HSL/L and HSV/H modes, RGB/HSL/HSV, ASS and HTML fields, alpha, 32 recent colours, and an eyedropper: drag or two clicks, 7×7 magnification, and releasing takes the pixel. In the text field the colour is previewed live in a separate undo step that Cancel undoes;
* colour themes (`theme.*`, the "Colours" page in the settings). Every colour the program draws (grid, text field with Aegisub's styles and their bolding, audio, visual tools) is a role. There are built-in Dark and Light themes (Aegisub's default colours), user themes in `.ini` files (save as, delete, restore) and the window palette (system, light, dark). Changes show immediately, and Cancel restores the previous theme;
* 3D rotations in the visual tools computed the way libass does (`fs-bridge::visual::projection`): `\fax`/`\fay` shear (scaled by `\fscx`/`\fscy`) from the drawing's (0,0) point, Z, X and Y rotation around `\org`, then perspective projection with the camera at a distance of 312.5 · PlayResY / frame height. Drawing handles lie on the rendered shape, and dragging inverts the projection. The scale axes, the Z rotation circle and the X/Y rotation grid are drawn in the line's plane. A test compares the projection with libass bounding boxes for nine cases;
* visual tool options from Kainote, remembered in the settings. Scale: change all scale tags and keep proportions. Z rotation: change all `\frz` and keep proportions. X/Y rotation: change all `\frx`/`\fry`. Move: `\move` from two points and the video time. The anchor is the time of the first point, not the start of the line as in Kainote. Each line moves from where it was at that time, within its own `\move` times;
* position and scale by rectangle (Kainote). The line is measured the way libass draws it (`fs-media::bounds`): in a script 3× larger, with the line offset by the frame size, so the off-screen part also counts, and the result is the bounding box of the whole and of the text alone (without outline and shadow). Position places the line in the rectangle in one of 21 ways (9 alignments, below, above, before and after), scale fills it with text on the chosen axes, and the outline stays unchanged. A second, custom rectangle (right button) replaces the measured one;
* Hydra (the "all tags" tool): a user tag list with range, step, digit count, default values, insertion point and mode. Sliders over the video show the active line's values, the wheel changes a value by one step, Shift+wheel selects the tag, and the right button moves the sliders. For `\fad` and `\t`, the keys for the distance from the line's start and end (Ctrl+, and Ctrl+.) take the video time;
* the "File associations" page in the settings (Windows): a `FSEditor.<extension>` ProgID in `HKCU\Software\Classes`, so no administrator rights are needed. The type gets it as its default and in the "Open with" list, and turning it off restores the previous program. A choice made in Explorer (UserChoice) takes precedence, because only Explorer can write it;
* text field context menu: spelling suggestions, adding a word to the dictionary and removing it from there, the spell-check language (or turning spell-check off) and the thesaurus;
* autosave every N seconds (Aegisub): an O(1) copy of the document on the GUI thread, and the ASS write to disk on a worker thread. On a crash or logoff, the save is synchronous. Next to the copy lies a `.from` file with the original's path, so "Save" after recovery starts in that location;
* dragging in the grid and in audio past the edge keeps scrolling on a timer, even when the mouse is still, faster the further away it is. Lines picked up by Ctrl+dragging a marker are drawn along with it;
* fonts attached to the video for the position and scale rectangles load in the background as soon as you choose the tool. The style manager does not flag fonts attached to the file as missing. A font removed from `[Fonts]` also disappears from the screen: the renderer starts over, because libass can only remove all fonts at once.

## 8. Milestones

| Milestone | Scope | Completion criterion |
|---|---|---|
| 0 | Plan, repo layout, licence, toolchain | this document, `cargo check` |
| 1 | `fs-core` + `fs-formats` + `fsed` CLI | byte-for-byte roundtrip tests on real files, unit tests for time/VFR/undo/tags/formats |
| 2 | `fs-audio` (peaks, FFT, cpal) + `fs-media` (BestSource, libass) | tests on generated audio/video (frame-accurate seeking, VFR, sample-accurate audio, libass), `fsed media info`, `fsed media frame` |
| 3 | `fs-automation` (Automation 4, karaskel, MoonScript, DependencyControl) | running real scripts (kara-templater, TypesettingTools scripts) in `fsed lua` |
| 4 | `fs-bridge` + Qt GUI | the application with the full Kainote layout, video and audio on QRhi |
| 5 | Packaging (unpack-and-run ZIPs for Windows and Linux; macOS later), CI with packaged smoke tests, tag-driven releases | release ZIPs built and smoke-tested by GitHub Actions |
