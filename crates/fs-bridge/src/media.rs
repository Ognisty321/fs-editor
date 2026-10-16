//! Video, subtitle renderer and audio wrappers for the GUI.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

/// Kainote's external fonts folder: its fonts render without being installed. Read by each new
/// renderer, so it applies to videos opened after it is set.
static FONTS_DIR: RwLock<String> = RwLock::new(String::new());
/// The output device audio plays on (Aegisub's and Kainote's choice), "" the system's default;
/// for audio opened after it is set.
static AUDIO_DEVICE: RwLock<String> = RwLock::new(String::new());

fn put(lock: &RwLock<String>, v: &str) {
    *lock.write().unwrap_or_else(|e| e.into_inner()) = v.to_string();
}

fn get(lock: &RwLock<String>) -> Option<String> {
    let v = lock.read().unwrap_or_else(|e| e.into_inner());
    (!v.is_empty()).then(|| v.clone())
}

pub fn set_fonts_dir(dir: &str) {
    put(&FONTS_DIR, dir);
}

fn fonts_dir() -> Option<PathBuf> {
    get(&FONTS_DIR).map(PathBuf::from)
}

pub fn set_audio_device(name: &str) {
    put(&AUDIO_DEVICE, name);
}

use fs_audio::{
    AudioSource, FreqScale, PcmBuffer, Peak, PeakCache, Player, Spectrogram, SpectrumConfig,
    SpectrumView,
};
use fs_core::Document;
use fs_core::ass::Section;
use fs_core::vfr::{Framerate, TimeType};

use crate::ffi::VideoInfo;

fn cache(dir: &str) -> PathBuf {
    if dir.is_empty() {
        fs_media::default_cache_dir()
    } else {
        PathBuf::from(dir)
    }
}

/// Kainote's preferred audio tracks: the first name of `preferred` (";"-separated, in order) that is
/// a track's language or part of its title picks it; -1 when none does.
pub fn preferred_track<'a>(tracks: &[(i32, &'a str, &'a str)], preferred: &str) -> i32 {
    preferred
        .split(';')
        .map(|p| p.trim().to_lowercase())
        .filter(|p| !p.is_empty())
        .find_map(|p| {
            tracks.iter().find(|(_, lang, title)| {
                lang.to_lowercase() == p || title.to_lowercase().contains(&p)
            })
        })
        .map_or(-1, |t| t.0)
}

/// The header in Aegisub's spelling ("TV.601", "None"); empty when it is missing or invalid.
pub fn ycbcr_header(header: &str) -> String {
    let h = header.trim().to_ascii_lowercase();
    if h == "none" {
        return "None".into();
    }
    let Some((range, matrix)) = h.split_once('.') else {
        return String::new();
    };
    let range = match range {
        "tv" => "TV",
        "pc" => "PC",
        _ => return String::new(),
    };
    let matrix = match matrix {
        "601" => "601",
        "709" => "709",
        "fcc" => "FCC",
        "240m" => "240M",
        _ => return String::new(),
    };
    format!("{range}.{matrix}")
}

/// What a video is decoded with for a script's header: its own colour space for None, VSFilter's
/// TV.601 when the header is missing or invalid (Aegisub's `Header::to_effective`).
pub fn decode_as(header: &str) -> (fs_media::Matrix, fs_media::Range) {
    use fs_media::{Matrix, Range};
    let h = ycbcr_header(header);
    if h == "None" {
        return (Matrix::Auto, Range::AsDecoded);
    }
    let range = if h.starts_with("PC") {
        Range::Pc
    } else {
        Range::Tv
    };
    let matrix = match h.split_once('.').map_or("", |(_, m)| m) {
        "709" => Matrix::Bt709,
        "FCC" => Matrix::Fcc,
        "240M" => Matrix::Smpte240m,
        _ => Matrix::Bt601,
    };
    (matrix, range)
}

