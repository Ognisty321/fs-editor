//! Audio playback through cpal. The player is the editor's clock: the GUI
//! polls [`Player::position`] to move the video and the audio cursor.
//!
//! The device runs at its own rate; the track is resampled linearly in the
//! callback. The audible position is extrapolated from the last callback's
//! playback timestamp so the cursor moves smoothly between callbacks.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU32, AtomicU64, Ordering::*};
use std::time::Instant;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{FromSample, SampleFormat, SizedSample};

use crate::pcm::PcmBuffer;

#[derive(Debug, thiserror::Error)]
pub enum PlayerError {
    #[error("no audio output device")]
    NoDevice,
    #[error("audio device error: {0}")]
    Device(String),
}

impl From<cpal::Error> for PlayerError {
    fn from(e: cpal::Error) -> Self {
        PlayerError::Device(e.to_string())
    }
}

/// State shared with the audio callback. Positions are in source samples;
/// fractional positions are stored as f64 bits.
struct Shared {
    playing: AtomicBool,
    /// Bumped by `play` so the callback picks up a new start position.
    seek_gen: AtomicU64,
    seek_to: AtomicI64,
    end: AtomicI64,
    volume: AtomicU32,
    /// Source position of the first frame of the last buffer (f64 bits).
    buf_pos: AtomicU64,
    /// Wall-clock nanoseconds (since `epoch`) when that frame becomes audible.
    buf_audible_ns: AtomicI64,
    epoch: Instant,
}

pub struct Player {
    pcm: PcmBuffer,
    shared: Arc<Shared>,
    device_rate: u32,
    device_name: String,
    _stream: cpal::Stream,
}

/// Names of the available output devices.
pub fn output_devices() -> Vec<String> {
    let host = cpal::default_host();
    host.output_devices()
        .map(|it| {
            it.filter_map(|d| d.description().ok().map(|n| n.to_string()))
                .collect()
        })
        .unwrap_or_default()
}

impl Player {
    /// Open the default output device (or the one whose name matches
    /// `device`).
    pub fn new(pcm: PcmBuffer, device: Option<&str>) -> Result<Self, PlayerError> {
        let host = cpal::default_host();
        let dev = match device {
            Some(name) => host
                .output_devices()?
                .find(|d| d.description().is_ok_and(|n| n.to_string() == name))
                .or_else(|| host.default_output_device()),
            None => host.default_output_device(),
        }
        .ok_or(PlayerError::NoDevice)?;
        let device_name = dev.description().map(|n| n.to_string()).unwrap_or_default();
        // prefer the track's own rate when the device supports it
        let supported = dev.default_output_config()?;
        let cfg = dev
            .supported_output_configs()
            .ok()
            .and_then(|mut it| {
                it.find(|c| {
                    c.sample_format() == supported.sample_format()
                        && c.channels() == supported.channels()
                        && (c.min_sample_rate()..=c.max_sample_rate()).contains(&pcm.rate())
                })
            })
            .map(|c| c.with_sample_rate(pcm.rate()))
            .unwrap_or(supported);
        let shared = Arc::new(Shared {
            playing: AtomicBool::new(false),
            seek_gen: AtomicU64::new(0),
            seek_to: AtomicI64::new(0),
            end: AtomicI64::new(0),
            volume: AtomicU32::new(1f32.to_bits()),
            buf_pos: AtomicU64::new(0f64.to_bits()),
            buf_audible_ns: AtomicI64::new(0),
            epoch: Instant::now(),
        });
        let device_rate = cfg.sample_rate();
        let stream = match cfg.sample_format() {
            SampleFormat::F32 => build::<f32>(&dev, &cfg.config(), &pcm, &shared)?,
            SampleFormat::I16 => build::<i16>(&dev, &cfg.config(), &pcm, &shared)?,
            SampleFormat::I32 => build::<i32>(&dev, &cfg.config(), &pcm, &shared)?,
            SampleFormat::U16 => build::<u16>(&dev, &cfg.config(), &pcm, &shared)?,
            SampleFormat::F64 => build::<f64>(&dev, &cfg.config(), &pcm, &shared)?,
            f => {
                return Err(PlayerError::Device(format!(
                    "unsupported sample format {f:?}"
                )));
            }
        };
        stream.play()?;
        Ok(Self {
            pcm,
            shared,
            device_rate,
            device_name,
            _stream: stream,
        })
    }

    pub fn device_name(&self) -> &str {
        &self.device_name
    }
    pub fn device_rate(&self) -> u32 {
        self.device_rate
    }
    pub fn pcm(&self) -> &PcmBuffer {
        &self.pcm
    }

