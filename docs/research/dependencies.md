# FS Editor: third-party dependency research

Status: researched on **2026-10-13**. Versions come from the upstream release pages, the crates.io API, the Qt online repository and the vcpkg `master` ports tree (commit `e456309491fd875487bf02b8a0dca76c1b1b00cc`, 2026-10-10). The URLs used are listed in each section.

The project is licensed **BSD-3-Clause**, so the licence column matters. Every C/C++ dependency with a copyleft licence (Qt LGPLv3, FFmpeg LGPL, FriBidi LGPL) must be **linked dynamically** (or relinking must be made possible). The FFmpeg `gpl` / `nonfree` features must stay **off**.

---

## 0. Summary tables

### 0.1 Native (C/C++) dependencies

| Component | Latest stable (2026-10-13) | Released | Licence | Pin for FS Editor |
|---|---|---|---|---|
| Qt | **6.12.0** (LTS, 5-year maintenance) | 2026-09-30 | LGPLv3 / GPL / commercial | `6.12.0` |
| Qt (previous branch) | 6.11.3 (online repo only; source tarballs on `official_releases` stop at 6.11.2) | 2026-09-24 | same | — |
| aqtinstall | 3.3.0 on PyPI (2025-06-02): **cannot install Qt ≥ 6.11 on Windows**. Use git `master` (3.3.1.dev166, commit `076e1659`) | — | MIT | git master |
| BestSource | **R22** (release), master = v23-dev (`cf10da7567`, 2026-10-03) | R22: 2026-09-16 | MIT | `R22` |
| FFmpeg | **9.0.2 "Lei"** (libavcodec 63.1, libavformat 63, libavutil 61.1) | 2026-09-18 | LGPL-2.1+ (GPL with `--enable-gpl`) | 9.0.x |
| FFmpeg (older maintained branch) | 8.1.3 "Hoare" | 2026-09-21 | same | — |
| libass | **0.17.5** (`LIBASS_VERSION 0x01705000`) | 2026-06-24 | ISC | 0.17.5 |
| FreeType | 2.15.0 upstream / 2.14.3 in vcpkg | — | FTL or GPL-2.0+ | vcpkg |
| FriBidi | 1.0.17 | 2026-09-20 | LGPL-2.1+ | vcpkg (dynamic) |
| HarfBuzz | 14.6.0 upstream / 14.5.1 in vcpkg | 2026-10-05 | MIT | vcpkg |
| Fontconfig (Linux only) | 2.17.1 in vcpkg | — | MIT | system / vcpkg |
| xxHash | 0.8.4 | 2026-09-19 | BSD-2-Clause | vcpkg |
| libp2p (sekrit-twc, BestSource dep) | git HEAD (no releases) | 2026-07-05 | WTFPL | wrap/overlay |
| Corrosion | **v0.6.1** | 2026-01-17 | MIT | v0.6.1 |
| LuaJIT (vendored by mlua) | 2.1 rolling, commit `c6ffc141a876` (2026-09-08) via `luajit-src 210.7.4+c6ffc14` | 2026-09-27 (crate) | MIT | through `mlua` |
| LPeg | **1.1.0** | 2023-06-26 | MIT | 1.1.0 (C sources via `cc`) |
| MoonScript | **v0.7.0** (master is 0.8.0-dev with a new parser) | 2026-07-26 | MIT | 0.7.0, or 0.5.0 for strict Aegisub parity (see §7) |
| vcpkg tool (standalone) | 2026.07.29 | 2026-07-31 | MIT | — |

### 0.2 Rust crates (crates.io `max_stable_version`)

| Crate | Version | Licence | MSRV / notes |
|---|---|---|---|
| cxx | **1.0.205** | MIT OR Apache-2.0 | MSRV 1.88; features `c++14/17/20` |
| cxx-build | **1.0.205** | MIT OR Apache-2.0 | |
| cxxbridge-cmd | 1.0.205 | MIT OR Apache-2.0 | Corrosion builds this exact version |
| mlua | **0.12.2** | MIT | MSRV 1.88; `luajit`, **`luajit52` exists**, `vendored` |
| mlua-sys | 0.13.0 | MIT | `links = "lua"` |
| luajit-src | 210.7.4+c6ffc14 | MIT | mlua-sys requires `>=210.7.0, <210.8.0` |
| lua-src | 551.0.2 | MIT | (PUC Lua 5.5.1 etc., not used) |
| cpal | **0.18.2** | Apache-2.0 | MSRV 1.85 |
| realfft | **3.5.0** | MIT | depends on rustfft ^6.4 |
| rustfft | **6.4.1** | MIT OR Apache-2.0 | MSRV 1.61 |
| clap | **4.6.7** | MIT OR Apache-2.0 | MSRV 1.85 |
| thiserror | **2.0.21** | MIT OR Apache-2.0 | |
| anyhow | **1.0.104** | MIT OR Apache-2.0 | |
| encoding_rs | **0.8.42** | (Apache-2.0 OR MIT) AND BSD-3-Clause | |
| chardetng | **1.0.0** | Apache-2.0 OR MIT | |
| regex | **1.13.1** | MIT OR Apache-2.0 | |
| fancy-regex | **0.19.2** | MIT | |
| serde | **1.0.229** | MIT OR Apache-2.0 | |
| serde_json | **1.0.154** | MIT OR Apache-2.0 | |
| insta | **1.49.0** | Apache-2.0 | |
| proptest | **1.12.0** | MIT OR Apache-2.0 | MSRV 1.88 |
| pretty_assertions | **1.4.1** | MIT OR Apache-2.0 | |
| unicode-segmentation | **1.13.3** | MIT OR Apache-2.0 | |
| smallvec | **1.16.3** | MIT OR Apache-2.0 | |
| memchr | **2.8.3** | Unlicense OR MIT | |
| indexmap | **2.14.2** | Apache-2.0 OR MIT | |
| tempfile | **3.27.0** | MIT OR Apache-2.0 | |
| criterion | **0.8.2** | Apache-2.0 OR MIT | MSRV 1.86 |
| cc | **1.8.0** | MIT OR Apache-2.0 | |
| cmake | **0.1.58** | MIT OR Apache-2.0 | |
| lpeg (crate) | **does not exist** on crates.io | — | compile LPeg C sources with `cc` (§5.3) |

Every MSRV is well below the pinned toolchain (Rust 1.99.0, edition 2024).

