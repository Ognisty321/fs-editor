//! Spectrogram (realfft). Columns are computed at a fixed hop, cached, and
//! sampled for display; [`Spectrogram::render`] produces an intensity image
//! the GUI uploads as a texture and colours with a palette in the shader.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use realfft::{RealFftPlanner, RealToComplex};

use crate::pcm::PcmBuffer;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SpectrumConfig {
    /// FFT size as a power of two (Aegisub "spectrum quality": 8..=14).
    pub fft_log2: u32,
    /// Columns per FFT window (overlap factor, power of two).
    pub overlap: u32,
    /// Dynamic range shown, in dB.
    pub range_db: f32,
    /// Extra gain in dB before mapping.
    pub gain_db: f32,
}

impl Default for SpectrumConfig {
    fn default() -> Self {
        Self {
            fft_log2: 10,
            overlap: 4,
            range_db: 96.0,
            gain_db: 0.0,
        }
    }
}

/// How frequency maps onto image rows.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum FreqScale {
    Linear,
    /// Logarithmic from `min_hz` up.
    Log {
        min_hz: f32,
    },
}

/// The part of the spectrogram to draw.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SpectrumView {
    /// First sample of column 0.
    pub start: f64,
    /// Samples per column.
    pub spp: f64,
    pub width: usize,
    pub height: usize,
    /// Highest frequency shown (top row).
    pub max_hz: f32,
    pub scale: FreqScale,
}

pub struct Spectrogram {
    pcm: PcmBuffer,
    cfg: SpectrumConfig,
    size: usize,
    hop: usize,
    window: Vec<f32>,
    norm: f32,
    fft: Arc<dyn RealToComplex<f32>>,
    cache: Mutex<HashMap<i64, Arc<[f32]>>>,
}

const CACHE_LIMIT_BYTES: usize = 64 << 20;

impl Spectrogram {
    pub fn new(pcm: PcmBuffer, cfg: SpectrumConfig) -> Self {
        let size = 1usize << cfg.fft_log2.clamp(6, 15);
        let hop = (size / cfg.overlap.clamp(1, 64) as usize).max(1);
        // Hann window; normalise so a full-scale sine reads 0 dB
        let window: Vec<f32> = (0..size)
            .map(|i| 0.5 - 0.5 * (std::f32::consts::TAU * i as f32 / size as f32).cos())
            .collect();
        let norm = window.iter().sum::<f32>() / 2.0;
        let fft = RealFftPlanner::<f32>::new().plan_fft_forward(size);
        Self {
            pcm,
            cfg,
            size,
            hop,
            window,
            norm,
            fft,
            cache: Mutex::new(HashMap::new()),
        }
    }

    pub fn config(&self) -> SpectrumConfig {
        self.cfg
    }
    pub fn pcm(&self) -> &PcmBuffer {
        &self.pcm
    }
    /// Number of frequency bins per column (DC .. Nyquist).
    pub fn bins(&self) -> usize {
        self.size / 2 + 1
    }
    pub fn hop(&self) -> usize {
        self.hop
    }
    pub fn bin_hz(&self, bin: usize) -> f32 {
        bin as f32 * self.pcm.rate() as f32 / self.size as f32
    }

    /// Column centred on sample `index * hop`, as intensities in [0, 1].
    pub fn column(&self, index: i64) -> Arc<[f32]> {
        if let Some(c) = self.cache.lock().unwrap().get(&index) {
            return c.clone();
        }
        let centre = index * self.hop as i64;
        let start = centre - self.size as i64 / 2;
        let mut input: Vec<f32> = (0..self.size)
            .map(|i| self.pcm.get(start + i as i64) as f32 / 32768.0 * self.window[i])
            .collect();
        let mut spec = self.fft.make_output_vec();
        let mut scratch = self.fft.make_scratch_vec();
        self.fft
            .process_with_scratch(&mut input, &mut spec, &mut scratch)
            .expect("fft sizes match");
        let (range, gain) = (self.cfg.range_db.max(1.0), self.cfg.gain_db);
        let col: Arc<[f32]> = spec
            .iter()
            .map(|c| {
                let db = 20.0 * (c.norm() / self.norm).max(1e-12).log10() + gain;
                ((db + range) / range).clamp(0.0, 1.0)
            })
            .collect();
        let mut cache = self.cache.lock().unwrap();
        if cache.len() * self.bins() * 4 > CACHE_LIMIT_BYTES {
            cache.clear();
        }
        cache.insert(index, col.clone());
        col
    }

