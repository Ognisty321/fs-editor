//! Video and audio decoding (BestSource, frame/sample exact) and subtitle
//! rendering (libass) for FS Editor.
//!
//! The C++ side (`cpp/fsmedia.h`) is also used directly by the Qt GUI; this
//! crate wraps it for Rust (the CLI, tests, and the audio pipeline, which
//! feeds BestSource audio into `fs-audio`).

mod ffi;

use std::collections::HashSet;
use std::hash::{Hash, Hasher};
use std::path::Path;
use std::sync::Mutex;

use cxx::UniquePtr;
use ffi::{ProgressSink, bridge};

pub use bridge::{AudioProps, TrackInfo, VideoProps};

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

/// All streams in a media file.
pub fn list_tracks(path: &Path) -> Result<Vec<TrackInfo>> {
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

/// One embedded subtitle stream as file text plus its extension: an ASS
/// script for ASS/SSA streams, SRT for the rest.
pub fn extract_subtitles(path: &Path, index: i32) -> Result<(String, &'static str)> {
    use fs_core::time::Time;
    let mut packets = Vec::new();
    let header = bridge::subtitle_packets(path_str(path)?, index, &mut packets)?;
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
    Ok(bridge::attached_fonts(path_str(path)?)?
        .into_iter()
        .map(|f| (f.name, f.data))
        .collect())
}

/// Chapters of a Matroska/MP4 file, as (start in ms, title), by start.
pub fn chapters(path: &Path) -> Result<Vec<(i64, String)>> {
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
    pub fn open(
        path: &Path,
        track: i32,
        cache_dir: Option<&Path>,
        progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self> {
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
}

/// An audio track; implements [`fs_audio::AudioSource`] so it can be
/// loaded into a `PcmBuffer` for the waveform, spectrogram and player.
pub struct Audio {
    inner: Mutex<UniquePtr<bridge::AudioSource>>,
    props: AudioProps,
}

impl Audio {
    pub fn open(
        path: &Path,
        track: i32,
        cache_dir: Option<&Path>,
        progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self> {
        let mut sink = ProgressSink(Box::new(progress));
        let cache = cache_dir.map(path_str).transpose()?.unwrap_or("");
        let inner = bridge::open_audio(path_str(path)?, track, cache, &mut sink)?;
        let props = bridge::audio_props(&inner);
        Ok(Self {
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
