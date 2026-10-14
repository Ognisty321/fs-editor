//! Waveform data: min/max/average per screen column at any zoom level.
//!
//! A mipmap of block summaries is built once per track (block sizes 64,
//! 256, 1024, … samples); a column covering `spp` samples aggregates blocks
//! from the coarsest level that still resolves it, so drawing is O(width)
//! whatever the zoom.

use crate::pcm::PcmBuffer;

/// Summary of a range of samples, normalised to [-1, 1].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Peak {
    pub min: f32,
    pub max: f32,
    /// Mean absolute amplitude (Aegisub draws it as the inner, brighter band).
    pub avg: f32,
}

impl Peak {
    pub const SILENT: Peak = Peak {
        min: 0.0,
        max: 0.0,
        avg: 0.0,
    };
}

#[derive(Debug, Clone, Copy)]
struct Block {
    min: i16,
    max: i16,
    /// Sum of |x| over the block.
    sum_abs: u32,
}

const BASE_SHIFT: u32 = 6; // 64 samples
const LEVEL_SHIFT: u32 = 2; // each level is 4x coarser

pub struct PeakCache {
    pcm: PcmBuffer,
    levels: Vec<Vec<Block>>,
}

impl PeakCache {
    pub fn new(pcm: PcmBuffer) -> Self {
        let s = pcm.samples();
        let base = 1usize << BASE_SHIFT;
        let mut levels = vec![
            s.chunks(base)
                .map(|c| Block {
                    min: *c.iter().min().unwrap(),
                    max: *c.iter().max().unwrap(),
                    sum_abs: c.iter().map(|&x| (x as i32).unsigned_abs()).sum(),
                })
                .collect::<Vec<_>>(),
        ];
        while levels.last().unwrap().len() > 1 {
            let prev = levels.last().unwrap();
            let next = prev
                .chunks(1 << LEVEL_SHIFT)
                .map(|c| Block {
                    min: c.iter().map(|b| b.min).min().unwrap(),
                    max: c.iter().map(|b| b.max).max().unwrap(),
                    sum_abs: c.iter().map(|b| b.sum_abs).fold(0u32, u32::saturating_add),
                })
                .collect();
            levels.push(next);
        }
        Self { pcm, levels }
    }

    pub fn pcm(&self) -> &PcmBuffer {
        &self.pcm
    }

    /// Summary of samples `[start, end)`.
    pub fn range(&self, start: i64, end: i64) -> Peak {
        let len = self.pcm.len();
        let (s, e) = (start.clamp(0, len), end.clamp(0, len));
        if e <= s {
            return Peak::SILENT;
        }
        let n = (e - s) as u64;
        // finest level whose blocks are no larger than a quarter of the range
        let mut lvl = 0usize;
        while lvl + 1 < self.levels.len()
            && (1u64 << (BASE_SHIFT + (lvl as u32 + 1) * LEVEL_SHIFT)) * 4 <= n
        {
            lvl += 1;
        }
        let (mut mn, mut mx, mut sum) = (i16::MAX, i16::MIN, 0u64);
        let samples = self.pcm.samples();
        let raw = |a: i64, b: i64, mn: &mut i16, mx: &mut i16, sum: &mut u64| {
            for &x in &samples[a as usize..b as usize] {
                *mn = (*mn).min(x);
                *mx = (*mx).max(x);
                *sum += (x as i32).unsigned_abs() as u64;
            }
        };
        if n < (1 << BASE_SHIFT) * 2 {
            raw(s, e, &mut mn, &mut mx, &mut sum);
        } else {
            let shift = BASE_SHIFT + lvl as u32 * LEVEL_SHIFT;
            let bs = 1i64 << shift;
            // whole blocks inside the range, raw samples at the edges
            let first = (s + bs - 1) >> shift;
            let last = e >> shift;
            if first >= last {
                raw(s, e, &mut mn, &mut mx, &mut sum);
            } else {
                raw(s, first << shift, &mut mn, &mut mx, &mut sum);
                for b in &self.levels[lvl][first as usize..last as usize] {
                    mn = mn.min(b.min);
                    mx = mx.max(b.max);
                    sum += b.sum_abs as u64;
                }
                raw(last << shift, e, &mut mn, &mut mx, &mut sum);
            }
        }
        Peak {
            min: mn as f32 / 32768.0,
            max: mx as f32 / 32768.0,
            avg: sum as f32 / n as f32 / 32768.0,
        }
    }

    /// One [`Peak`] per column: column `c` covers samples
    /// `[start + c*spp, start + (c+1)*spp)`.
    pub fn columns(&self, start: f64, spp: f64, out: &mut [Peak]) {
        let spp = spp.max(1.0 / 64.0);
        for (c, o) in out.iter_mut().enumerate() {
            let a = (start + c as f64 * spp).floor() as i64;
            let b = ((start + (c + 1) as f64 * spp).floor() as i64).max(a + 1);
            *o = self.range(a, b);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sine(rate: u32, freq: f32, amp: f32, secs: f32) -> PcmBuffer {
        let n = (rate as f32 * secs) as usize;
        PcmBuffer::new(
            rate,
            (0..n)
                .map(|i| {
                    (amp * (i as f32 * freq * std::f32::consts::TAU / rate as f32).sin() * 32767.0)
                        as i16
                })
                .collect(),
        )
    }

    #[test]
    fn matches_brute_force() {
        let mut pcm = sine(48000, 440.0, 0.5, 2.0).samples().to_vec();
        // a click in the middle must survive any zoom
        pcm[50_001] = 32767;
        let cache = PeakCache::new(PcmBuffer::new(48000, pcm.clone()));
        for (s, e) in [
            (0, 10),
            (0, 96_000),
            (49_000, 51_000),
            (123, 77_777),
            (95_990, 100_000),
        ] {
            let p = cache.range(s, e);
            let w = &pcm[s as usize..(e as usize).min(pcm.len())];
            assert_eq!(p.min, *w.iter().min().unwrap() as f32 / 32768.0, "{s}..{e}");
            assert_eq!(p.max, *w.iter().max().unwrap() as f32 / 32768.0, "{s}..{e}");
            let avg =
                w.iter().map(|&x| (x as i32).abs() as f64).sum::<f64>() / w.len() as f64 / 32768.0;
            assert!(
                (p.avg as f64 - avg).abs() < 1e-3,
                "{s}..{e}: {} vs {avg}",
                p.avg
            );
        }
        let mut cols = [Peak::SILENT; 100];
        cache.columns(0.0, 960.0, &mut cols);
        assert!(
            cols.iter()
                .all(|c| (c.max - 0.5).abs() < 0.01 || c.max > 0.99)
        );
        assert_eq!(cols.iter().filter(|c| c.max > 0.99).count(), 1);
    }
}