    /// Render `view.width x view.height` intensities into `out` (row-major,
    /// row 0 = highest frequency).
    pub fn render(&self, view: &SpectrumView, out: &mut [f32]) {
        let SpectrumView {
            start,
            spp,
            width,
            height,
            max_hz,
            scale,
        } = *view;
        assert!(out.len() >= width * height);
        if width == 0 || height == 0 {
            return;
        }
        let nyq = self.pcm.rate() as f32 / 2.0;
        let max_hz = max_hz.clamp(1.0, nyq);
        let bins = self.bins();
        // bin range per row (top row is max_hz)
        let row_bins: Vec<(usize, usize)> = (0..height)
            .map(|r| {
                let f = |y: f32| -> f32 {
                    let t = 1.0 - y / height as f32; // 1 at top
                    match scale {
                        FreqScale::Linear => t * max_hz,
                        FreqScale::Log { min_hz } => {
                            let lo = min_hz.clamp(1.0, max_hz * 0.5);
                            lo * (max_hz / lo).powf(t)
                        }
                    }
                };
                let (hi, lo) = (f(r as f32), f(r as f32 + 1.0));
                let b0 = ((lo / nyq) * (bins - 1) as f32).floor() as usize;
                let b1 = ((hi / nyq) * (bins - 1) as f32).ceil() as usize;
                (b0.min(bins - 1), b1.clamp(b0 + 1, bins))
            })
            .collect();
        let hop = self.hop as f64;
        for x in 0..width {
            let a = start + x as f64 * spp;
            let b = a + spp;
            // all hops inside the column (at least one), capped for speed
            let i0 = (a / hop).round() as i64;
            let i1 = ((b / hop).round() as i64).max(i0 + 1);
            let step = ((i1 - i0) / 8).max(1);
            let cols: Vec<Arc<[f32]>> = (i0..i1)
                .step_by(step as usize)
                .map(|i| self.column(i))
                .collect();
            for (y, &(b0, b1)) in row_bins.iter().enumerate() {
                let mut v = 0f32;
                for c in &cols {
                    for &m in &c[b0..b1] {
                        v = v.max(m);
                    }
                }
                out[y * width + x] = v;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sine_peaks_at_its_frequency() {
        let rate = 48000;
        let pcm = PcmBuffer::new(
            rate,
            (0..rate as usize)
                .map(|i| {
                    ((i as f32 * 1000.0 * std::f32::consts::TAU / rate as f32).sin() * 32767.0)
                        as i16
                })
                .collect(),
        );
        let sp = Spectrogram::new(
            pcm,
            SpectrumConfig {
                fft_log2: 12,
                ..Default::default()
            },
        );
        let col = sp.column(20);
        let peak = (0..sp.bins())
            .max_by(|&a, &b| col[a].total_cmp(&col[b]))
            .unwrap();
        assert!(
            (sp.bin_hz(peak) - 1000.0).abs() < sp.bin_hz(1),
            "{}",
            sp.bin_hz(peak)
        );
        // full-scale sine reads ~0 dB
        assert!(col[peak] > 0.97, "{}", col[peak]);
        // far from the tone is quiet
        assert!(col[sp.bins() - 10] < 0.3);

        let (w, h) = (50, 64);
        let mut img = vec![0f32; w * h];
        let mut view = SpectrumView {
            start: 0.0,
            spp: 960.0,
            width: w,
            height: h,
            max_hz: 2000.0,
            scale: FreqScale::Linear,
        };
        sp.render(&view, &mut img);
        // 1 kHz of 2 kHz → middle row is the brightest in every column
        let col_of = |x: usize| {
            (0..h)
                .max_by(|&a, &b| img[a * w + x].total_cmp(&img[b * w + x]))
                .unwrap()
        };
        assert!((col_of(25) as i32 - 32).abs() <= 1, "{}", col_of(25));
        view.max_hz = 24000.0;
        view.scale = FreqScale::Log { min_hz: 50.0 };
        sp.render(&view, &mut img);
        assert!(img.iter().all(|v| (0.0..=1.0).contains(v)));
    }
}
