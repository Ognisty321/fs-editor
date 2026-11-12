//! Video and audio decoding (BestSource, frame/sample exact) and subtitle
//! rendering (libass) for FS Editor.
//!
//! The C++ side (`cpp/fsmedia.h`) is also used directly by the Qt GUI; this
//! crate wraps it for Rust (the CLI, tests, and the audio pipeline, which
//! feeds BestSource audio into `fs-audio`).
//!
//! An Avisynth (`.avs`) or VapourSynth (`.vpy`) script opens as video and audio like a file
//! does ([`Video::open`], [`Audio::open`]). Their libraries are not linked: each is loaded when
//! the first script of its kind is opened, and a missing one is an error that names it.

mod ffi;

use std::collections::HashSet;
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

use cxx::UniquePtr;
use ffi::{ProgressSink, bridge};

pub use bridge::{AudioProps, PlaneLayout, TrackInfo, VideoProps};

#[derive(Debug, thiserror::Error)]
pub enum MediaError {
    #[error("{0}")]
    Decoder(String),
    #[error("path is not valid UTF-8: {0}")]
    Path(String),
}

impl From<cxx::Exception> for MediaError {
    fn from(e: cxx::Exception) -> Self {
        MediaError::Decoder(e.what().to_string())
    }
}

pub type Result<T> = std::result::Result<T, MediaError>;

fn path_str(p: &Path) -> Result<&str> {
    p.to_str()
        .ok_or_else(|| MediaError::Path(p.display().to_string()))
}

/// Default directory for BestSource index files.
pub fn default_cache_dir() -> std::path::PathBuf {
    let base = std::env::var_os("LOCALAPPDATA")
        .or_else(|| std::env::var_os("XDG_CACHE_HOME"))
        .map(std::path::PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| Path::new(&h).join(".cache")))
        .unwrap_or_else(std::env::temp_dir);
    base.join("fs-editor").join("bsindex")
}

/// Aegisub's CleanCache: the oldest files ending in `ext` in `dir` go until at most `max_files`
/// of at most `max_bytes` in all are left (0: no limit on the number); the newest always stays.
/// Those starting with `used` (the file just opened) are dated now first, so the ones used
/// longest ago go, not the index in use. Returns how many went.
pub fn clean_cache(dir: &Path, ext: &str, used: &str, max_bytes: u64, max_files: usize) -> usize {
    if !used.is_empty() {
        for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            if e.file_name().to_string_lossy().starts_with(used) {
                let _ = std::fs::File::options()
                    .write(true)
                    .open(e.path())
                    .and_then(|f| f.set_modified(std::time::SystemTime::now()));
            }
        }
    }
    let max_files = if max_files == 0 {
        usize::MAX
    } else {
        max_files
    };
    let mut files: Vec<(std::time::SystemTime, u64, std::path::PathBuf)> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().ends_with(ext))
        .filter_map(|e| {
            let m = e.metadata().ok().filter(|m| m.is_file())?;
            Some((m.modified().ok()?, m.len(), e.path()))
        })
        .collect();
    files.sort();
    let mut total: u64 = files.iter().map(|f| f.1).sum();
    let mut left = files.len();
    let mut gone = 0;
    for (_, size, path) in &files {
        if (total <= max_bytes && left <= max_files) || left < 2 {
            break;
        }
        if std::fs::remove_file(path).is_ok() {
            total -= size;
            left -= 1;
            gone += 1;
        }
    }
    gone
}