Sources: `https://crates.io/api/v1/crates/<name>`, plus `https://crates.io/api/v1/crates/<name>/<ver>` for features and MSRV.

---

## 1. Qt 6

### 1.1 Version

* **Qt 6.12.0** was released on **2026-09-30**. It is an **LTS** release with 5 years of maintenance, and Qt now ships a new LTS every 2 years. It is the newest Qt branch.
  * https://www.qt.io/blog/qt-6.12-released
  * https://download.qt.io/official_releases/qt/6.12/ (only `6.12.0/`, dated 30-Sep-2026)
  * https://www.phoronix.com/news/Qt-6.12-LTS-Released
* The previous branch is 6.11: 6.11.0 (2026-03-20), 6.11.1 (2026-05-12), 6.11.2 (2026-08-18), and 6.11.3, which is available in the online repository (`qt6_6113`, 2026-09-24). The 6.10 branch ends at 6.10.3.
* The project already requires Qt ≥ 6.8, and **6.12.0 is the right pin**. It is LTS, so it gets long patch support, and it has `QRhiWidget`. The installed Qt 6.7.3 at `C:\Qt` should be replaced.

### 1.2 Supported toolchains (Qt 6.12 docs: https://doc.qt.io/qt-6/supported-platforms.html)

* **Windows 10 1809+ / 11 x64:** the binaries are built with **MSVC 2022** (`msvc2022_64`). MSVC 14.51 from VS 2026 is binary-compatible with them, since the MSVC v14x ABI has been stable since 2015. The app can be built with VS 2026 against the `msvc2022_64` binaries. VS 2026 is listed officially only for ARM64EC.
* **macOS:** build with Xcode 16 (macOS 15 SDK) or newer. Deployment target ≥ **macOS 14.4**. x86_64 and arm64 (the `clang_64` package is universal).
* **Linux:** the reference platforms are Ubuntu 22.04 (GCC 11) and 24.04 (GCC 13), RHEL 9.6/10, Debian 11/12 and openSUSE 16. For C++23 in the app, use GCC ≥ 13 or Clang ≥ 17.

### 1.3 Installing with aqtinstall

