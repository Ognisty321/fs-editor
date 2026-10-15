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
    cache: Mutex<Cache>,
}

const CACHE_LIMIT_BYTES: usize = 64 << 20;

/// Computed columns with the read each was last used by.
#[derive(Default)]
struct Cache {
    columns: HashMap<i64, (Arc<[f32]>, u64)>,
    /// Bumped once per lookup.
    tick: u64,
}

impl Cache {
    /// Over the budget, the least recently read columns go until half of it is left, so a
    /// zoomed-out view scrolled back and forth keeps what it returns to (it was all cleared).
    fn evict(&mut self, column_bytes: usize) {
        let keep = CACHE_LIMIT_BYTES / 2 / column_bytes.max(1);
        if self.columns.len() * column_bytes <= CACHE_LIMIT_BYTES || keep == 0 {
            return;
        }
        let mut stamps: Vec<u64> = self.columns.values().map(|c| c.1).collect();
        let (_, &mut oldest_kept, _) = stamps.select_nth_unstable_by(keep - 1, |a, b| b.cmp(a));
        self.columns.retain(|_, c| c.1 >= oldest_kept);
    }
}

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
            cache: Mutex::default(),
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
        self.columns(&[index]).swap_remove(0)
    }

    /// The columns of `indices`; those not cached are computed on all cores.
    fn columns(&self, indices: &[i64]) -> Vec<Arc<[f32]>> {
        let mut out = Vec::with_capacity(indices.len());
        let mut missing = Vec::new();
        {
            let mut cache = self.cache.lock().unwrap();
            cache.tick += 1;
            let tick = cache.tick;
            for (k, i) in indices.iter().enumerate() {
                match cache.columns.get_mut(i) {
                    Some((c, used)) => {
                        *used = tick;
                        out.push(Some(c.clone()));
                    }
                    None => {
                        out.push(None);
                        missing.push(k);
                    }
                }
            }
        }
        if !missing.is_empty() {
            let threads = std::thread::available_parallelism()
                .map_or(1, |n| n.get())
                .min(missing.len().div_ceil(32));
            let computed: Vec<Arc<[f32]>> = if threads <= 1 {
                missing.iter().map(|&k| self.compute(indices[k])).collect()
            } else {
                std::thread::scope(|s| {
                    let parts: Vec<_> = missing
                        .chunks(missing.len().div_ceil(threads))
                        .map(|part| {
                            s.spawn(move || {
                                part.iter()
                                    .map(|&k| self.compute(indices[k]))
                                    .collect::<Vec<_>>()
                            })
                        })
                        .collect();
                    parts
                        .into_iter()
                        .flat_map(|h| h.join().unwrap_or_else(|e| std::panic::resume_unwind(e)))
                        .collect()
                })
            };
            let mut cache = self.cache.lock().unwrap();
            let tick = cache.tick;
            for (&k, c) in missing.iter().zip(computed) {
                cache.columns.insert(indices[k], (c.clone(), tick));
                out[k] = Some(c);
            }
            cache.evict(self.bins() * size_of::<f32>());
        }
        out.into_iter()
            .map(|c| c.expect("every column found or computed"))
            .collect()
    }

    fn compute(&self, index: i64) -> Arc<[f32]> {
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
        spec.iter()
            .map(|c| {
                let db = 20.0 * (c.norm() / self.norm).max(1e-12).log10() + gain;
                ((db + range) / range).clamp(0.0, 1.0)
            })
            .collect()
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
        // The hops inside each column (at least one), capped for speed at every `step`-th: one
        // stride for the whole view, on its multiples, so a view scrolled by any amount reads the
        // columns the last one computed (sampling from each column's first hop shifted them all).
        let step = ((spp / hop / 8.0) as i64).max(1);
        let hops: Vec<std::ops::Range<i64>> = (0..width)
            .map(|x| {
                let a = start + x as f64 * spp;
                let i0 = (a / hop).round() as i64;
                let i1 = (((a + spp) / hop).round() as i64).max(i0 + 1);
                let first = i0 + (step - i0.rem_euclid(step)) % step;
                if first < i1 { first..i1 } else { i0..i0 + 1 }
            })
            .collect();
        let mut need: Vec<i64> = hops
            .iter()
            .flat_map(|r| r.clone().step_by(step as usize))
            .collect();
        need.sort_unstable();
        need.dedup();
        let cols = self.columns(&need);
        for (x, r) in hops.into_iter().enumerate() {
            let col: Vec<&[f32]> = r
                .step_by(step as usize)
                .map(|i| &*cols[need.binary_search(&i).expect("needed")])
                .collect();
            for (y, &(b0, b1)) in row_bins.iter().enumerate() {
                let mut v = 0f32;
                for c in &col {
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

    /// Zoomed out, a view scrolled by a fraction of a column computes only the hops it uncovers.
    #[test]
    fn scrolling_reuses_columns() {
        let rate = 48000;
        let pcm = PcmBuffer::new(
            rate,
            (0..rate as usize * 60).map(|i| (i % 2000) as i16).collect(),
        );
        let sp = Spectrogram::new(pcm, SpectrumConfig::default());
        let mut view = SpectrumView {
            start: 0.0,
            spp: 48000.0 * 30.0 / 200.0,
            width: 200,
            height: 32,
            max_hz: 24000.0,
            scale: FreqScale::Linear,
        };
        let mut img = vec![0f32; 200 * 32];
        sp.render(&view, &mut img);
        let first = sp.cache.lock().unwrap().columns.len();
        view.start += view.spp * 3.37;
        sp.render(&view, &mut img);
        let more = sp.cache.lock().unwrap().columns.len() - first;
        assert!(more <= 4 * 16, "{first} then {more} more");
    }

    #[test]
    fn eviction_keeps_the_most_recently_read() {
        let mut cache = Cache::default();
        let col: Arc<[f32]> = Arc::from([0f32; 1]);
        for i in 1..=30 {
            cache.columns.insert(i, (col.clone(), i as u64));
        }
        cache.evict(CACHE_LIMIT_BYTES / 10);
        let mut kept: Vec<i64> = cache.columns.keys().copied().collect();
        kept.sort_unstable();
        assert_eq!(kept, [26, 27, 28, 29, 30]);
    }
}