#[cfg(test)]
mod tests {
    #[test]
    fn clean_cache_keeps_the_newest_within_limits() {
        let dir = tempfile::tempdir().unwrap();
        let t0 = std::time::SystemTime::now() - std::time::Duration::from_secs(100);
        for (k, size) in [10usize, 20, 30, 40].into_iter().enumerate() {
            let p = dir.path().join(format!("{k}.bsindex"));
            std::fs::write(&p, vec![0u8; size]).unwrap();
            let f = std::fs::File::options().write(true).open(&p).unwrap();
            f.set_modified(t0 + std::time::Duration::from_secs(k as u64))
                .unwrap();
        }
        std::fs::write(dir.path().join("other.txt"), "x").unwrap();
        // three files at most: the oldest goes; then 70 bytes at most: the next one too
        assert_eq!(super::clean_cache(dir.path(), ".bsindex", "", 1000, 3), 1);
        // the oldest left was just used: the next one goes in its place
        assert_eq!(super::clean_cache(dir.path(), ".bsindex", "1.", 1000, 2), 1);
        assert!(dir.path().join("1.bsindex").exists() && !dir.path().join("2.bsindex").exists());
        // the newest (now 1) stays even over the size
        assert_eq!(super::clean_cache(dir.path(), ".bsindex", "", 0, 0), 1);
        assert!(dir.path().join("1.bsindex").exists() && dir.path().join("other.txt").exists());
    }
}

/// The script language of a file, by its extension.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ScriptKind {
    /// `.avs`, run by Avisynth (AviSynth+).
    Avisynth,
    /// `.vpy`, run by VapourSynth through VSScript.
    VapourSynth,
}

pub fn script_kind(path: &Path) -> Option<ScriptKind> {
    let ext = path.extension()?.to_str()?;
    if ext.eq_ignore_ascii_case("avs") {
        Some(ScriptKind::Avisynth)
    } else if ext.eq_ignore_ascii_case("vpy") {
        Some(ScriptKind::VapourSynth)
    } else {
        None
    }
}

/// How scripts are opened; read at each open.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ScriptOptions {
    /// The VSScript library of the VapourSynth to use. Empty: the one `VSSCRIPT_PATH` names,
    /// the one in the registry (Windows), the library path, then the one Python tells of.
    pub vsscript: PathBuf,
    /// Aegisub's "Avisynth memory limit" in MB; 0 leaves Avisynth's own.
    pub avs_memory_max_mb: i32,
    /// Aegisub's "Allow pre-2.56a Avisynth": here one older than 2.6, which converts the
    /// frames to RGB itself.
    pub avs_allow_ancient: bool,
    /// Aegisub's "Force sample rate" of Avisynth audio; 0 keeps the script's, but 44100 for
    /// one under 32000 as Aegisub does.
    pub avs_sample_rate: i32,
    /// Aegisub's "Avisynth down-mixer", a function applied to the audio first (`ConvertToMono`,
    /// `GetLeftChannel`, `GetRightChannel`). Empty: the channels as the script gives them.
    pub avs_downmixer: String,
}

static VSSCRIPT_NAMED: AtomicBool = AtomicBool::new(false);

pub fn set_script_options(o: &ScriptOptions) {
    let vsscript = o.vsscript.to_str().unwrap_or("");
    VSSCRIPT_NAMED.store(!vsscript.is_empty(), Ordering::Relaxed);
    bridge::set_script_options(
        vsscript,
        o.avs_memory_max_mb,
        o.avs_allow_ancient,
        o.avs_sample_rate,
        &o.avs_downmixer,
    );
}

/// Loads the library that runs scripts of `kind`, unless it is loaded already. The error says
/// what is missing and what to install.
pub fn load_script_library(kind: ScriptKind) -> Result<()> {
    match kind {
        ScriptKind::Avisynth => Ok(bridge::load_avisynth()?),
        ScriptKind::VapourSynth => {
            let not_found = match bridge::load_vapoursynth("") {
                Ok(()) => return Ok(()),
                Err(e) => e,
            };
            // installed with pip and not registered (`vapoursynth register-install`), or
            // registered for a Python that is gone: the Python on the path knows where it is
            if !VSSCRIPT_NAMED.load(Ordering::Relaxed)
                && let Some(lib) = python_vsscript()
            {
                return Ok(bridge::load_vapoursynth(lib)?);
            }
            Err(not_found.into())
        }
    }
}