    /// Play samples `[start, end)`.
    pub fn play(&self, start: i64, end: i64) {
        let s = &self.shared;
        s.seek_to.store(start.max(0), SeqCst);
        s.end.store(end.min(self.pcm.len()), SeqCst);
        s.buf_pos.store((start.max(0) as f64).to_bits(), SeqCst);
        s.buf_audible_ns.store(i64::MAX, SeqCst);
        s.seek_gen.fetch_add(1, SeqCst);
        s.playing.store(true, SeqCst);
    }

    /// Play from `start_ms` to `end_ms`.
    pub fn play_ms(&self, start_ms: i64, end_ms: i64) {
        self.play(
            self.pcm.ms_to_sample(start_ms),
            self.pcm.ms_to_sample(end_ms),
        );
    }

    pub fn stop(&self) {
        self.shared.playing.store(false, SeqCst);
    }

    /// Move the end of the range being played (dragging a marker while playing).
    pub fn set_end(&self, end: i64) {
        self.shared.end.store(end.min(self.pcm.len()), SeqCst);
    }

    pub fn end(&self) -> i64 {
        self.shared.end.load(SeqCst)
    }

    pub fn is_playing(&self) -> bool {
        self.shared.playing.load(SeqCst)
    }

    pub fn set_volume(&self, v: f32) {
        self.shared
            .volume
            .store(v.clamp(0.0, 4.0).to_bits(), SeqCst);
    }

    /// Sample currently heard.
    pub fn position(&self) -> i64 {
        let s = &self.shared;
        let base = f64::from_bits(s.buf_pos.load(SeqCst));
        let audible_at = s.buf_audible_ns.load(SeqCst);
        let now = s.epoch.elapsed().as_nanos() as i64;
        let ahead = if audible_at == i64::MAX {
            0.0
        } else {
            (now - audible_at).max(0) as f64 * 1e-9 * self.pcm.rate() as f64
        };
        let start = s.seek_to.load(SeqCst);
        ((base + ahead) as i64).clamp(start, s.end.load(SeqCst).max(start))
    }

    pub fn position_ms(&self) -> i64 {
        self.pcm.sample_to_ms(self.position())
    }
}

fn build<T>(
    dev: &cpal::Device,
    cfg: &cpal::StreamConfig,
    pcm: &PcmBuffer,
    shared: &Arc<Shared>,
) -> Result<cpal::Stream, PlayerError>
where
    T: SizedSample + FromSample<f32>,
{
    let pcm = pcm.clone();
    let sh = shared.clone();
    let channels = cfg.channels as usize;
    let step = pcm.rate() as f64 / cfg.sample_rate as f64;
    let mut gen_seen = u64::MAX;
    let mut pos = 0f64;
    let data_cb = move |out: &mut [T], info: &cpal::OutputCallbackInfo| {
        let g = sh.seek_gen.load(Acquire);
        if g != gen_seen {
            gen_seen = g;
            pos = sh.seek_to.load(Acquire) as f64;
        }
        let playing = sh.playing.load(Acquire);
        let end = sh.end.load(Acquire) as f64;
        let vol = f32::from_bits(sh.volume.load(Relaxed));
        if playing {
            // when the first frame of this buffer will be heard
            let ts = info.timestamp();
            let latency = ts
                .playback
                .checked_duration_since(ts.callback)
                .unwrap_or_default();
            let audible = sh.epoch.elapsed() + latency;
            sh.buf_pos.store(pos.to_bits(), Release);
            sh.buf_audible_ns.store(audible.as_nanos() as i64, Release);
        }
        let samples = pcm.samples();
        let mut finished = !playing;
        for frame in out.chunks_mut(channels) {
            let v = if finished || pos >= end {
                finished = true;
                0.0
            } else {
                let i = pos.floor() as usize;
                let t = (pos - i as f64) as f32;
                let a = samples.get(i).copied().unwrap_or(0) as f32;
                let b = samples.get(i + 1).copied().unwrap_or(0) as f32;
                pos += step;
                (a + (b - a) * t) / 32768.0 * vol
            };
            let s = T::from_sample(v.clamp(-1.0, 1.0));
            frame.fill(s);
        }
        if playing && finished {
            sh.playing.store(false, Release);
        }
    };
    let err_cb = |e: cpal::Error| eprintln!("audio stream error: {e}");
    Ok(dev.build_output_stream::<T, _, _>(*cfg, data_cb, err_cb, None)?)
}