/// The header a script on this video should have (Aegisub's `to_best_practice` of the guessed
/// colour space): TV/PC.601 or .709, None for anything else. `colorspace` and `range` are
/// AVColorSpace and AVColorRange; unspecified ones are guessed as Aegisub does.
pub fn best_matrix(colorspace: i32, range: i32, width: i32, height: i32) -> &'static str {
    // AVColorSpace: 1 BT.709, 2 unspecified, 5 BT.470BG, 6 SMPTE 170M; AVColorRange 2: full
    let cs = match colorspace {
        2 if width > 1024 || height >= 600 => 1,
        2 => 6,
        c => c,
    };
    match (cs, range == 2) {
        (1, false) => "TV.709",
        (1, true) => "PC.709",
        (5 | 6, false) => "TV.601",
        (5 | 6, true) => "PC.601",
        _ => "None",
    }
}

pub struct Video {
    inner: fs_media::Video,
    fr: Framerate,
}

impl Video {
    /// `progress(done, total)` while indexing; false cancels.
    pub fn open(
        path: &str,
        cache_dir: &str,
        progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self, String> {
        let inner = fs_media::Video::open(Path::new(path), -1, Some(&cache(cache_dir)), progress)
            .map_err(|e| format!("{path}: {e}"))?;
        let fr = inner.framerate();
        Ok(Video { inner, fr })
    }

    pub fn info(&self) -> VideoInfo {
        let p = self.inner.props();
        VideoInfo {
            width: p.width,
            height: p.height,
            frames: p.num_frames,
            fps: self.fr.fps(),
            vfr: self.inner.is_vfr(),
            sar_num: p.sar_num,
            sar_den: p.sar_den,
            duration_ms: (p.duration_s * 1000.0).round() as i64,
            matrix: p.matrix.clone(),
            untagged: p.colorspace == 2,
            hdr: p.hdr,
            best_matrix: best_matrix(p.colorspace, p.color_range, p.width, p.height).to_string(),
        }
    }

    /// Decode with the colour space a script's YCbCr Matrix header names (Aegisub's
    /// `override_colorspace`), so its colours match the video as VSFilter shows them.
    pub fn set_ycbcr_header(&mut self, header: &str) {
        let (m, r) = decode_as(header);
        self.inner.set_matrix(m, r);
    }

    pub fn frame(&mut self, n: i64, out: &mut [u8]) -> Result<(), String> {
        let n = n.clamp(0, (self.inner.num_frames() - 1).max(0));
        self.inner.frame_into(n, out).map_err(|e| e.to_string())
    }

    pub fn timecodes(&self) -> Vec<i64> {
        self.inner.timestamps_ms().to_vec()
    }

    pub fn keyframes(&self) -> Vec<i64> {
        self.inner.keyframes().to_vec()
    }

    /// Frame shown at `ms`.
    pub fn frame_at_ms(&self, ms: i64) -> i64 {
        self.fr
            .frame_at_time(ms, TimeType::Exact)
            .clamp(0, (self.inner.num_frames() - 1).max(0))
    }

    pub fn ms_at_frame(&self, frame: i64) -> i64 {
        self.fr.time_at_frame(frame, TimeType::Exact)
    }
}

pub struct Renderer {
    ass: fs_media::SubtitleRenderer,
    /// The `[Fonts]` section last registered.
    section: Option<Arc<Section>>,
    /// The names of the script's fonts registered, to notice one taken out.
    script_fonts: HashSet<String>,
    /// The media whose attached fonts are registered, to register them again after a reset.
    media: Option<String>,
}

impl Renderer {
    pub fn new() -> Result<Self, String> {
        Ok(Renderer {
            ass: Self::fresh()?,
            section: None,
            script_fonts: HashSet::new(),
            media: None,
        })
    }

    fn fresh() -> Result<fs_media::SubtitleRenderer, String> {
        let mut r = fs_media::SubtitleRenderer::new().map_err(|e| e.to_string())?;
        r.set_fonts("Arial", fonts_dir().as_deref())
            .map_err(|e| e.to_string())?;
        Ok(r)
    }