/// The VSScript library of the VapourSynth a Python on the path has (R73 and newer tell it).
fn python_vsscript() -> Option<&'static str> {
    use std::io::Read;
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};
    static FOUND: OnceLock<Option<String>> = OnceLock::new();
    let ask = |exe: &str, first: &[&str]| -> Option<String> {
        let mut cmd = Command::new(exe);
        cmd.args(first)
            .args(["-m", "vapoursynth", "get-vsscript"])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console flashes over the editor
        }
        let mut child = cmd.spawn().ok()?;
        let give_up = Instant::now() + Duration::from_secs(20);
        loop {
            match child.try_wait() {
                Ok(Some(status)) if status.success() => break,
                Ok(None) if Instant::now() < give_up => {
                    std::thread::sleep(Duration::from_millis(20));
                }
                _ => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return None;
                }
            }
        }
        let mut out = String::new();
        child.stdout.take()?.read_to_string(&mut out).ok()?;
        let lib = out.trim();
        (!lib.is_empty() && Path::new(lib).is_file()).then(|| lib.to_string())
    };
    FOUND
        .get_or_init(|| {
            ask("python", &[])
                .or_else(|| ask("python3", &[]))
                .or_else(|| ask("py", &["-3"]))
        })
        .as_deref()
}

/// True when the script was opened as it is now, by any of [`Video::open`], [`Audio::open`] and
/// [`list_tracks`]: its tracks are known then, and [`list_tracks`] does not run it again.
pub fn script_evaluated(path: &Path) -> bool {
    path.to_str().is_some_and(bridge::script_evaluated)
}

/// All streams in a media file. A script has a video and an audio track for what it outputs.
pub fn list_tracks(path: &Path) -> Result<Vec<TrackInfo>> {
    if let Some(kind) = script_kind(path) {
        load_script_library(kind)?;
    }
    Ok(bridge::list_tracks(path_str(path)?)?)
}

/// Text subtitle streams of a file (ASS/SSA/SRT and plain text); bitmap
/// formats such as PGS cannot be edited and are left out.
pub fn subtitle_tracks(path: &Path) -> Result<Vec<TrackInfo>> {
    const TEXT: [&str; 6] = ["ass", "ssa", "subrip", "srt", "text", "webvtt"];
    Ok(list_tracks(path)?
        .into_iter()
        .filter(|t| t.kind == "subtitle" && TEXT.contains(&t.codec.as_str()))
        .collect())
}

/// How a subtitle stream's packets are found in the file.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SubtitleRead {
    /// From the file's index where it has one that names every block of the stream (Matroska
    /// written by FFmpeg or mkvmerge): a seek and a few bytes per line. As packets otherwise.
    Auto = 0,
    /// As packets, which reads the whole file, video and audio with it.
    Packets = 1,
    /// From the index, or an error.
    Index = 2,
}

/// One embedded subtitle stream as file text plus its extension: an ASS
/// script for ASS/SSA streams, SRT for the rest.
pub fn extract_subtitles(path: &Path, index: i32) -> Result<(String, &'static str)> {
    extract_subtitles_with(path, index, SubtitleRead::Auto, |_, _| true)
}