> **Important:** the latest PyPI release, `aqtinstall 3.3.0` (2025-06-02), **fails for Qt 6.11.x and 6.12.x on Windows** with `Failed to download checksum for the file 'Updates.xml'`. Starting with 6.11, the Windows repository is split per architecture (`qt6_6120/qt6_6120_msvc2022_64/Updates.xml`). The fix is merged upstream (miurahr/aqtinstall #959, #1000) but has not been released yet. Linux and macOS listing works with 3.3.0. Verified locally on 2026-10-13: `aqt list-qt windows desktop --arch 6.12.0` errors with 3.3.0 and returns the arch list with git master `3.3.1.dev166`.

```powershell
# Use an isolated venv; install aqtinstall from git master (pinned commit)
py -3.12 -m venv C:\Qt\.aqt-venv
C:\Qt\.aqt-venv\Scripts\python -m pip install "aqtinstall @ git+https://github.com/miurahr/aqtinstall.git@076e1659"

# Check what is available
C:\Qt\.aqt-venv\Scripts\aqt list-qt windows desktop                     # ... 6.11.0-6.11.3, 6.12.0
C:\Qt\.aqt-venv\Scripts\aqt list-qt windows desktop --arch 6.12.0       # win64_msvc2022_64 win64_mingw win64_llvm_mingw win64_msvc2022_arm64_cross_compiled
C:\Qt\.aqt-venv\Scripts\aqt list-qt windows desktop --modules 6.12.0 win64_msvc2022_64

# Install (qtbase includes Widgets, Gui, the RHI and the private headers needed for Qt6::GuiPrivate)
C:\Qt\.aqt-venv\Scripts\aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 -O C:\Qt -m qtshadertools qtimageformats
# -> C:\Qt\6.12.0\msvc2022_64   (use as CMAKE_PREFIX_PATH / Qt6_DIR=C:\Qt\6.12.0\msvc2022_64\lib\cmake\Qt6)
```

Linux and macOS (PyPI 3.3.0 is fine there, but use the same git pin for reproducibility):

```bash
aqt install-qt linux desktop 6.12.0 linux_gcc_64 -O ~/Qt -m qtshadertools qtimageformats   # -> ~/Qt/6.12.0/gcc_64
aqt install-qt mac   desktop 6.12.0 clang_64     -O ~/Qt -m qtshadertools qtimageformats   # -> ~/Qt/6.12.0/macos
```

Modules available for 6.12.0 / `win64_msvc2022_64` (from `aqt list-qt ... --modules`): qt3d, qt5compat, qtactiveqt, qtcanvaspainter, qtcharts, qtconnectivity, qtdatavis3d, qtgraphs, qtgrpc, qthttpserver, qtimageformats, qtlanguageserver, qtlocation, qtlottie, qtmultimedia, qtnetworkauth, qtopenapi, qtpdf, qtpositioning, qtquick3d, qtquick3dphysics, qtquickeffectmaker, qtquicktimeline, qtremoteobjects, qtscxml, qtsensors, qtserialbus, qtserialport, **qtshadertools**, qtspeech, qttasktree, qtvirtualkeyboard, qtwebchannel, qtwebengine, qtwebsockets, qtwebview.

* **`qtshadertools` is required** for `qt_add_shaders()` (CMake `find_package(Qt6 REQUIRED COMPONENTS ShaderTools)`). It compiles GLSL to `.qsb` (SPIR-V, HLSL, MSL and GLSL variants) at build time. Licence: the runtime library is LGPLv3/GPLv2 since Qt 6.3, and the `qsb` tool is GPLv3 or commercial. The tool runs only at build time and is not shipped. https://doc.qt.io/qt-6/qtshadertools-index.html
* **Do not use Qt Multimedia.** Audio goes through `cpal` and video through BestSource. Qt Multimedia also deploys its own FFmpeg DLLs. Those have different sonames from FFmpeg 9 (`avcodec-63.dll`), so they would not clash, but they are dead weight.
* vcpkg also has `qtbase` 6.11.2 (not 6.12 yet). Prefer the aqt binaries for development and keep vcpkg for the C libraries only.

Sources:
* https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/
* https://pypi.org/project/aqtinstall/
* https://github.com/miurahr/aqtinstall (issues #959 and #1000)

### 1.4 QRhi / QRhiWidget status in 6.12

* **QRhi** (`#include <rhi/qrhi.h>`) has been available since **6.6** as a *limited-compatibility* API. The 6.12 docs state there are no source or binary compatibility guarantees: code is guaranteed only for the Qt version it was built against, and source-incompatible changes are kept minimal and made only in minor releases. To use it, link **`Qt6::GuiPrivate`** (`QT += gui-private` with qmake).
  * https://doc.qt.io/qt-6/qrhi.html
* **QRhiWidget** (`#include <QRhiWidget>`, module **Qt Widgets**, since **6.7**) is a **fully public** class. Anything that touches the `QRhi*` resource classes inside it still needs `Qt6::GuiPrivate`.
  * https://doc.qt.io/qt-6/qrhiwidget.html
* Consequence: **pin the exact Qt minor version** (`tools/qt-version.txt` / `CMakePresets.json`, as ARCHITECTURE.md already plans). Expect small source fixes when moving to a new minor version.

#### QRhiWidget usage notes for FS Editor (video + audio views)

* Subclass `QRhiWidget` and override `initialize(QRhiCommandBuffer*)`, `render(QRhiCommandBuffer*)` and `releaseResources()`.
  * `initialize()` runs on first show and again on resize, format or sample-count change, and on a QRhi change after reparenting to another top-level window. Re-create size-dependent resources there.
  * `releaseResources()` must drop everything synchronously.
* **Backend:** the default is D3D11 on Windows, Metal on macOS and OpenGL on Linux. Change it with `setApi()` in the constructor (`Vulkan`, `Direct3D12`, ...). **Every QRhiWidget/QQuickWidget in one top-level window must use the same API.** Pick one per platform for the main window.
* **Video:**
  * Upload decoded frames (BestSource `AVFrame`) as plane textures: R8/R16 for Y, U and V, or RG8 for NV12 UV. Do the YUV→RGB conversion in the fragment shader (BT.601/709/2020, limited/full range, matching the Aegisub "colour matrix override").
  * Alternatively convert with swscale to RGBA on the CPU (simpler; this is what Aegisub does, to `AV_PIX_FMT_BGR0`) and upload one RGBA8 texture.
  * Use `QRhiResourceUpdateBatch::uploadTexture()` per frame and draw a fullscreen quad with letterboxing.
* **Subtitles:** libass returns alpha-mask bitmaps with a per-image colour. Either composite them on the CPU into one premultiplied RGBA8 overlay texture (simple, and fast enough for a 1080p editor preview), or pack them into an R8 atlas and draw instanced quads with a colour uniform. Blend with premultiplied alpha over the video quad.
* **Audio views** (waveform / spectrogram): use a separate QRhiWidget with the same API.
  * Upload peak min/max per column as a 1D/2D texture, or as a vertex buffer.
  * Upload the spectrogram (from `realfft`) as an R16F/R32F texture with a palette LUT in the shader.
  * Call `update()` from `render()` only while playing; otherwise render on demand.
  * **Audio playback is not QRhi's job:** `cpal` (Rust) is the clock master and the GUI polls the playback position.
* `colorBufferFormat` RGBA8 is enough. `RGBA16F` can be used later for HDR preview.
* `grabFramebuffer()` works off-screen, which is useful for "export frame with subtitles" and for GUI tests.
* Shaders: `qt_add_shaders(target "shaders" PREFIX "/shaders" FILES video.vert video.frag ...)`. Load the result with `QShader::fromSerialized()`.

---

## 2. BestSource

Repository: https://github.com/vapoursynth/bestsource. Licence: **MIT** (Fredrik Mellbin). Releases: https://github.com/vapoursynth/bestsource/releases

### 2.1 Versions

| Tag | Date | FFmpeg minimum (meson) | Note |
|---|---|---|---|
| **R22** (latest release) | 2026-09-16 | **FFmpeg 9.0**: libavcodec ≥ 63.1, libavformat ≥ 63.1, libavutil ≥ 61.1 | GPU support reworked (Vulkan only), **not source compatible with R21** (constructor changed) |
| R21 | 2026-08-18 | FFmpeg 8.0: libavcodec ≥ 62.11, libavformat ≥ 62.3, libavutil ≥ 60.8 | last version with the `HWDeviceName` string constructor |
| master (v23-dev, `BEST_SOURCE_VERSION_MAJOR 23`) | `cf10da7567`, 2026-10-03 | FFmpeg 9.0 | linear-decoding fallback fixes, audio preroll clean-up |

Recommendation: **pin R22** (or a master commit if fixes after R22 are needed) together with **FFmpeg 9.0.x**.

### 2.2 Build system and dependencies

* **meson ≥ 1.2.3** with ninja, plus pkg-config (or pkgconf). The C++ standard is C++17. Meson's `python` module is always used, so Python must be present at configure time even when the plugin is disabled.
* Required dependencies: `libavcodec`, `libavformat`, `libavutil` (FFmpeg ≥ 9.0), **`libp2p`** (sekrit-twc pixel pack/unpack, WTFPL; pulled in as a meson wrap with `subprojects/packagefiles/libp2p/meson.build`, plus an SSE4.1 TU on x86), and **`libxxhash`**.
* Optional GPU path (`-Denable_gpu=auto|enabled|disabled`) needs:
  * Vulkan headers ≥ 1.4 (only for the plugin; a library-only build is fine with older headers),
  * `glslangValidator`,
  * FFmpeg *master* (libavutil ≥ 61.6) built with `--enable-vulkan`.

  It is not needed for FS Editor: **use `-Denable_gpu=disabled`**, because we want CPU frames for upload.
* **Standalone build (no VapourSynth or AviSynth): yes.** `-Denable_plugin=false` drops `vapoursynth.cpp`, `avisynth.cpp` and `synthshared.cpp`, installs the headers into `include/bestsource/`, generates `bestsource.pc`, and also `meson.override_dependency('bestsource', ...)` for subproject use. Aegisub does exactly this.

```bash
# Linux / macOS (FFmpeg 9 dev packages visible via pkg-config)
git clone --branch R22 --depth 1 https://github.com/vapoursynth/bestsource.git
cd bestsource
meson setup build -Denable_plugin=false -Denable_gpu=disabled --buildtype=release
meson compile -C build && meson install -C build
```

* **Windows:** upstream recommends vcpkg for `ffmpeg`, `xxhash` (and, for GPU builds, `vulkan-headers` and `glslang[tools]`). Upstream's command uses `ffmpeg[...,gpl,version3,...]:x64-windows-static`. **Do not copy the `gpl` feature** (licence, see §3).
* **vcpkg has no `bestsource` port.** As planned in ARCHITECTURE.md, write an **overlay port**:
  * `vcpkg_from_github` for bestsource@R22 **and** for sekrit-twc/libp2p, pinned to a commit. Copy libp2p into `subprojects/libp2p` together with BestSource's `packagefiles/libp2p/meson.build`, so meson does not hit the network.
  * `vcpkg_configure_meson(OPTIONS -Denable_plugin=false -Denable_gpu=disabled)`.
  * Dependencies: `ffmpeg[core,avcodec,avformat]`, `xxhash`.
  * The simpler alternative is to compile the 5 BestSource `.cpp` files (`audiosource`, `bsshared`, `gpuhash` (stubbed with no `BS_GPU`), `tracklist`, `videosource`) and the libp2p sources (`p2p_api.cpp`, `v210.cpp`, `simd/cpuinfo_x86.cpp`, `simd/p2p_simd.cpp`, `simd/p2p_sse41.cpp` + `-DP2P_SIMD`) directly from CMake in `crates/fs-media/cpp`.

### 2.3 C++ API (R22 / master headers: `videosource.h`, `audiosource.h`, `tracklist.h`, `bsshared.h`)

Common types:
* Errors are reported as exceptions. `BestSourceException : std::runtime_error`, with subclasses `BestSourceHWDecoderException` and `BestSourceCancelledException` (master).
* `using ProgressFunction = std::function<bool(int Track, int64_t Current, int64_t Total)>;` Return `false` to cancel indexing.
* `SetCancellationCallback(std::function<bool()>)` cancels long operations.
* `BestCacheMode`:
  * `bcmDisable = 0`
  * `bcmAutoSubTree = 1`: read the index, write it only when worthwhile, under `<CachePath>/<subtree of source path>`
  * `bcmAlwaysWriteSubTree = 2`
  * `bcmAutoAbsolutePath = 3`: `CachePath` is used as the base *filename*, with track number and extension appended
  * `bcmAlwaysAbsolutePath = 4`

  The default cache directory is `%LOCALAPPDATA%` on Windows and `$XDG_CACHE_HOME/bsindex` or `~/bsindex` elsewhere.
* `SetFFmpegLogLevel(int)` and `SetBSDebugOutput(bool)`.

**Track selection** (`BestTrackList`):
```cpp
BestTrackList tl(path, /*LAVFOpts*/ &opts);
for (int i = 0; i < tl.GetNumTracks(); ++i) {
    const auto &ti = tl.GetTrackInfo(i); // MediaType (AVMediaType), MediaTypeString, Codec (AVCodecID), CodecString, Disposition, DispositionString
}
tl.GetFileMetadata(); tl.GetTrackMetadata(i);   // std::map<std::string,std::string>
```
The `Track` argument of the sources is either an absolute stream index (≥ 0) or **negative to mean the n-th track of that type** (`-1` = first video/audio track). `GetTrack()` returns the resolved absolute index.

**Video** (R22 constructor; *R21 and earlier used `(path, const std::string &HWDeviceName, int ExtraHWFrames, Track, ...)`*):
```cpp
BestVideoSource(const std::filesystem::path &SourceFile,
                bool GPU,                                            // false for us
                const std::optional<std::array<uint8_t,16>> &DeviceUUID, // std::nullopt
                int Track, int ViewID /*0*/, int Threads /*0 = auto*/,
                int CacheMode, const std::filesystem::path &CachePath,
                const std::map<std::string,std::string> *LAVFOpts,
                const ProgressFunction &Progress = nullptr);
```
* The constructor **indexes the whole track**: it decodes every frame once and stores a hash per frame, unless a valid index file is found in the cache. Run it on a worker thread and show progress.
* `const BSVideoProperties &GetVideoProperties()` returns:
  * `TimeBase`, `Duration`, `NumFrames`, `FPS`, `SAR`, `Width`, `Height`, `SSModWidth/Height`, `VF` (`ColorFamily`, `Bits`, `SubSamplingW/H`, `Alpha`, `Float`), `Format` (AVPixelFormat),
  * `StartTime` (s), `FileTimeBase`, `FileStartTime`, `NumRFFFrames`, `TFF`,
  * HDR mastering / CLL metadata, and `FlipVertical` / `Rotation`.
* `BestVideoFrame *GetFrame(int64_t N, bool Linear = false)` returns an **owning raw pointer**, so wrap it in `std::unique_ptr`. The frame offers:
  * `GetAVFrame()`, which keeps the native pixel format: feed it to swscale or upload the planes.
  * `ExportAsPlanar(dst[], stride[], alphaDst, alphaStride)`.
  * Fields `Width`, `Height`, `PTS`, `Duration`, `Matrix`, `Primaries`, `Transfer`, `ColorRange`, `ChromaLocation`, `KeyFrame`, `PictType`, `RepeatPict`, `InterlacedFrame`, `TopFieldFirst`, plus DoVi, HDR10+ and ICC blobs.
* `GetFrameWithRFF(N)` applies RFF (soft telecine). `GetFrameByTime(double seconds)` is also available.
* **Frame-exact seeking:**
  * BestSource seeks to the nearest keyframe before the target, minus a preroll (`SetSeekPreRoll(frames)`; the default of 20 is auto-reduced to fit 80% of the cache), then decodes forward and **verifies the frame hashes** against the index.
  * When a seek proves unreliable, the bad location is blacklisted and retried. If seeking cannot be trusted, BestSource falls back to **linear decoding**: `GetLinearDecodingState()` returns true and seeking becomes slow. Aegisub shows a warning when this happens.
  * Known slow or bad cases: MPEG-TS/PS/VOB seeking, VC-1 (unseekable), VFR H.264 in AVI.
* **VFR timestamps:** `const FrameInfo &GetFrameInfo(int64_t N)` gives `{PTS, RepeatPict, KeyFrame, TFF, Format, Width, Height, Hash}`.
  * Timestamp in ms = `PTS * TimeBase.Num * 1000 / TimeBase.Den`.
  * Iterate over all `NumFrames` to build the timestamp vector (VFR) and the keyframe list.
  * `WriteTimecodes(path)` writes a v2 timecode file.
* **Caching and memory:**
  * `SetMaxCacheSize(bytes)`: the decoded-frame LRU, default 100 MB.
  * `SetMaxDecoderInstances(1..4)`: default 4; each 4K H.264 decoder costs about 250 MB.
* **Variable format:** use `GetFormatSets()` / `SelectFormatSet(i)` (`-1` = variable) for files whose resolution or pixel format changes mid-stream.

**Audio:**
```cpp
BestAudioSource(const std::filesystem::path &SourceFile, int Track /*-1*/,
                int AdjustDelay /* -1 = relative to first video track, -2 = off */,
                int Threads, int CacheMode, const std::filesystem::path &CachePath,
                const std::map<std::string,std::string> *LAVFOpts, double DrcScale /*0*/,
                const ProgressFunction &Progress = nullptr);
```
* `GetAudioProperties()` returns `AF.Float`, `AF.Bits`, `AF.BytesPerSample`, `SampleRate`, `Channels`, `ChannelLayout`, `NumSamples`, `NumFrames` and `StartTime`.
* `GetPackedAudio(uint8_t *dst, int64_t Start, int64_t Count)` returns interleaved samples. Samples outside the stream (for example a negative start caused by the delay adjustment) are zero-filled. `GetPlanarAudio(uint8_t *const *dst, Start, Count)` is the planar variant.
* `GetFrameRangeBySamples(start, count)` is also available.
* Access is sample-exact. Seeking is slow for some lossy formats, notably AAC, and chained Ogg is decoded linearly.
* For FS Editor, decode the full track once into `fs-audio`'s own cache (peaks and FFT) and use `GetPackedAudio` for playback ranges.

**Thread safety:** a single `BestVideoSource` or `BestAudioSource` instance is **not** thread-safe. Guard it with a mutex, or use separate instances for the preview thread and the background tasks (keyframe and peak scanning). Separate instances share the on-disk index.

### 2.4 How Aegisub (arch1t3cht fork) integrates BestSource

Repository: https://github.com/arch1t3cht/Aegisub, default branch `feature`. Files:
* `src/video_provider_bestsource.cpp`
* `src/audio_provider_bestsource.cpp`
* `src/bestsource_common.{h,cpp}`
* `subprojects/bestsource.wrap`
* `subprojects/packagefiles/bestsource/0001.patch`

* **Build:**
  * `meson.build` uses `dependency('bestsource', version: '>=6.0', fallback: ['bestsource','bestsource_dep'], default_options: ['enable_plugin=false'])` and defines `WITH_BESTSOURCE`.
  * The wrap still pins **R8** (`revision = R8`, `clone-recursive = true`) with a small patch that hard-codes the meson project version.
  * Aegisub links FFmpeg (`libavutil`, `libswscale`) itself for colour conversion.
* **Common code (`provider_bs`):**
  * `SelectTrack()` builds a `BestTrackList`, collects the tracks of the wanted `AVMediaType`, and shows a chooser dialog when there is more than one. It also reports whether the file has audio.
  * `GetCacheFile()` returns `?local/bsindex/<filename>_<crc32(path)>_<mtime>`, which is passed as `CachePath` with cache mode 1.
  * `CleanBSCache()` prunes `*.bsindex` by total size and file count (user options).
* **Video provider:**
  * Construction runs inside `BackgroundRunner` with a progress callback ("Decoding the full track to ensure perfect frame accuracy"). Returning `!ps->IsCancelled()` cancels; the cancel is detected via the exception message.
  * It then sets `SetMaxCacheSize(MB << 20)`, `SetSeekPreRoll(n)` and the thread count from options.
  * It scans `GetFrameInfo(n)` for all frames to build `Keyframes` and a millisecond timecode vector, which becomes `agi::vfr::Framerate`. With fewer than 2 distinct stamps it falls back to CFR from `FPS`.
  * It reads the colour matrix and range from frame 0 (`avframe->colorspace`, `color_range`, guessed when unknown), then converts every frame with `sws_scale` to `AV_PIX_FMT_BGR0`, honouring the user's colour-matrix override.
  * It optionally uses `GetFrameWithRFF`.
  * It throws on variable format (frame format or size ≠ first frame).
  * It shows a one-time warning when `GetLinearDecodingState()` becomes true.
  * `WantsCaching() = false`, because BestSource caches internally.
* **Audio provider:**
  * It calls `BestAudioSource(path, track, -1 /*adjust delay to first video track*/, ...)` with the same cache-file scheme.
  * The properties map to Aegisub's `float_samples`, `bytes_per_sample`, `sample_rate`, `channels` and `num_samples`.
  * `FillBuffer()` is simply `bs->GetPackedAudio(buf, start, count)`.
  * An option controls whether Aegisub's own RAM or disk audio cache is layered on top.
* **For FS Editor:** port this logic to the R22 constructor (`GPU = false`, `DeviceUUID = std::nullopt`, `ViewID = 0`). Keep the per-file cache naming (path hash + mtime), the cancellable progress, the keyframe and timecode extraction, the linear-mode warning and the colour-matrix handling.

---

## 3. FFmpeg

* **Latest stable: FFmpeg 9.0.2 "Lei"**, released 2026-09-18. Other maintained branches: 8.1.3 (2026-09-21), 8.0.3, 7.1.5 and 6.1.6.
  * https://ffmpeg.org/download.html
  * https://ffmpeg.org/releases/
* 9.0 library majors: **libavcodec 63**, **libavformat 63**, **libavutil 61** (minor 1). BestSource R22 requires exactly this generation.
* Licence: **LGPL-2.1-or-later** by default. `--enable-gpl` (x264, x265, etc.) makes it GPL, and `--enable-nonfree` makes it undistributable. FS Editor (BSD-3) must ship an **LGPL, dynamically linked** build. Only decoders are needed.

### Obtaining dev libraries

**Windows, option A (recommended): vcpkg manifest.**
* Port `ffmpeg` **9.0.2#1** (https://github.com/microsoft/vcpkg/tree/master/ports/ffmpeg).
* Default features: `avcodec, avdevice, avfilter, avformat, swresample, swscale`.
* Other features include `all`, `all-gpl`, `all-nonfree`, `amf`, `aom`, `ass`, `avisynthplus`, `bzip2`, `dav1d`, `drawtext`, `fdk-aac`, `fontconfig`, `freetype`, `fribidi`, `gpl`, `iconv`, `lzma`, `modplug`, `mp3lame`, `nonfree`, `nvcodec`, `opencl`, `openh264`, `openjpeg`, `openmpt`, `openssl`, `opus`, `qsv`, `snappy`, `soxr`, `speex`, `srt`, `svt-av1`, `theora`, `vaapi`, `version3`, `vorbis`, `vpx`, `vulkan`, `webp`, `x264`, `x265`, `xml2`, `zlib`, ...

```json
{
  "name": "ffmpeg",
  "default-features": false,
  "features": ["avcodec", "avformat", "swscale", "swresample", "dav1d", "zlib", "bzip2", "lzma"]
}
```

* Use the dynamic triplet **`x64-windows`** for LGPL compliance. Static is only acceptable if you provide object files for relinking.
* The `dav1d` feature (BSD-2) adds a fast AV1 decoder. FFmpeg's native decoders cover H.264, HEVC, VP9, MPEG-2, ProRes, AAC, AC-3, Opus, FLAC and others.
* `vulkan` is not needed.
* VS 2026 ships vcpkg only as a bootstrapper (`VC\vcpkg`, manifest mode). Use a manifest with `builtin-baseline` set to a recent commit (e.g. `e456309491fd875487bf02b8a0dca76c1b1b00cc`, 2026-10-10), or clone https://github.com/microsoft/vcpkg yourself.

**Windows, option B: prebuilt shared builds** (for quick experiments and CI).
* **BtbN/FFmpeg-Builds** (https://github.com/BtbN/FFmpeg-Builds/releases, daily `latest` tag): `ffmpeg-n9.0-latest-win64-lgpl-shared-9.0.zip`. It contains `include/`, the MSVC-compatible import libraries `lib/*.lib` and `bin/*.dll`. There are also `-gpl-` variants (avoid), and `linux64` and `linuxarm64` builds with the same naming.
* **gyan.dev** (https://www.gyan.dev/ffmpeg/builds/): `ffmpeg-release-full-shared.7z`, currently 9.0.2. **This build is GPLv3** because it bundles x264 and others, so it is **not suitable for redistribution with FS Editor**.

**Linux:** distribution packages (`libavcodec-dev` etc.) often trail behind and may still be 7.x or 8.x. BestSource R22 needs ≥ 9.0. Options:
* build FFmpeg 9.0.2 from the release tarball (`./configure --enable-shared --disable-programs --disable-doc --enable-libdav1d`),
* use BtbN `linux64-lgpl-shared`,
* use vcpkg (`x64-linux-dynamic`),
* or pin BestSource **R21** if you must use a distro FFmpeg 8.x.

**macOS:** Homebrew `ffmpeg` follows upstream releases (check with `brew info ffmpeg`). Note that Homebrew's build enables GPL components. For distribution, build LGPL FFmpeg via vcpkg (`arm64-osx-dynamic` / `x64-osx-dynamic`) or from source.

---

## 4. libass

* **Latest: 0.17.5** (2026-06-24). It fixes two out-of-bounds-write CVEs (CVE-2026-61626 and CVE-2026-61627), adds automatic dropping of fully transparent images, and supports legacy Arabic Windows charmaps. 0.17.4 (2025-06) added `ass_prune_events`, `ass_configure_prune` and `ASS_OVERRIDE_BIT_BLUR`. 0.17.3 added `ass_malloc` and `ass_free`. https://github.com/libass/libass/releases
* Licence: **ISC**.
* **vcpkg port `libass` 0.17.5** (https://github.com/microsoft/vcpkg/tree/master/ports/libass):
  * dependencies `freetype`, `fribidi`, `harfbuzz`, and `fontconfig` (Linux only);
  * built with meson;
  * the font provider is **DirectWrite** on Windows and **CoreText** on macOS (picked automatically), **Fontconfig** on Linux.
* Dependency licences: FreeType (FTL or GPLv2; use FTL), **FriBidi LGPL-2.1+** (link dynamically: a vcpkg `x64-windows` DLL), HarfBuzz MIT, Fontconfig MIT.
* Optional dependencies:
  * `libunibreak` (Unicode line breaking, `-Dlibunibreak=enabled`; not enabled by the vcpkg port),
  * NASM for the x86 asm rasteriser (`-Dasm=enabled`, which noticeably speeds up blur and rasterising).
* Build options (meson): `fontconfig`, `directwrite`, `coretext`, `asm`, `libunibreak`, `require-system-font-provider`, `large-tiles`. Autotools is also supported on Unix.
* Linux: distro `libass-dev`, or build 0.17.5 with meson. macOS: `brew install libass`, or vcpkg.

### API essentials (`ass/ass.h`)

```c
ASS_Library  *lib = ass_library_init();
ass_set_message_cb(lib, cb, user);            // route log to our logger
ass_set_extract_fonts(lib, 1);                // use [Fonts] section / attachments
ass_add_font(lib, name, data, size);          // MKV attachments, user font dirs via ass_set_fonts_dir()

ASS_Renderer *r = ass_renderer_init(lib);
ass_set_storage_size(r, video_w, video_h);    // ORIGINAL video storage resolution (for aspect/blur/\be scaling, VSFilter compat)
ass_set_frame_size(r, out_w, out_h);          // the size of the bitmap we render into (widget/texture size)
ass_set_pixel_aspect(r, par);                 // only if anamorphic; ass_set_aspect_ratio() is deprecated
ass_set_fonts(r, NULL, "Arial", ASS_FONTPROVIDER_AUTODETECT, NULL, 1); // may be slow (fontconfig cache) -> worker thread
ass_set_shaper(r, ASS_SHAPING_COMPLEX);       // HarfBuzz shaping
ass_set_cache_limits(r, glyph_max, bitmap_max_mb);

ASS_Track *t = ass_read_memory(lib, buf, len, NULL /*codepage, NULL = UTF-8*/);
// incremental alternative: t = ass_new_track(lib); ass_process_data(t, data, len);

int changed;                                  // 0 = identical, 1 = positions changed, 2 = content changed
ASS_Image *img = ass_render_frame(r, t, time_ms, &changed);
for (; img; img = img->next) {
    // img->w, h, stride, bitmap (8-bit coverage), dst_x, dst_y, type
    // img->color = 0xRRGGBBAA where AA is TRANSPARENCY (0 = opaque):
    //   a = bitmap[x] * (255 - (color & 0xFF)) / 255;  blend colour RGB with alpha a
}
ass_free_track(t); ass_renderer_done(r); ass_library_done(lib);
```

Integration notes:
* `ASS_Image` memory belongs to the renderer and is valid until the next `ass_render_frame` call or until the renderer is freed.
* Renderer and track are **not thread-safe**. Keep one `ASS_Renderer` per render thread.
* The editor re-renders after every edit: either re-parse with `ass_read_memory()` from the serialised document (simple; ARCHITECTURE.md already plans this), or use `ass_flush_events()` followed by `ass_process_chunk()` per changed line.
* Use the `changed` value to skip texture re-uploads.
* The bitmap's last row may be unpadded. Only `stride*(h-1)+w` bytes are guaranteed.
* For VSFilter-exact colours, apply the `YCbCr Matrix` header handling described in `ass_types.h`, which matches Aegisub's colour-matrix logic.
* Use `ass_malloc`/`ass_free` for any memory exchanged with libass (multiple CRTs on Windows).

---

## 5. Rust crates: integration notes

### 5.1 cxx / cxx-build 1.0.205

* Add `cxx = "1.0.205"` to `fs-bridge` (`crate-type = ["staticlib"]`; Corrosion needs `staticlib` to import the crate).
* When Corrosion drives the bridge (§6), `cxx-build` is only needed for Rust-side C++ code. An example is `fs-media`, which compiles its BestSource/libass shim with `cxx_build::bridge("src/ffi.rs").files([...]).std("c++20").compile("fs_media_cpp")`.
* `cxx` supports up to `c++20` as a feature flag. Compiling the generated code as C++23 is fine.

### 5.2 mlua 0.12.2 (+ LuaJIT)

* Features:
  * `lua51 lua52 lua53 lua54 lua55 luajit` **`luajit52`** `luau luau-jit luau-vector4`
  * `vendored async send error-send serialize/serde macros module anyhow userdata-wrappers`
* **`luajit52` exists.** It implies `luajit` and builds LuaJIT with `-DLUAJIT_ENABLE_LUA52COMPAT` (on MSVC: `msvcbuild.bat lua52compat static`). **Aegisub builds its LuaJIT with `LUAJIT_ENABLE_LUA52COMPAT`** (arch1t3cht `subprojects/packagefiles/luajit/meson.build`), so use the same for Automation 4 script compatibility:

```toml
mlua = { version = "0.12.2", features = ["luajit52", "vendored", "send", "serialize"] }
```

* `vendored` goes through `mlua-sys 0.13.0`, which uses `luajit-src >=210.7.0,<210.8.0`. Currently that is **210.7.4+c6ffc14**: LuaJIT 2.1 rolling, commit `c6ffc141a876` from 2026-09-08 (the LuaJIT `v2.1` branch HEAD is `634daf245355`, 2026-10-13).
* The build is static. On Windows it uses `msvcbuild.bat`, so it must run from an environment where `cl.exe` is available (cargo invoked from a VS Developer prompt, or via the `cc` crate's env detection). On macOS it defaults to `MACOSX_DEPLOYMENT_TARGET` 11.0 (arm64) or 10.14 (x86_64).
* vcpkg also has a `luajit` port (version-date 2026-09-08, MIT), but it is not needed when vendored.

### 5.3 LPeg via `cc` (no crate exists)

* crates.io has **no `lpeg` crate**, so compile the **LPeg 1.1.0** C sources ourselves. Download https://www.inf.puc-rio.br/~roberto/lpeg/lpeg-1.1.0.tar.gz and vendor `third_party/lpeg-1.1.0/`.
* Sources: `lpcap.c lpcode.c lpcset.c lpprint.c lptree.c lpvm.c` (+ headers). `re.lua` is optional and pure Lua.
* LPeg 1.1.0 keeps Lua 5.1 compatibility (`#if (LUA_VERSION_NUM == 501)` in `lptypes.h`), so it works with LuaJIT.
* **Headers:** `mlua-sys` does not export the Lua include directory (luajit-src prints only link metadata, so there is no `DEP_LUA_INCLUDE`). Either:
  * (a) vendor LuaJIT's `lua.h`, `luaconf.h`, `lauxlib.h`, `lualib.h` and `luajit.h` from the same LuaJIT commit next to lpeg (recommended; these headers are stable across 2.1), or
  * (b) add `luajit-src = "=210.7.4"` as a build-dependency and call `luajit_src::Build::new().lua52compat(true).build()` only to get `include_dir()`. This is slower because it builds LuaJIT twice.
* The `lua_*` symbols resolve against the static LuaJIT that `mlua-sys` links into the same binary.

```rust
// build.rs (fs-automation)
cc::Build::new()
    .files(["lpcap.c","lpcode.c","lpcset.c","lpprint.c","lptree.c","lpvm.c"].map(|f| format!("third_party/lpeg-1.1.0/{f}")))
    .include("third_party/luajit-headers")
    .warnings(false)
    .compile("lpeg");

// registration
unsafe extern "C-unwind" { fn luaopen_lpeg(l: *mut mlua::ffi::lua_State) -> std::ffi::c_int; }
let open = unsafe { lua.create_c_function(luaopen_lpeg)? };
lua.globals().get::<mlua::Table>("package")?.get::<mlua::Table>("preload")?.set("lpeg", open)?;
```

### 5.4 Other crates

* **cpal 0.18.2**:
  * Backends: WASAPI on Windows (opt-in `asio`), CoreAudio on macOS, ALSA by default on Linux. The opt-in features `pipewire`, `pulseaudio` and `jack` need the system dev packages: `libasound2-dev`, `libpipewire-0.3-dev` + clang, `libpulse-dev`, `libjack-dev`. Windows 10 minimum.
  * Upstream's *Unreleased* changelog (after 0.18.2) renames `play()` to `start()` and merges the callback-info types. Pin `0.18.x`.
* **realfft 3.5.0** is built on rustfft ^6.4 (6.4.1) and provides the real-to-complex FFT for the spectrogram. SIMD features (`avx`, `sse`, `neon`) are on by default.
* **encoding_rs 0.8.42 + chardetng 1.0.0** handle charset detection and decoding of legacy `.ass`/`.srt` files. chardetng reached 1.0 in 2026-03.
* **fancy-regex 0.19.2** handles the Aegisub/Lua-style regex features, such as backreferences and look-around, that `regex 1.13.1` does not support.
* **insta 1.49.0** is used for parser/serialiser snapshot tests (install the `cargo-insta` CLI separately). Other test and bench crates: proptest 1.12.0, pretty_assertions 1.4.1, criterion 0.8.2.
* **cmake 0.1.58** is only needed if a crate must build a CMake project from `build.rs`. Corrosion does the opposite direction.

---

## 6. Corrosion (CMake ↔ Cargo)

* Latest **v0.6.1** (2026-01-17). v0.6.0 (2025-11-23) requires **CMake ≥ 3.22** (CMake 4.4 is fine), removed the native tooling, and fixed multi-config output directories. Licence MIT.
  * https://github.com/corrosion-rs/corrosion/releases
  * https://corrosion-rs.github.io/corrosion/

```cmake
include(FetchContent)
FetchContent_Declare(Corrosion GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git GIT_TAG v0.6.1)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(MANIFEST_PATH ${CMAKE_SOURCE_DIR}/Cargo.toml CRATES fs-bridge)   # fs-bridge: crate-type = ["staticlib"]

corrosion_add_cxxbridge(fs_bridge_cxx           # C++ target that Corrosion creates
    CRATE fs-bridge                             # imported Corrosion target (must be a staticlib/cdylib, not rlib-only)
    FILES lib.rs project.rs audio.rs)           # files with #[cxx::bridge], relative to the crate's src/

target_link_libraries(fsedit PRIVATE fs_bridge_cxx)   # also links the Rust staticlib
# C++: #include "fs_bridge_cxx/lib.h"  and  #include "rust/cxx.h"
```

`corrosion_add_cxxbridge` is still marked **EXPERIMENTAL** in `cmake/Corrosion.cmake`. How it works:
* It reads the `cxx` / `cxxbridge-cmd` version from `Cargo.lock` (`cargo tree -i`).
* It uses a `cxxbridge` binary of **exactly** that version from `PATH`, and otherwise builds `cxxbridge-cmd` of that version (currently 1.0.205) inside the build tree.
* It generates `rust/cxx.h` and one `.h`/`.cc` pair per file into `corrosion_generated/cxxbridge/<target>/`.
* It cannot pass `--cfg`, `--include` or `--cxx-impl-annotations`.
* Optional `REGEN_TARGET` regenerates the bindings without rebuilding.
* Tip: run `cargo install cxxbridge-cmd --version 1.0.205 --locked` once on dev machines and CI to avoid building it in every fresh build directory.

**MSVC CRT:** Rust's `x86_64-pc-windows-msvc` target links the dynamic release CRT (`/MD`). Keep the CMake side on the DLL CRT as well (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL$<$<CONFIG:Debug>:DebugDLL>`). In Debug configurations, expect the linker warning LNK4098 (msvcrt vs msvcrtd). This is harmless as long as no allocation or STL object crosses the boundary, and `cxx` guarantees that.

---

## 7. MoonScript and LPeg

| | Version | Date | Licence | Source |
|---|---|---|---|---|
| **MoonScript** | **v0.7.0** (luarocks `0.7.0-1`) | 2026-07-26 | MIT | https://github.com/leafo/moonscript/releases |
| MoonScript previous | v0.6.0 | 2026-01-10 | MIT | (first release since 0.5.0, 2016) |
| MoonScript master | 0.8.0-dev (`version.lua`) | 2026-08 | MIT | moves to the new parser https://github.com/leafo/moonscript-parser |
| **LPeg** | **1.1.0** (luarocks `1.1.0-2`) | 2023-06-26 | MIT | https://www.inf.puc-rio.br/~roberto/lpeg/ |

* MoonScript 0.7.0 is announced as **the last release that uses LPeg**. 0.7.0 adds destructuring in function arguments, syntax fixes and a better linter. 0.6.0 switched the CLI to argparse and added `moon -e` and `moonc --transform`.
* Rockspec dependencies: `lpeg >= 0.10, ~= 0.11`, plus `argparse >= 0.7` and `luafilesystem >= 1.5`. The last two are only needed by the `moon`/`moonc` CLI. Embedding the compiler (`require "moonscript.base"` → `to_lua` / `loadstring`) needs only `lpeg`.
* **Aegisub compatibility:** arch1t3cht/Aegisub still ships the amalgamated **MoonScript 0.5.0** (`automation/include/moonscript.lua`, `version = "0.5.0"`). It is compiled at runtime with LPeg. Existing `.moon` automation scripts (DependencyControl, TypesettingTools) target 0.5.0 semantics.
  * Recommendation: bundle **0.5.0 for maximum parity** (Aegisub's file, MIT), or 0.7.0 after running the real-script test suite planned in ARCHITECTURE.md phase 3.
  * Either way, LPeg 1.1.0 works. Aegisub uses LPeg too.

---

## 8. LuaJIT version used by mlua

* `mlua 0.12.2` → `mlua-sys 0.13.0` → `luajit-src >=210.7.0, <210.8.0`. The latest is **`210.7.4+c6ffc14`** (published 2026-09-27; previous releases were 210.7.3+1ee778a on 2026-08-29 and 210.7.2+b925b3e on 2026-05-30).
* Version scheme: `210` = LuaJIT **2.1.0** (the rolling 2.1 branch, with no numbered releases upstream any more). The build metadata `c6ffc14` is the upstream LuaJIT commit `c6ffc141a876` (2026-09-08).
* The vendored build is static. `luajit52` passes `LUAJIT_ENABLE_LUA52COMPAT`.
* Sources:
  * https://crates.io/crates/luajit-src
  * https://github.com/mlua-rs/luajit-src-rs
  * https://github.com/LuaJIT/LuaJIT/tree/v2.1

---

## 9. Action items

1. Uninstall or ignore Qt 6.7.3. Install **Qt 6.12.0 `win64_msvc2022_64`** with aqtinstall **from git master**, plus `qtshadertools`. Record `6.12.0` in `tools/qt-version.txt`.
2. Create `vcpkg.json` with `builtin-baseline: e456309491fd875487bf02b8a0dca76c1b1b00cc`, triplet `x64-windows` (dynamic), and the dependencies `ffmpeg` (LGPL feature set from §3, no `gpl`), `libass`, `xxhash`, plus an overlay port `bestsource` (R22, `-Denable_plugin=false -Denable_gpu=disabled`, libp2p vendored).
3. Pin Rust crates at the versions in table 0.2, with mlua features `luajit52, vendored`. Vendor LPeg 1.1.0 and the LuaJIT headers into `crates/fs-automation/third_party/`.
4. Pull in Corrosion v0.6.1 via FetchContent, and pre-install `cxxbridge-cmd 1.0.205` on CI.
5. On Linux CI, build FFmpeg 9.0.2 (or use the BtbN `linux64-lgpl-shared` build), because distro FFmpeg is likely too old for BestSource R22.