    /// Register fonts attached to an MKV (silently nothing for other files).
    pub fn load_attached_fonts(&mut self, media_path: &str) {
        let fonts = fs_media::attached_fonts(Path::new(media_path)).unwrap_or_default();
        (self.ass).add_fonts(fonts.iter().map(|(n, d)| (n.as_str(), d.as_slice())));
        self.media = Some(media_path.to_string());
    }

    /// The subtitles' script and embedded fonts (which the script leaves out; decoded again
    /// only when the `[Fonts]` section changed). Runs on the render thread.
    pub fn load_subs(&mut self, subs: &crate::session::RenderSubs) -> Result<(), String> {
        self.add_script_fonts(&subs.doc);
        self.set_script(&subs.script())
    }

    /// The script's `[Fonts]`, registered again only when the section changed. A font taken out
    /// goes from the screen too: libass can only drop all of them, so the renderer starts afresh
    /// with the media's fonts and the ones left.
    pub(crate) fn add_script_fonts(&mut self, doc: &Document) {
        let section = doc.fonts_section();
        if section.map(Arc::as_ptr) == self.section.as_ref().map(Arc::as_ptr) {
            return;
        }
        self.section = section.cloned();
        let fonts = doc.embedded_fonts();
        let names: HashSet<String> = fonts.iter().map(|(n, _)| n.clone()).collect();
        if !self.script_fonts.is_subset(&names)
            && let Ok(ass) = Self::fresh()
        {
            self.ass = ass;
            if let Some(media) = self.media.take() {
                self.load_attached_fonts(&media);
            }
        }
        self.script_fonts = names;
        (self.ass).add_fonts(fonts.iter().map(|(n, d)| (n.as_str(), d.as_slice())));
    }

    pub fn set_script(&mut self, ass: &str) -> Result<(), String> {
        self.ass.set_script(ass).map_err(|e| e.to_string())
    }

    /// The row's line as libass draws it at `at_ms`, in script pixels around its anchor:
    /// `[left, top, right, bottom]` of the text with borders (no shadow), the same of the text
    /// alone, then where the anchor is at that time. Empty when nothing of it shows.
    pub fn line_box(&mut self, s: &crate::session::Session, row: usize, at_ms: i64) -> Vec<f64> {
        let Some((script, ms, (w, h), (ax, ay), anchor)) = s.measure_script(row, at_ms) else {
            return Vec::new();
        };
        self.add_script_fonts(&s.ed.doc);
        if self.set_script(&script).is_err() {
            return Vec::new();
        }
        match self.ass.bounds(ms, w as usize, h as usize) {
            Ok(Some(b)) => {
                let mut out: Vec<f64> = (b.iter().enumerate())
                    .map(|(i, &v)| v as f64 - if i % 2 == 0 { ax } else { ay })
                    .collect();
                out.extend([anchor.0, anchor.1]);
                out
            }
            _ => Vec::new(),
        }
    }

    pub fn blend(
        &mut self,
        ms: i64,
        width: usize,
        height: usize,
        video_w: usize,
        video_h: usize,
        rgba: &mut [u8],
    ) -> Result<bool, String> {
        self.ass
            .blend(ms, width, height, (video_w, video_h), rgba)
            .map_err(|e| e.to_string())
    }

    pub fn overlay(
        &mut self,
        ms: i64,
        width: usize,
        height: usize,
        video_w: usize,
        video_h: usize,
        rgba: &mut [u8],
    ) -> Result<bool, String> {
        self.ass
            .overlay(ms, width, height, (video_w, video_h), rgba)
            .map_err(|e| e.to_string())
    }
}

pub struct Audio {
    peaks: PeakCache,
    spectrum: Spectrogram,
    /// None when no output device is available (the graph still works).
    player: Option<Player>,
}

impl Audio {
    fn from_pcm(pcm: PcmBuffer) -> Self {
        // opening the output device takes ~200 ms (WASAPI): meanwhile build the peaks
        std::thread::scope(|sc| {
            let player = sc.spawn(|| Player::new(pcm.clone(), get(&AUDIO_DEVICE).as_deref()).ok());
            Audio {
                peaks: PeakCache::new(pcm.clone()),
                spectrum: Spectrogram::new(pcm.clone(), SpectrumConfig::default()),
                player: player.join().ok().flatten(),
            }
        })
    }

