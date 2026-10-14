//! Audio for FS Editor: decoded PCM cache, waveform peaks, spectrogram
//! (realfft) and playback (cpal).

pub mod pcm;
pub mod peaks;
pub mod player;
pub mod spectrum;
pub mod wav;

pub use pcm::{AudioSource, Cancelled, MemorySource, PcmBuffer};
pub use peaks::{Peak, PeakCache};
pub use player::{Player, PlayerError, output_devices};
pub use spectrum::{FreqScale, Spectrogram, SpectrumConfig, SpectrumView};