/// [`extract_subtitles`], read as `how` says; `progress(done, total)` returns false to cancel,
/// which is an error.
pub fn extract_subtitles_with(
    path: &Path,
    index: i32,
    how: SubtitleRead,
    progress: impl FnMut(i64, i64) -> bool,
) -> Result<(String, &'static str)> {
    use fs_core::time::Time;
    let mut packets = Vec::new();
    let mut sink = ProgressSink(Box::new(progress));
    let header =
        bridge::subtitle_packets(path_str(path)?, index, how as i32, &mut sink, &mut packets)?;
    drop(sink);
    let times = |p: &bridge::SubPacket| {
        let start = Time::from_ms(p.start_ms);
        (start, Time::from_ms(p.start_ms + p.duration_ms.max(0)))
    };
    if !header.contains("[Script Info]") {
        let mut out = String::new();
        for (n, p) in packets.iter().enumerate() {
            let (a, b) = times(p);
            let text = p.text.trim_end_matches(['\r', '\n', '\0']);
            out += &format!("{}\n{} --> {}\n{text}\n\n", n + 1, a.to_srt(), b.to_srt());
        }
        return Ok((out, "srt"));
    }
    // Matroska ASS blocks: ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text
    let mut lines: Vec<(i64, String)> = packets
        .iter()
        .enumerate()
        .map(|(n, p)| {
            let mut f = p.text.splitn(3, ',');
            let order = f
                .next()
                .and_then(|o| o.trim().parse().ok())
                .unwrap_or(n as i64);
            let layer = f.next().unwrap_or("0");
            let rest = f.next().unwrap_or("").trim_end_matches(['\r', '\n', '\0']);
            let (a, b) = times(p);
            (
                order,
                format!("Dialogue: {layer},{},{},{rest}\n", a.to_ass(), b.to_ass()),
            )
        })
        .collect();
    lines.sort_by_key(|l| l.0);
    let mut out = header.trim_end_matches('\0').trim_end().to_owned();
    out.push('\n');
    if !out.contains("[Events]") {
        out += "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    }
    out.extend(lines.into_iter().map(|l| l.1));
    Ok((out, "ass"))
}

/// Fonts attached to a Matroska file, as (file name, data).
pub fn attached_fonts(path: &Path) -> Result<Vec<(String, Vec<u8>)>> {
    if script_kind(path).is_some() {
        return Ok(Vec::new());
    }
    Ok(bridge::attached_fonts(path_str(path)?)?
        .into_iter()
        .map(|f| (f.name, f.data))
        .collect())
}

/// Chapters of a Matroska/MP4 file, as (start in ms, title), by start.
pub fn chapters(path: &Path) -> Result<Vec<(i64, String)>> {
    if script_kind(path).is_some() {
        return Ok(Vec::new());
    }
    Ok(bridge::chapters(path_str(path)?)?
        .into_iter()
        .map(|c| (c.start_ms, c.title))
        .collect())
}

/// YUV→RGB matrix override (Aegisub's "Override colour matrix").
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Matrix {
    #[default]
    Auto = 0,
    Bt601 = 1,
    Bt709 = 2,
    Fcc = 3,
    Smpte240m = 4,
    Bt2020 = 5,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Range {
    #[default]
    AsDecoded = 0,
    Tv = 1,
    Pc = 2,
}

pub struct Video {
    inner: UniquePtr<bridge::VideoSource>,
    props: VideoProps,
    timestamps: Vec<i64>,
    keyframes: Vec<i64>,
}

impl Video {
    /// Open and index a video track (`track` < 0: n-th video track, -1 =
    /// first). `progress(done, total)` returns false to cancel. With a
    /// cache directory the index is stored and reused on the next open, and a
    /// Matroska/MP4 file not indexed yet opens from its packets at once while
    /// BestSource indexes it in the background ([`Video::is_exact`]).
    ///
    /// An `.avs` or `.vpy` file is evaluated and its clip served: nothing is indexed, the
    /// frame rate is the clip's and the keyframes are the ones the script names (VapourSynth:
    /// `__aegi_keyframes`, with `__aegi_timecodes` for the frames' times).
    pub fn open(
        path: &Path,
        track: i32,
        cache_dir: Option<&Path>,
        progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self> {
        if let Some(kind) = script_kind(path) {
            load_script_library(kind)?;
        }
        let mut sink = ProgressSink(Box::new(progress));
        let cache = cache_dir.map(path_str).transpose()?.unwrap_or("");
        let inner = bridge::open_video(path_str(path)?, track, cache, &mut sink)?;
        let props = bridge::video_props(&inner);
        let timestamps = bridge::video_timestamps(&inner);
        let keyframes = bridge::video_keyframes(&inner);
        Ok(Self {
            inner,
            props,
            timestamps,
            keyframes,
        })
    }

    pub fn props(&self) -> &VideoProps {
        &self.props
    }
    pub fn width(&self) -> usize {
        self.props.width as usize
    }
    pub fn height(&self) -> usize {
        self.props.height as usize
    }
    pub fn num_frames(&self) -> i64 {
        self.props.num_frames
    }
    /// Start time of every frame in ms, relative to the first frame.
    pub fn timestamps_ms(&self) -> &[i64] {
        &self.timestamps
    }
    pub fn keyframes(&self) -> &[i64] {
        &self.keyframes
    }
    pub fn linear_decoding(&self) -> bool {
        bridge::video_linear(&self.inner)
    }
    /// False while a file opened from its packets waits for BestSource's exact index.
    pub fn is_exact(&self) -> bool {
        bridge::video_exact(&self.inner)
    }

    /// True when the frame timestamps do not follow the nominal frame rate
    /// (more than 1 ms off — container timestamps are rounded to 1 ms).
    pub fn is_vfr(&self) -> bool {
        let (num, den) = (
            self.props.fps_num.max(1) as i64,
            self.props.fps_den.max(1) as i64,
        );
        let t0 = self.timestamps.first().copied().unwrap_or(0);
        self.timestamps
            .iter()
            .enumerate()
            .any(|(n, &t)| ((t - t0) - n as i64 * 1000 * den / num).abs() > 1)
    }

    /// Frame rate for timing: the real per-frame timestamps (VFR aware),
    /// falling back to the nominal rate when they are unusable.
    pub fn framerate(&self) -> fs_core::vfr::Framerate {
        use fs_core::vfr::Framerate;
        let mut tc = self.timestamps.clone();
        tc.dedup();
        if tc.len() == self.timestamps.len()
            && tc.len() > 1
            && let Ok(f) = Framerate::from_timecodes(tc)
        {
            return f;
        }
        Framerate::from_rational(
            self.props.fps_num.max(1) as i64,
            self.props.fps_den.max(1) as i64,
            false,
        )
        .unwrap_or_else(|_| Framerate::from_rational(24000, 1001, false).unwrap())
    }

    pub fn set_matrix(&mut self, matrix: Matrix, range: Range) {
        bridge::video_set_matrix(self.inner.pin_mut(), matrix as u8, range as u8);
    }

    /// Decode frame `n` as RGBA8 into `out` (`width*height*4` bytes).
    pub fn frame_into(&mut self, n: i64, out: &mut [u8]) -> Result<()> {
        Ok(bridge::video_frame_rgba(self.inner.pin_mut(), n, out)?)
    }

    pub fn frame(&mut self, n: i64) -> Result<Vec<u8>> {
        let mut v = vec![0u8; self.width() * self.height() * 4];
        self.frame_into(n, &mut v)?;
        Ok(v)
    }

    /// Decode frame `n` and copy its planes as the decoder gives them into `y`, `u` and `v`, row
    /// after row: what a shader makes the RGB of [`frame_into`](Self::frame_into) from, without
    /// the conversion on the CPU. The answer says how (its matrix has the range and what
    /// [`set_matrix`](Self::set_matrix) overrides), whether the frame has such planes at all, and
    /// whether they were copied: a buffer shorter than its plane leaves all three untouched.
    pub fn planes_into(
        &mut self,
        n: i64,
        y: &mut [u8],
        u: &mut [u8],
        v: &mut [u8],
    ) -> Result<PlaneLayout> {
        Ok(bridge::video_frame_planes(
            self.inner.pin_mut(),
            n,
            y,
            u,
            v,
        )?)
    }

    /// [`planes_into`](Self::planes_into) with buffers of its own; `None` for a frame without
    /// planes.
    pub fn planes(&mut self, n: i64) -> Result<Option<Planes>> {
        let layout = self.planes_into(n, &mut [], &mut [], &mut [])?;
        if !layout.planar {
            return Ok(None);
        }
        let (luma, chroma) = layout.sizes();
        let mut p = Planes {
            layout,
            y: vec![0; luma],
            u: vec![0; chroma],
            v: vec![0; chroma],
        };
        p.layout = self.planes_into(n, &mut p.y, &mut p.u, &mut p.v)?;
        Ok((p.layout.planar && p.layout.copied).then_some(p))
    }
}

impl PlaneLayout {
    /// The bytes of the luma plane and of each chroma plane.
    pub fn sizes(&self) -> (usize, usize) {
        let n = |w: i32, h: i32| w.max(0) as usize * h.max(0) as usize * self.bytes.max(1) as usize;
        (
            n(self.width, self.height),
            n(self.chroma_width, self.chroma_height),
        )
    }
}

/// A frame's planes ([`Video::planes`]).
pub struct Planes {
    pub layout: PlaneLayout,
    pub y: Vec<u8>,
    pub u: Vec<u8>,
    pub v: Vec<u8>,
}

/// What [`SubtitleRenderer::overlay_box`] found at a time.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Overlay {
    /// Nothing shows.
    Empty,
    /// Drawn: `[left, top, right, bottom]` of the part of the frame the buffer now starts with.
    Drawn([i32; 4]),
    /// What the call before found, and nothing was written.
    Same,
}

/// An audio track; implements [`fs_audio::AudioSource`] so it can be
/// loaded into a `PcmBuffer` for the waveform, spectrogram and player.
pub struct Audio {
    inner: Mutex<UniquePtr<bridge::AudioSource>>,
    cancel: cxx::SharedPtr<bridge::AudioCancel>,
    props: AudioProps,
}

impl Audio {
    /// The audio track `track` of a file, or the audio an `.avs` or `.vpy` script outputs.
    pub fn open(
        path: &Path,
        track: i32,
        cache_dir: Option<&Path>,
        progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self> {
        if let Some(kind) = script_kind(path) {
            load_script_library(kind)?;
        }
        let mut sink = ProgressSink(Box::new(progress));
        let cache = cache_dir.map(path_str).transpose()?.unwrap_or("");
        let inner = bridge::open_audio(path_str(path)?, track, cache, &mut sink)?;
        let props = bridge::audio_props(&inner);
        Ok(Self {
            cancel: bridge::audio_canceller(&inner),
            inner: Mutex::new(inner),
            props,
        })
    }

    pub fn props(&self) -> &AudioProps {
        &self.props
    }

    /// Interleaved samples from `start`; errors from the decoder are
    /// returned rather than turned into silence.
    pub fn read(&self, start: i64, out: &mut [f32]) -> Result<()> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        Ok(bridge::audio_fill(g.pin_mut(), start, out)?)
    }

    /// The same samples averaged over the channels as 16-bit, what [`fs_audio::PcmBuffer`] keeps.
    pub fn read_mono(&self, start: i64, out: &mut [i16]) -> Result<()> {
        let mut g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        Ok(bridge::audio_fill_mono(g.pin_mut(), start, out)?)
    }

    /// False once BestSource gave up on seeking in this track (AC-3, DTS-HD MA): a read away
    /// from the last one then decodes from the start of the file.
    pub fn seeks(&self) -> bool {
        let g = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        bridge::audio_seeks(&g)
    }

    /// From any thread: the read under way fails at its next frame, as do the ones after it. For
    /// a seek BestSource retries for seconds in a track it cannot seek in.
    pub fn cancel(&self) {
        if let Some(c) = self.cancel.as_ref() {
            bridge::audio_cancel(c);
        }
    }
}

impl fs_audio::AudioSource for Audio {
    fn sample_rate(&self) -> u32 {
        self.props.sample_rate as u32
    }
    fn channels(&self) -> u16 {
        self.props.channels as u16
    }
    fn num_frames(&self) -> i64 {
        self.props.num_samples
    }
    fn fill(&self, start: i64, out: &mut [f32]) {
        if self.read(start, out).is_err() {
            out.fill(0.0);
        }
    }
    fn fill_mono(&self, start: i64, out: &mut [i16]) {
        if self.read_mono(start, out).is_err() {
            out.fill(0);
        }
    }
    fn seeks(&self) -> bool {
        Audio::seeks(self)
    }
    fn cancel(&self) {
        Audio::cancel(self);
    }
}

/// libass renderer for one script.
pub struct SubtitleRenderer {
    inner: UniquePtr<bridge::SubtitleRenderer>,
    /// Hashes of the fonts given to libass, which keeps every copy it gets until it is dropped.
    fonts: HashSet<u64>,
}

impl SubtitleRenderer {
    pub fn new() -> Result<Self> {
        Ok(Self {
            inner: bridge::new_subtitle_renderer()?,
            fonts: HashSet::new(),
        })
    }

    pub fn set_fonts(&mut self, default_family: &str, fonts_dir: Option<&Path>) -> Result<()> {
        let dir = fonts_dir.map(path_str).transpose()?.unwrap_or("");
        bridge::subs_set_fonts(self.inner.pin_mut(), default_family, dir);
        Ok(())
    }

    pub fn add_font(&mut self, name: &str, data: &[u8]) {
        self.add_fonts([(name, data)]);
    }

    /// Register fonts (Matroska attachments, a script's `[Fonts]`); ones already registered are
    /// skipped, and the font selection is rebuilt once.
    pub fn add_fonts<'a>(&mut self, fonts: impl IntoIterator<Item = (&'a str, &'a [u8])>) {
        let mut added = false;
        for (name, data) in fonts {
            let mut h = std::collections::hash_map::DefaultHasher::new();
            data.hash(&mut h);
            if self.fonts.insert(h.finish()) {
                bridge::subs_add_font(self.inner.pin_mut(), name, data);
                added = true;
            }
        }
        if added {
            bridge::subs_refresh_fonts(self.inner.pin_mut());
        }
    }

    pub fn set_script(&mut self, ass: &str) -> Result<()> {
        Ok(bridge::subs_set_script(self.inner.pin_mut(), ass)?)
    }

    /// Composite subtitles at `ms` onto an RGBA8 frame of `width x height`
    /// (the video's native size is `video` for scaling rules). Returns true
    /// if anything was drawn.
    pub fn blend(
        &mut self,
        ms: i64,
        width: usize,
        height: usize,
        video: (usize, usize),
        rgba: &mut [u8],
    ) -> Result<bool> {
        Ok(bridge::subs_blend(
            self.inner.pin_mut(),
            ms,
            width as i32,
            height as i32,
            video.0 as i32,
            video.1 as i32,
            rgba,
        )?)
    }

    /// What the subtitles cover at `ms` in a `width x height` frame: `[left, top, right, bottom]`
    /// (right and bottom exclusive) of the text with its borders, no shadows, then of the text
    /// alone (the first again when only borders show).
    pub fn bounds(&mut self, ms: i64, width: usize, height: usize) -> Result<Option<[i32; 8]>> {
        let mut b = [0i32; 8];
        let shown = bridge::subs_bounds(
            self.inner.pin_mut(),
            ms,
            width as i32,
            height as i32,
            &mut b,
        )?;
        Ok(shown.then_some(b))
    }

    /// The subtitles at `ms` alone, as [`overlay`](Self::overlay) draws them, but only the part
    /// of the frame they cover, at the start of `rgba` (which must hold a whole frame): what a
    /// view draws over a picture it converts itself. [`Overlay::Same`] when libass saw no change
    /// since the call before this one and no other call of this renderer came between.
    pub fn overlay_box(
        &mut self,
        ms: i64,
        width: usize,
        height: usize,
        video: (usize, usize),
        rgba: &mut [u8],
    ) -> Result<Overlay> {
        let mut b = [0i32; 4];
        let found = bridge::subs_overlay_box(
            self.inner.pin_mut(),
            ms,
            width as i32,
            height as i32,
            video.0 as i32,
            video.1 as i32,
            rgba,
            &mut b,
        )?;
        Ok(match found {
            1 => Overlay::Drawn(b),
            2 => Overlay::Same,
            _ => Overlay::Empty,
        })
    }

    /// Render into a transparent premultiplied RGBA8 overlay.
    pub fn overlay(
        &mut self,
        ms: i64,
        width: usize,
        height: usize,
        video: (usize, usize),
        rgba: &mut [u8],
    ) -> Result<bool> {
        Ok(bridge::subs_overlay(
            self.inner.pin_mut(),
            ms,
            width as i32,
            height as i32,
            video.0 as i32,
            video.1 as i32,
            rgba,
        )?)
    }
}