    /// `progress(done, total)` while indexing (first open only) and again while decoding;
    /// false cancels. `track`: a stream index, -1 the first audio track.
    pub fn open(
        path: &str,
        track: i32,
        cache_dir: &str,
        mut progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self, String> {
        let open = |progress: &mut dyn FnMut(i64, i64) -> bool| {
            fs_media::Audio::open(Path::new(path), track, Some(&cache(cache_dir)), progress)
                .map_err(|e| format!("{path}: {e}"))
        };
        // the first one indexes the file (once, cached) for the others
        let first = open(&mut progress)?;
        let p = first.props();
        // A track over a minute decodes in parts, each with its own decoder; BestSource
        // seeks sample-exactly, so this gives the same samples (checked by bench_media).
        let n = if p.num_samples > i64::from(p.sample_rate) * 60 {
            std::thread::available_parallelism()
                .map_or(1, |n| n.get())
                .min(4)
        } else {
            1
        };
        let more: Vec<fs_media::Audio> = std::thread::scope(|sc| {
            let h: Vec<_> = (1..n)
                .map(|_| sc.spawn(|| open(&mut |_, _| true)))
                .collect();
            h.into_iter().filter_map(|h| h.join().ok()?.ok()).collect()
        });
        let srcs: Vec<&dyn AudioSource> = std::iter::once(&first)
            .chain(&more)
            .map(|s| s as &dyn AudioSource)
            .collect();
        let pcm = PcmBuffer::load_parts(&srcs, progress).map_err(|_| "cancelled".to_string())?;
        Ok(Self::from_pcm(pcm))
    }

    /// Silent audio for timing without a sound track (Kainote "dummy audio"). At 1 kHz every
    /// millisecond is a sample and Aegisub's 2h30m takes 18 MB (800 at 44.1 kHz); the player
    /// resamples for the device. A day at most: a wild length must not abort on the allocation.
    /// `noise`: Aegisub's noise audio instead, white noise of ±5000.
    pub fn blank(ms: i64, noise: bool) -> Self {
        let mut pcm = vec![0; ms.clamp(0, 24 * 3600 * 1000) as usize];
        if noise {
            let mut x = 0x9E37_79B9_7F4A_7C15_u64; // xorshift64*
            for s in &mut pcm {
                x ^= x >> 12;
                x ^= x << 25;
                x ^= x >> 27;
                *s = ((x.wrapping_mul(0x2545_F491_4F6C_DD1D) >> 32) % 10001) as i16 - 5000;
            }
        }
        Self::from_pcm(PcmBuffer::new(1000, pcm))
    }

    pub fn duration_ms(&self) -> i64 {
        self.peaks.pcm().duration_ms()
    }

    pub fn rate(&self) -> u32 {
        self.peaks.pcm().rate()
    }

    /// Aegisub's "Save audio clip": `[start_ms, end_ms)` as a mono 16-bit WAV.
    pub fn save_clip(&self, path: &str, start_ms: i64, end_ms: i64) -> Result<(), String> {
        let pcm = self.peaks.pcm();
        let clip = pcm.slice(pcm.ms_to_sample(start_ms), pcm.ms_to_sample(end_ms));
        let mut f = std::io::BufWriter::new(
            std::fs::File::create(path).map_err(|e| format!("{path}: {e}"))?,
        );
        fs_audio::wav::write_i16(&mut f, pcm.rate(), 1, clip).map_err(|e| format!("{path}: {e}"))
    }

    pub fn peaks(&self, start: f64, spp: f64, out: &mut [f32]) {
        let mut cols = vec![Peak::SILENT; out.len() / 3];
        self.peaks.columns(start, spp, &mut cols);
        for (o, p) in out.as_chunks_mut::<3>().0.iter_mut().zip(cols) {
            *o = [p.min, p.max, p.avg];
        }
    }

    pub fn spectrum(
        &self,
        start: f64,
        spp: f64,
        width: usize,
        height: usize,
        log: bool,
        out: &mut [f32],
    ) {
        let view = SpectrumView {
            start,
            spp,
            width,
            height,
            max_hz: (self.rate() / 2) as f32,
            scale: if log {
                FreqScale::Log { min_hz: 40.0 }
            } else {
                FreqScale::Linear
            },
        };
        self.spectrum.render(&view, out);
    }

    pub fn play(&mut self, start_ms: i64, end_ms: i64) -> Result<(), String> {
        let p = self.player.as_ref().ok_or("no audio output device")?;
        p.play_ms(start_ms, end_ms);
        Ok(())
    }

    pub fn stop(&self) {
        if let Some(p) = &self.player {
            p.stop();
        }
    }

    pub fn playing(&self) -> bool {
        self.player.as_ref().is_some_and(Player::is_playing)
    }

    pub fn set_end_ms(&self, end_ms: i64) {
        if let Some(p) = &self.player {
            p.set_end_ms(end_ms);
        }
    }

    pub fn position_ms(&self) -> i64 {
        self.player.as_ref().map_or(-1, Player::position_ms)
    }

    pub fn set_volume(&self, v: f32) {
        if let Some(p) = &self.player {
            p.set_volume(v);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{Audio, Renderer};

    /// A font taken out of [Fonts] makes the renderer start afresh; one added does not.
    #[test]
    fn a_removed_script_font_resets_the_renderer() {
        let mut doc = fs_core::Document::new();
        doc.attach(true, "a.ttf", b"not really a font");
        let mut r = Renderer::new().unwrap();
        r.add_script_fonts(&doc);
        assert_eq!(r.script_fonts.len(), 1);
        doc.attach(true, "b.ttf", b"nor this");
        r.add_script_fonts(&doc);
        assert_eq!(r.script_fonts.len(), 2);
        assert!(doc.remove_attachment(true, "a_0.ttf"));
        r.add_script_fonts(&doc);
        assert_eq!(r.script_fonts.len(), 1);
        assert!(r.set_script(&doc.to_ass_string()).is_ok());
    }

    #[test]
    fn preferred_audio_track_follows_the_order_of_the_names() {
        let t = [
            (1, "eng", "English 5.1"),
            (2, "jpn", ""),
            (3, "", "Commentary"),
        ];
        assert_eq!(super::preferred_track(&t, "JPN; eng"), 2);
        assert_eq!(super::preferred_track(&t, "pol;english"), 1); // part of the title
        assert_eq!(super::preferred_track(&t, " ;commentary"), 3);
        assert_eq!(super::preferred_track(&t, "ger"), -1);
        assert_eq!(super::preferred_track(&t, ""), -1);
    }

    #[test]
    fn ycbcr_headers_as_aegisub_reads_them() {
        use fs_media::{Matrix, Range};
        assert_eq!(super::ycbcr_header(" tv.709 "), "TV.709");
        assert_eq!(super::ycbcr_header("pc.240m"), "PC.240M");
        assert_eq!(super::ycbcr_header("NONE"), "None");
        assert_eq!(super::ycbcr_header(""), "");
        assert_eq!(super::ycbcr_header("TV.2020"), "");
        assert_eq!(super::ycbcr_header("TV.709.1"), "");
        assert_eq!(super::decode_as("None"), (Matrix::Auto, Range::AsDecoded));
        assert_eq!(super::decode_as(""), (Matrix::Bt601, Range::Tv)); // VSFilter's
        assert_eq!(super::decode_as("junk"), (Matrix::Bt601, Range::Tv));
        assert_eq!(super::decode_as("PC.709"), (Matrix::Bt709, Range::Pc));
        assert_eq!(super::decode_as("TV.FCC"), (Matrix::Fcc, Range::Tv));
        // untagged: guessed by size, limited range
        assert_eq!(super::best_matrix(2, 0, 1920, 1080), "TV.709");
        assert_eq!(super::best_matrix(2, 0, 720, 480), "TV.601");
        assert_eq!(super::best_matrix(5, 2, 720, 576), "PC.601");
        assert_eq!(super::best_matrix(1, 1, 1280, 720), "TV.709");
        assert_eq!(super::best_matrix(9, 1, 3840, 2160), "None"); // BT.2020
    }

    #[test]
    fn noise_audio_is_not_silent_and_stays_in_range() {
        let a = Audio::blank(2000, true);
        let s = a.peaks.pcm().samples();
        assert_eq!(s.len(), 2000);
        assert!(s.iter().all(|v| (-5000..=5000).contains(v)));
        assert!(s.iter().any(|&v| v < -2500) && s.iter().any(|&v| v > 2500));
        assert!(
            Audio::blank(10, false)
                .peaks
                .pcm()
                .samples()
                .iter()
                .all(|&v| v == 0)
        );
    }

    #[test]
    fn save_clip_writes_the_range() {
        use fs_audio::AudioSource;
        let a = Audio::blank(2000, false);
        let out = std::env::temp_dir().join(format!("fsb-clip-{}.wav", std::process::id()));
        a.save_clip(out.to_str().unwrap(), 500, 1500).unwrap();
        let wav = fs_audio::wav::read(&std::fs::read(&out).unwrap()).unwrap();
        assert_eq!(wav.num_frames(), i64::from(a.rate()));
        std::fs::remove_file(out).unwrap();
    }

    /// The GUI's audio path on tests/media/cfr.mkv (1 kHz left + 500 Hz right):
    /// both tones land on their rows of the log spectrogram, and playback runs.
    #[test]
    fn spectrum_rows_and_playback() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/media/cfr.mkv");
        let cache = std::env::temp_dir().join("fs-bridge-test-cache");
        let mut a = Audio::open(path, -1, cache.to_str().unwrap(), |_, _| true).unwrap();

        let (w, h) = (8usize, 200usize);
        let mut out = vec![0f32; w * h];
        a.spectrum(48000.0 * 2.0, 1024.0, w, h, true, &mut out);
        let col: Vec<f32> = (0..h).map(|r| out[r * w + w / 2]).collect();
        let mut sorted = col.clone();
        sorted.sort_by(f32::total_cmp);
        let median = sorted[h / 2];
        // same mapping as the renderer: log scale from 40 Hz to Nyquist, row 0 on top
        let row = |hz: f32| ((1.0 - (hz / 40.0).ln() / (24000f32 / 40.0).ln()) * h as f32) as usize;
        for hz in [500.0, 1000.0] {
            let r = row(hz);
            let peak = col[r - 2..=r + 2].iter().copied().fold(0f32, f32::max);
            assert!(
                peak > median * 4.0 && peak > 0.0,
                "{hz} Hz row {r}: {peak} vs median {median}"
            );
        }
        let quiet = col[row(3000.0)];
        assert!(
            quiet <= median * 2.0,
            "3 kHz should be quiet: {quiet} vs {median}"
        );

        a.set_volume(0.0);
        if a.play(500, 800).is_err() {
            eprintln!("skipping playback: no output device");
            return;
        }
        let t0 = std::time::Instant::now();
        let mut seen = 0;
        while a.playing() && t0.elapsed().as_secs() < 3 {
            seen = seen.max(a.position_ms());
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
        assert!(!a.playing() && (550..=800).contains(&seen), "{seen}");
    }

    /// FSED_BENCH_MEDIA=video.mkv cargo test --release -p fs-bridge bench_media -- --ignored --nocapture
    #[test]
    #[ignore]
    fn bench_media() {
        use std::time::Instant;
        let Ok(path) = std::env::var("FSED_BENCH_MEDIA") else {
            return;
        };
        let cache = std::env::temp_dir().join("fsb-bench-cache");
        let cache = cache.to_str().unwrap();
        let t0 = Instant::now();
        let src = fs_media::Audio::open(
            std::path::Path::new(&path),
            -1,
            Some(&super::cache(cache)),
            |_, _| true,
        )
        .unwrap();
        eprintln!("audio source open: {:?}", t0.elapsed());
        let t0 = Instant::now();
        let pcm = fs_audio::PcmBuffer::load(&src, |_, _| true).unwrap();
        eprintln!("audio decode: {:?}", t0.elapsed());
        let t0 = Instant::now();
        let srcs: Vec<fs_media::Audio> = std::thread::scope(|sc| {
            let h: Vec<_> = (0..4)
                .map(|_| {
                    sc.spawn(|| {
                        fs_media::Audio::open(
                            std::path::Path::new(&path),
                            -1,
                            Some(&super::cache(cache)),
                            |_, _| true,
                        )
                        .unwrap()
                    })
                })
                .collect();
            h.into_iter().map(|h| h.join().unwrap()).collect()
        });
        let refs: Vec<&dyn fs_audio::AudioSource> = srcs
            .iter()
            .map(|s| s as &dyn fs_audio::AudioSource)
            .collect();
        let par = fs_audio::PcmBuffer::load_parts(&refs, |_, _| true).unwrap();
        eprintln!("audio open+decode x4: {:?}", t0.elapsed());
        let diff: Vec<usize> = (pcm.samples().iter().zip(par.samples()))
            .enumerate()
            .filter(|(_, (a, b))| a != b)
            .map(|(i, _)| i)
            .collect();
        eprintln!(
            "parallel differs in {} samples (first {:?}, last {:?}) of {}",
            diff.len(),
            diff.first(),
            diff.last(),
            pcm.len()
        );
        let t0 = Instant::now();
        drop(fs_audio::peaks::PeakCache::new(pcm.clone()));
        eprintln!("peak cache: {:?}", t0.elapsed());
        let t0 = Instant::now();
        drop(fs_audio::spectrum::Spectrogram::new(
            pcm.clone(),
            Default::default(),
        ));
        eprintln!("spectrogram: {:?}", t0.elapsed());
        let t0 = Instant::now();
        drop(fs_audio::player::Player::new(pcm, None));
        eprintln!("player: {:?}", t0.elapsed());
        let t0 = Instant::now();
        let a = Audio::open(&path, -1, cache, |_, _| true).unwrap();
        eprintln!(
            "audio open: {:?} ({} ms of audio)",
            t0.elapsed(),
            a.duration_ms()
        );
        let (w, h) = (1600usize, 300usize);
        let mut peaks = vec![0f32; w * 3];
        let t0 = Instant::now();
        a.peaks(
            0.0,
            a.rate() as f64 * a.duration_ms() as f64 / 1000.0 / w as f64,
            &mut peaks,
        );
        eprintln!("peaks whole file: {:?}", t0.elapsed());
        let t0 = Instant::now();
        a.peaks(48000.0 * 60.0, 48000.0 * 10.0 / w as f64, &mut peaks);
        eprintln!("peaks 10 s: {:?}", t0.elapsed());
        let mut spec = vec![0f32; w * h];
        let spp = a.rate() as f64 * 10.0 / w as f64;
        for k in 0..3 {
            let t0 = Instant::now();
            a.spectrum(
                a.rate() as f64 * (60.0 + k as f64 * 2.0),
                spp,
                w,
                h,
                false,
                &mut spec,
            );
            eprintln!("spectrum 10 s view #{k}: {:?}", t0.elapsed());
        }
        let t0 = Instant::now();
        let mut v = super::Video::open(&path, cache, |_, _| true).unwrap();
        let info = v.info();
        eprintln!(
            "video open: {:?} ({}x{}, {} frames)",
            t0.elapsed(),
            info.width,
            info.height,
            info.frames
        );
        let mut px = vec![0u8; info.width as usize * info.height as usize * 4];
        let t0 = Instant::now();
        for f in 1000..1100 {
            v.frame(f, &mut px).unwrap();
        }
        eprintln!("frame sequential: {:?}", t0.elapsed() / 100);
        let t0 = Instant::now();
        for k in 0..10 {
            v.frame(5000 + k * 1777, &mut px).unwrap();
        }
        eprintln!("frame seek: {:?}", t0.elapsed() / 10);
    }
}
