//! Decoded audio. Decoders implement [`AudioSource`]; the editor keeps one
//! [`PcmBuffer`] per file — the whole track downmixed to mono 16-bit, like
//! Aegisub's RAM cache — which the waveform, spectrogram and player share.

use std::sync::Arc;

/// Something that can produce interleaved PCM (BestSource, a WAV file, a
/// test signal).
pub trait AudioSource: Send + Sync {
    fn sample_rate(&self) -> u32;
    fn channels(&self) -> u16;
    /// Number of sample frames (samples per channel).
    fn num_frames(&self) -> i64;
    /// Fill `out` with interleaved samples in [-1, 1] starting at frame
    /// `start`; `out.len()` is a multiple of `channels()`. Frames outside the
    /// stream are zero.
    fn fill(&self, start: i64, out: &mut [f32]);
}

/// Interleaved samples held in memory.
#[derive(Debug, Clone)]
pub struct MemorySource {
    pub rate: u32,
    pub channels: u16,
    pub data: Arc<[f32]>,
}

impl MemorySource {
    pub fn new(rate: u32, channels: u16, data: Vec<f32>) -> Self {
        assert!(channels > 0 && data.len().is_multiple_of(channels as usize));
        Self {
            rate,
            channels,
            data: data.into(),
        }
    }
}

impl AudioSource for MemorySource {
    fn sample_rate(&self) -> u32 {
        self.rate
    }
    fn channels(&self) -> u16 {
        self.channels
    }
    fn num_frames(&self) -> i64 {
        (self.data.len() / self.channels as usize) as i64
    }
    fn fill(&self, start: i64, out: &mut [f32]) {
        let ch = self.channels as i64;
        for (i, o) in out.iter_mut().enumerate() {
            let idx = start * ch + i as i64;
            *o = if idx >= 0 && (idx as usize) < self.data.len() {
                self.data[idx as usize]
            } else {
                0.0
            };
        }
    }
}

/// Error returned when loading is cancelled from the progress callback.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
#[error("audio loading cancelled")]
pub struct Cancelled;

/// A whole track as mono 16-bit samples.
#[derive(Debug, Clone)]
pub struct PcmBuffer {
    rate: u32,
    samples: Arc<[i16]>,
}

impl PcmBuffer {
    pub fn new(rate: u32, samples: Vec<i16>) -> Self {
        Self {
            rate: rate.max(1),
            samples: samples.into(),
        }
    }

    /// Decode `src` completely, averaging channels. `progress(done, total)`
    /// returns `false` to cancel.
    pub fn load(
        src: &dyn AudioSource,
        mut progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self, Cancelled> {
        let total = src.num_frames().max(0);
        let ch = src.channels().max(1) as usize;
        let mut out = Vec::with_capacity(total as usize);
        const CHUNK: i64 = 1 << 16;
        let mut buf = vec![0f32; CHUNK as usize * ch];
        let mut pos = 0;
        while pos < total {
            let n = CHUNK.min(total - pos) as usize;
            src.fill(pos, &mut buf[..n * ch]);
            out.extend(
                buf[..n * ch]
                    .chunks_exact(ch)
                    .map(|f| to_i16(f.iter().sum::<f32>() / ch as f32)),
            );
            pos += n as i64;
            if !progress(pos, total) {
                return Err(Cancelled);
            }
        }
        Ok(Self::new(src.sample_rate(), out))
    }

    pub fn rate(&self) -> u32 {
        self.rate
    }
    pub fn len(&self) -> i64 {
        self.samples.len() as i64
    }
    pub fn is_empty(&self) -> bool {
        self.samples.is_empty()
    }
    pub fn samples(&self) -> &[i16] {
        &self.samples
    }
    /// Sample at `i`, zero outside the buffer.
    pub fn get(&self, i: i64) -> i16 {
        if i >= 0 && (i as usize) < self.samples.len() {
            self.samples[i as usize]
        } else {
            0
        }
    }
    pub fn duration_ms(&self) -> i64 {
        self.sample_to_ms(self.len())
    }
    pub fn ms_to_sample(&self, ms: i64) -> i64 {
        ms * self.rate as i64 / 1000
    }
    pub fn sample_to_ms(&self, s: i64) -> i64 {
        s * 1000 / self.rate as i64
    }
    /// Clamped copy of `[start, end)` (Aegisub's "Save audio clip").
    pub fn slice(&self, start: i64, end: i64) -> &[i16] {
        let s = start.clamp(0, self.len()) as usize;
        let e = end.clamp(start.max(0), self.len()) as usize;
        &self.samples[s..e]
    }
}

pub(crate) fn to_i16(x: f32) -> i16 {
    (x * 32767.0).round().clamp(-32768.0, 32767.0) as i16
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn downmix_and_cancel() {
        let src = MemorySource::new(8000, 2, vec![1.0, -1.0, 0.5, 0.5, -1.0, -1.0]);
        let pcm = PcmBuffer::load(&src, |_, _| true).unwrap();
        assert_eq!(pcm.samples(), &[0, 16384, -32767]);
        assert_eq!(pcm.get(-1), 0);
        assert_eq!(pcm.get(3), 0);
        assert!(PcmBuffer::load(&src, |_, _| false).is_err());
        assert_eq!(pcm.slice(1, 10), &[16384, -32767]);
    }
}
