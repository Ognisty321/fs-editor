//! Waveform data: min/max/average per screen column at any zoom level.
//!
//! A mipmap of block summaries is built once per track (block sizes 64,
//! 256, 1024, … samples); a column covering `spp` samples aggregates blocks
//! from the coarsest level that still resolves it, so drawing is O(width)
//! whatever the zoom. A track still being decoded is summarised as far as it
//! is ([`PeakCache::refresh`]); the rest of it is silence.

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
    /// Sum of |x| over the block, divided by 4 for each level above the first (by the 64-sample
    /// blocks it covers): at most 64 × 32768 at every level, where the plain sum overflowed u32
    /// from the 2^18-sample blocks up (loud audio read zoomed out at half its level).
    sum_abs: u32,
}

const BASE_SHIFT: u32 = 6; // 64 samples
const LEVEL_SHIFT: u32 = 2; // each level is 4x coarser

pub struct PeakCache {
    pcm: PcmBuffer,
    levels: Vec<Vec<Block>>,
    /// The samples from the start the levels summarise.
    built: usize,
}

const SILENT: Block = Block {
    min: 0,
    max: 0,
    sum_abs: 0,
};

impl PeakCache {
    pub fn new(pcm: PcmBuffer) -> Self {
        let mut levels = vec![vec![SILENT; (pcm.len() as usize).div_ceil(1 << BASE_SHIFT)]];
        while levels.last().unwrap().len() > 1 {
            let n = levels.last().unwrap().len().div_ceil(1 << LEVEL_SHIFT);
            levels.push(vec![SILENT; n]);
        }
        let mut cache = Self {
            pcm,
            levels,
            built: 0,
        };
        cache.refresh();
        cache
    }

    /// Takes in what was decoded since the track was last looked at: the blocks those samples
    /// are in, at every level. True when there was something.
    pub fn refresh(&mut self) -> bool {
        let s = self.pcm.samples();
        if s.len() <= self.built {
            return false;
        }
        let base = 1usize << BASE_SHIFT;
        // blocks [a, b) of the level; the first may have been summarised in part
        let (mut a, mut b) = (self.built >> BASE_SHIFT, s.len().div_ceil(base));
        for (i, block) in self.levels[0][a..b].iter_mut().enumerate() {
            let at = (a + i) << BASE_SHIFT;
            let c = &s[at..(at + base).min(s.len())];
            *block = Block {
                min: *c.iter().min().unwrap(),
                max: *c.iter().max().unwrap(),
                sum_abs: c.iter().map(|&x| (x as i32).unsigned_abs()).sum(),
            };
        }
        for l in 1..self.levels.len() {
            (a, b) = (a >> LEVEL_SHIFT, b.div_ceil(1 << LEVEL_SHIFT));
            let (below, level) = self.levels.split_at_mut(l);
            let prev = &below[l - 1];
            for (i, block) in level[0][a..b].iter_mut().enumerate() {
                let at = (a + i) << LEVEL_SHIFT;
                let c = &prev[at..(at + (1 << LEVEL_SHIFT)).min(prev.len())];
                *block = Block {
                    min: c.iter().map(|b| b.min).min().unwrap(),
                    max: c.iter().map(|b| b.max).max().unwrap(),
                    sum_abs: (c.iter().map(|b| u64::from(b.sum_abs)).sum::<u64>() >> LEVEL_SHIFT)
                        as u32,
                };
            }
        }
        self.built = s.len();
        true
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
            let decoded = b.min(samples.len() as i64);
            if decoded < b {
                // the rest is not decoded yet: silence
                *mn = (*mn).min(0);
                *mx = (*mx).max(0);
            }
            if a < decoded {
                for &x in &samples[a as usize..decoded as usize] {
                    *mn = (*mn).min(x);
                    *mx = (*mx).max(x);
                    *sum += (x as i32).unsigned_abs() as u64;
                }
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
                    sum += u64::from(b.sum_abs) << (lvl as u32 * LEVEL_SHIFT);
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

    /// Summarised as it is decoded, a track reads as it does summarised whole.
    #[test]
    fn a_track_taken_in_by_parts_is_the_same() {
        let whole: Vec<i16> = (0..200_003i64)
            .map(|i| ((i * 7919) % 60001 - 30000) as i16)
            .collect();
        let n = whole.len() as i64;
        let slow = Slow {
            data: whole.clone(),
            go: std::sync::atomic::AtomicI64::new(0),
        };
        let slow = std::sync::Arc::new(slow);
        let (pcm, _loading) =
            PcmBuffer::load_in_background(Box::new(Gate(slow.clone())), || None, 0);
        let mut cache = PeakCache::new(pcm.clone());
        assert_eq!(cache.range(0, n), Peak::SILENT);
        let reference = PeakCache::new(PcmBuffer::new(1000, whole.clone()));
        let ranges = [(0, n), (0, 70_000), (65_000, 140_000), (131_000, n), (5, 9)];
        for upto in [65_536, 131_072, n] {
            slow.go.store(upto, std::sync::atomic::Ordering::Release);
            let since = std::time::Instant::now();
            while pcm.ready() < upto {
                assert!(since.elapsed().as_secs() < 30);
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
            assert!(cache.refresh());
            assert!(!cache.refresh());
            // what is decoded reads as in the whole track, the rest as silence
            let mut part = whole.clone();
            part[upto as usize..].fill(0);
            let partial = PeakCache::new(PcmBuffer::new(1000, part));
            for (s, e) in ranges {
                assert_eq!(cache.range(s, e), partial.range(s, e), "{s}..{e} of {upto}");
            }
        }
        for (s, e) in ranges {
            assert_eq!(cache.range(s, e), reference.range(s, e), "{s}..{e}");
        }
    }

    /// Samples given out only up to `go`.
    struct Slow {
        data: Vec<i16>,
        go: std::sync::atomic::AtomicI64,
    }
    struct Gate(std::sync::Arc<Slow>);

    impl crate::pcm::AudioSource for Gate {
        fn sample_rate(&self) -> u32 {
            48000 // four seconds: one decoder, a chunk at a time
        }
        fn channels(&self) -> u16 {
            1
        }
        fn num_frames(&self) -> i64 {
            self.0.data.len() as i64
        }
        fn fill(&self, _: i64, _: &mut [f32]) {
            unreachable!("the buffer reads mono");
        }
        fn fill_mono(&self, start: i64, out: &mut [i16]) {
            let end = (start + out.len() as i64).min(self.num_frames());
            while self.0.go.load(std::sync::atomic::Ordering::Acquire) < end {
                std::thread::sleep(std::time::Duration::from_millis(1));
            }
            for (i, o) in out.iter_mut().enumerate() {
                *o = self.0.data.get(start as usize + i).copied().unwrap_or(0);
            }
        }
    }

    #[test]
    fn loud_audio_zoomed_out_keeps_its_level() {
        let n = 4 << 20;
        let p = PeakCache::new(PcmBuffer::new(48000, vec![30000; n])).range(0, n as i64);
        assert!((p.avg - 30000.0 / 32768.0).abs() < 1e-3, "{}", p.avg);
    }
}
