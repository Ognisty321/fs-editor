//! Video, subtitle renderer and audio wrappers for the GUI.

use std::path::{Path, PathBuf};

use fs_audio::{
    FreqScale, PcmBuffer, Peak, PeakCache, Player, Spectrogram, SpectrumConfig, SpectrumView,
};
use fs_core::vfr::{Framerate, TimeType};

use crate::ffi::VideoInfo;

fn cache(dir: &str) -> PathBuf {
    if dir.is_empty() {
        fs_media::default_cache_dir()
    } else {
        PathBuf::from(dir)
    }
}

pub struct Video {
    inner: fs_media::Video,
    fr: Framerate,
}

impl Video {
    pub fn open(path: &str, cache_dir: &str) -> Result<Self, String> {
        // shortcut: no progress/cancel while indexing; the GUI runs this on a worker thread with a busy indicator.
        let inner =
            fs_media::Video::open(Path::new(path), -1, Some(&cache(cache_dir)), |_, _| true)
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
        }
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

pub struct Renderer(fs_media::SubtitleRenderer);

impl Renderer {
    pub fn new() -> Result<Self, String> {
        let mut r = fs_media::SubtitleRenderer::new().map_err(|e| e.to_string())?;
        r.set_fonts("Arial", None).map_err(|e| e.to_string())?;
        Ok(Renderer(r))
    }

    /// Register fonts attached to an MKV (silently nothing for other files).
    pub fn load_attached_fonts(&mut self, media_path: &str) {
        for (name, data) in fs_media::attached_fonts(Path::new(media_path)).unwrap_or_default() {
            self.0.add_font(&name, &data);
        }
    }

    pub fn set_script(&mut self, ass: &str) -> Result<(), String> {
        self.0.set_script(ass).map_err(|e| e.to_string())
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
        self.0
            .blend(ms, width, height, (video_w, video_h), rgba)
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
        Audio {
            peaks: PeakCache::new(pcm.clone()),
            spectrum: Spectrogram::new(pcm.clone(), SpectrumConfig::default()),
            player: Player::new(pcm, None).ok(),
        }
    }

    pub fn open(path: &str, cache_dir: &str) -> Result<Self, String> {
        let src = fs_media::Audio::open(Path::new(path), -1, Some(&cache(cache_dir)), |_, _| true)
            .map_err(|e| format!("{path}: {e}"))?;
        let pcm = PcmBuffer::load(&src, |_, _| true).map_err(|_| "cancelled".to_string())?;
        Ok(Self::from_pcm(pcm))
    }

    /// Silent audio for timing without a sound track (Kainote "dummy audio").
    pub fn blank(ms: i64) -> Self {
        let rate = 44100u32;
        Self::from_pcm(PcmBuffer::new(
            rate,
            vec![0; (ms.max(0) * rate as i64 / 1000) as usize],
        ))
    }

    pub fn duration_ms(&self) -> i64 {
        self.peaks.pcm().duration_ms()
    }

    pub fn rate(&self) -> u32 {
        self.peaks.pcm().rate()
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
    use super::Audio;

    /// The GUI's audio path on tests/media/cfr.mkv (1 kHz left + 500 Hz right):
    /// both tones land on their rows of the log spectrogram, and playback runs.
    #[test]
    fn spectrum_rows_and_playback() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/media/cfr.mkv");
        let cache = std::env::temp_dir().join("fs-bridge-test-cache");
        let mut a = Audio::open(path, cache.to_str().unwrap()).unwrap();

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
}
