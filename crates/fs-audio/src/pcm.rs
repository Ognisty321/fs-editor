//! Decoded audio. Decoders implement [`AudioSource`]; the editor keeps one
//! [`PcmBuffer`] per file — the whole track downmixed to mono 16-bit, like
//! Aegisub's RAM cache — which the waveform, spectrogram and player share.
//! Like that cache it is there at once and fills in the background
//! ([`PcmBuffer::load_in_background`]): what is not decoded yet is silence.

use std::sync::atomic::{AtomicBool, AtomicU8, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

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
    /// The frames from `start` as a [`PcmBuffer`] keeps them: the mean of the
    /// channels, 16-bit. A decoder with a shorter way to them than through
    /// [`fill`](Self::fill) gives the same samples.
    fn fill_mono(&self, start: i64, out: &mut [i16]) {
        let ch = self.channels().max(1) as usize;
        let mut buf = vec![0f32; out.len() * ch];
        self.fill(start, &mut buf);
        for (o, f) in out.iter_mut().zip(buf.chunks_exact(ch)) {
            *o = to_i16(f.iter().sum::<f32>() / ch as f32);
        }
    }
    /// False once a read away from the last one decodes from the start of the
    /// track instead of seeking: a second decoder then gains nothing.
    fn seeks(&self) -> bool {
        true
    }
    /// From any thread: the read under way and the ones after it end early,
    /// with samples of no use.
    fn cancel(&self) {}
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

/// The samples of a track. Each is written once, by the decoder its part of the track belongs
/// to, and read only once it is below `ready`, which never goes back.
struct Samples {
    ptr: *mut i16,
    len: usize,
    /// `[..ready]` is decoded and not written again.
    ready: AtomicUsize,
}

// SAFETY: the samples below `ready` are immutable, and the ones above are written through
// `Plan::decode` alone, by the one thread that claimed their part.
unsafe impl Send for Samples {}
unsafe impl Sync for Samples {}

impl Samples {
    fn new(samples: Vec<i16>, ready: usize) -> Self {
        let b = samples.into_boxed_slice();
        Self {
            len: b.len(),
            ready: AtomicUsize::new(ready.min(b.len())),
            ptr: Box::into_raw(b).cast(),
        }
    }

    fn ready(&self) -> &[i16] {
        // SAFETY: `ready` samples from `ptr` are initialised (all of them are, to zero) and
        // nothing writes below `ready` (see `Plan::publish`).
        unsafe { std::slice::from_raw_parts(self.ptr, self.ready.load(Ordering::Acquire)) }
    }
}

impl Drop for Samples {
    fn drop(&mut self) {
        // SAFETY: the box `new` took apart, and the last reference to it.
        drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.ptr, self.len)) });
    }
}

/// A whole track as mono 16-bit samples.
#[derive(Clone)]
pub struct PcmBuffer {
    rate: u32,
    samples: Arc<Samples>,
}

impl std::fmt::Debug for PcmBuffer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PcmBuffer")
            .field("rate", &self.rate)
            .field("len", &self.len())
            .field("ready", &self.ready())
            .finish()
    }
}

/// Samples a decoder is asked for at a time: over a second, so little is lost between them, and
/// short enough for a cancelled load to stop at once.
const CHUNK: usize = 1 << 16;

impl PcmBuffer {
    pub fn new(rate: u32, samples: Vec<i16>) -> Self {
        let ready = samples.len();
        Self {
            rate: rate.max(1),
            samples: Arc::new(Samples::new(samples, ready)),
        }
    }

    /// Decode `src` completely, averaging channels. `progress(done, total)`
    /// returns `false` to cancel.
    pub fn load(
        src: &dyn AudioSource,
        mut progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self, Cancelled> {
        let mut samples = vec![0i16; src.num_frames().max(0) as usize];
        let (total, mut done) = (samples.len() as i64, 0i64);
        if total == 0 && !progress(0, 0) {
            return Err(Cancelled);
        }
        for c in samples.chunks_mut(CHUNK) {
            src.fill_mono(done, c);
            done += c.len() as i64;
            if !progress(done, total) {
                return Err(Cancelled);
            }
        }
        Ok(Self::new(src.sample_rate(), samples))
    }

    /// The track of `first`, `delay_ms` later (Kainote's audio delay: silence before it, or its
    /// start cut off when negative), returned at once and decoded on threads of its own:
    /// [`ready`](Self::ready) says how far. Dropping the [`Loading`] stops them.
    ///
    /// A track over a minute long is decoded in parts when it can be: `another` opens one more
    /// decoder of the same track for each part after the first. One that gets to its part by
    /// seeking (most codecs) decodes it; one that does not (DTS-HD MA: it would decode from the
    /// start of the file to get there) is stopped, and the decoder before it goes on through
    /// that part, as Aegisub's one decoder does through the whole track.
    pub fn load_in_background(
        first: Box<dyn AudioSource>,
        another: impl Fn() -> Option<Box<dyn AudioSource>> + Send + Sync + 'static,
        delay_ms: i64,
    ) -> (Self, Loading) {
        let rate = first.sample_rate().max(1);
        let frames = first.num_frames().max(0);
        let parts = if frames > i64::from(rate) * 60 {
            std::thread::available_parallelism()
                .map_or(1, |n| n.get())
                .min(4)
        } else {
            1
        };
        let shift = (i128::from(delay_ms) * i128::from(rate) / 1000) as i64;
        Self::load_with(first, Arc::new(another), shift, parts, PATIENCE)
    }

    fn load_with(
        first: Box<dyn AudioSource>,
        another: Arc<Opener>,
        shift: i64,
        parts: usize,
        patience: Duration,
    ) -> (Self, Loading) {
        let rate = first.sample_rate().max(1);
        // a day at most: a wild delay must not abort on the allocation
        let len = first
            .num_frames()
            .max(0)
            .saturating_add(shift)
            .clamp(0, i64::from(rate) * 86400) as usize;
        let samples = Arc::new(Samples::new(vec![0i16; len], 0));
        let size = len.div_ceil(parts.max(1)).max(1);
        let plan = Arc::new(Plan {
            parts: (0..len.div_ceil(size).max(1))
                .map(|k| Part {
                    start: (k * size).min(len),
                    end: ((k + 1) * size).min(len),
                    taken: AtomicBool::new(false),
                    done: AtomicUsize::new(0),
                    scout: Mutex::new(None),
                })
                .collect(),
            samples: samples.clone(),
            shift,
            cancel: AtomicBool::new(false),
            active: Mutex::new(Vec::new()),
            patience,
        });
        plan.publish(); // an empty track is complete
        let first: Arc<dyn AudioSource> = Arc::from(first);
        lock(&plan.active).push(first.clone());
        for k in 1..plan.parts.len() {
            let (plan, another) = (plan.clone(), another.clone());
            std::thread::spawn(move || helper(&plan, &another, k));
        }
        std::thread::spawn({
            let plan = plan.clone();
            move || {
                let mut k = 0;
                while plan.claim(k) && plan.decode(k, &*first, 0) {
                    k += 1;
                }
                plan.release(&first);
            }
        });
        (Self { rate, samples }, Loading(plan))
    }

    pub fn rate(&self) -> u32 {
        self.rate
    }

    /// The length of the track, decoded or not.
    pub fn len(&self) -> i64 {
        self.samples.len as i64
    }
    pub fn is_empty(&self) -> bool {
        self.samples.len == 0
    }
    /// How many samples from the start are decoded: [`len`](Self::len) once all are.
    pub fn ready(&self) -> i64 {
        self.samples.ready.load(Ordering::Acquire) as i64
    }
    pub fn is_complete(&self) -> bool {
        self.ready() == self.len()
    }
    /// The samples decoded so far, from the start; the rest of the track is silence until it is.
    pub fn samples(&self) -> &[i16] {
        self.samples.ready()
    }
    /// Sample at `i`, zero outside the buffer and where it is not decoded yet.
    pub fn get(&self, i: i64) -> i16 {
        if i < 0 {
            return 0;
        }
        self.samples().get(i as usize).copied().unwrap_or(0)
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
    /// Clamped copy of `[start, end)` of what is decoded (Aegisub's "Save audio clip").
    pub fn slice(&self, start: i64, end: i64) -> &[i16] {
        let all = self.samples();
        let s = start.clamp(0, all.len() as i64);
        // a range past the end is empty (`clamp` panics on min > max)
        let e = end.clamp(s, all.len() as i64);
        &all[s as usize..e as usize]
    }
}

/// How long a decoder may take to give the first samples of a part it seeks to. One that seeks
/// does it in a fraction of a second; BestSource retrying in a track it cannot seek in takes
/// tens of them, reading the file under the decoder that is getting on with it.
const PATIENCE: Duration = Duration::from_secs(2);

type Opener = dyn Fn() -> Option<Box<dyn AudioSource>> + Send + Sync;

/// A stretch of the track one decoder reads front to back.
struct Part {
    start: usize,
    end: usize,
    /// Set by the decoder that reads it: the one that seeked to its start, or else the one that
    /// finished the part before it.
    taken: AtomicBool,
    /// Samples of it decoded, from its start.
    done: AtomicUsize,
    /// The decoder seeking to its start, for whoever takes the part first to stop.
    scout: Mutex<Option<Arc<dyn AudioSource>>>,
}

struct Plan {
    samples: Arc<Samples>,
    parts: Vec<Part>,
    /// A sample's place in the buffer less its place in the track.
    shift: i64,
    cancel: AtomicBool,
    /// The decoders at work, to stop with the load.
    active: Mutex<Vec<Arc<dyn AudioSource>>>,
    patience: Duration,
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

impl Plan {
    fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::Acquire)
    }

    /// Part `k` for the calling thread to decode; false when it has a decoder (or is none).
    fn claim(&self, k: usize) -> bool {
        let Some(p) = self.parts.get(k) else {
            return false;
        };
        if p.taken.swap(true, Ordering::AcqRel) {
            return false;
        }
        // whoever is still seeking there comes too late
        if let Some(s) = lock(&p.scout).take() {
            s.cancel();
        }
        true
    }

    /// Decodes the rest of part `k`, claimed by the caller, from `from` samples into it. False
    /// when the load was cancelled.
    fn decode(&self, k: usize, src: &dyn AudioSource, from: usize) -> bool {
        let p = &self.parts[k];
        let mut pos = p.start + from;
        while pos < p.end {
            if self.cancelled() {
                return false;
            }
            let n = CHUNK.min(p.end - pos);
            // SAFETY: within the buffer (`pos + n <= p.end <= len`), in a part only this thread
            // writes (`claim`), above `ready`: that is at most `p.start + p.done`, which is `pos`.
            let out = unsafe { std::slice::from_raw_parts_mut(self.samples.ptr.add(pos), n) };
            src.fill_mono(pos as i64 - self.shift, out);
            if self.cancelled() {
                return false; // the read may have been cut short
            }
            pos += n;
            p.done.store(pos - p.start, Ordering::Release);
            self.publish();
        }
        true
    }

    /// Moves `ready` to the end of what is decoded without a gap from the start.
    fn publish(&self) {
        let mut ready = 0;
        for p in &self.parts {
            ready = p.start + p.done.load(Ordering::Acquire);
            if ready < p.end {
                break;
            }
        }
        self.samples.ready.fetch_max(ready, Ordering::AcqRel);
    }

    fn release(&self, src: &Arc<dyn AudioSource>) {
        lock(&self.active).retain(|s| !Arc::ptr_eq(s, src));
    }
}

/// One more decoder for part `k` and the ones after it that have none. It has to show first that
/// it gets there by seeking: the part's first samples are decoded aside, and the part is taken
/// only if they came in time from a decoder still seeking.
fn helper(plan: &Arc<Plan>, another: &Arc<Opener>, k: usize) {
    const WAITING: u8 = 0;
    const CAME: u8 = 1;
    const LATE: u8 = 2;
    let p = &plan.parts[k];
    if plan.cancelled() || p.taken.load(Ordering::Acquire) {
        return;
    }
    let Some(src) = another() else {
        return;
    };
    let src: Arc<dyn AudioSource> = Arc::from(src);
    lock(&plan.active).push(src.clone());
    *lock(&p.scout) = Some(src.clone());
    let state = Arc::new(AtomicU8::new(WAITING));
    let mut first = vec![0i16; CHUNK.min(p.end - p.start)];
    // registered before these checks: a cancel or a claim since then has stopped the decoder
    if !plan.cancelled() && !p.taken.load(Ordering::Acquire) {
        std::thread::spawn({
            let (state, src, patience) = (state.clone(), src.clone(), plan.patience);
            move || {
                let since = Instant::now();
                while state.load(Ordering::Acquire) == WAITING {
                    if since.elapsed() >= patience {
                        if state
                            .compare_exchange(WAITING, LATE, Ordering::AcqRel, Ordering::Acquire)
                            .is_ok()
                        {
                            src.cancel();
                        }
                        return;
                    }
                    std::thread::sleep(Duration::from_millis(10));
                }
            }
        });
        src.fill_mono(p.start as i64 - plan.shift, &mut first);
    }
    let came = state
        .compare_exchange(WAITING, CAME, Ordering::AcqRel, Ordering::Acquire)
        .is_ok();
    lock(&p.scout).take();
    if came && src.seeks() && !plan.cancelled() && plan.claim(k) {
        // SAFETY: as in `Plan::decode`: the part is this thread's, and none of it is ready.
        unsafe {
            std::ptr::copy_nonoverlapping(
                first.as_ptr(),
                plan.samples.ptr.add(p.start),
                first.len(),
            )
        };
        p.done.store(first.len(), Ordering::Release);
        plan.publish();
        let (mut k, mut from) = (k, first.len());
        while plan.decode(k, &*src, from) {
            (k, from) = (k + 1, 0);
            if !plan.claim(k) {
                break;
            }
        }
    }
    plan.release(&src);
}

/// A load under way ([`PcmBuffer::load_in_background`]); dropped, it stops.
pub struct Loading(Arc<Plan>);

impl Loading {
    /// Stops the decoders: what is not decoded stays silent.
    pub fn cancel(&self) {
        self.0.cancel.store(true, Ordering::Release);
        for s in lock(&self.0.active).drain(..) {
            s.cancel();
        }
    }
}

impl Drop for Loading {
    fn drop(&mut self) {
        self.cancel();
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
        // a selection after the audio's end, or one that ends before it starts, is empty
        assert!(pcm.slice(50, 60).is_empty());
        assert!(pcm.slice(2, 1).is_empty());
        assert!(pcm.slice(-9, -3).is_empty());
    }

    /// A ramp whose decoders count their reads and can be told not to seek, or to take their time
    /// over a read away from the start.
    struct Ramp {
        frames: i64,
        seeks: bool,
        slow: Duration,
        stopped: AtomicBool,
        reads: Arc<AtomicUsize>,
    }

    impl Ramp {
        fn new(frames: i64, seeks: bool, slow: Duration, reads: &Arc<AtomicUsize>) -> Box<Self> {
            Box::new(Self {
                frames,
                seeks,
                slow,
                stopped: AtomicBool::new(false),
                reads: reads.clone(),
            })
        }
        fn at(i: i64) -> i16 {
            (i % 30011) as i16
        }
    }

    impl AudioSource for Ramp {
        fn sample_rate(&self) -> u32 {
            1000
        }
        fn channels(&self) -> u16 {
            1
        }
        fn num_frames(&self) -> i64 {
            self.frames
        }
        fn fill(&self, _: i64, _: &mut [f32]) {
            unreachable!("the buffer reads mono");
        }
        fn fill_mono(&self, start: i64, out: &mut [i16]) {
            self.reads.fetch_add(1, Ordering::Relaxed);
            let until = Instant::now() + self.slow;
            while start > 0 && Instant::now() < until && !self.stopped.load(Ordering::Acquire) {
                std::thread::sleep(Duration::from_millis(1));
            }
            for (i, o) in out.iter_mut().enumerate() {
                let at = start + i as i64;
                *o = if self.stopped.load(Ordering::Acquire) || at < 0 || at >= self.frames {
                    0
                } else {
                    Self::at(at)
                };
            }
        }
        fn seeks(&self) -> bool {
            self.seeks
        }
        fn cancel(&self) {
            self.stopped.store(true, Ordering::Release);
        }
    }

    fn none() -> Option<Box<dyn AudioSource>> {
        None
    }

    fn wait(pcm: &PcmBuffer) {
        let since = Instant::now();
        while !pcm.is_complete() {
            assert!(since.elapsed() < Duration::from_secs(30), "{pcm:?}");
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    fn is_ramp(pcm: &PcmBuffer, shift: i64, frames: i64) {
        assert!(pcm.is_complete());
        for (i, &s) in pcm.samples().iter().enumerate() {
            let at = i as i64 - shift;
            let want = if at < 0 || at >= frames {
                0
            } else {
                Ramp::at(at)
            };
            assert_eq!(s, want, "sample {i}");
        }
    }

    #[test]
    fn a_background_load_is_the_track_whoever_decodes_it() {
        let frames = 1_000_003;
        for (seeks, slow) in [(true, 0), (false, 0), (true, 400)] {
            let reads = Arc::new(AtomicUsize::new(0));
            let slow = Duration::from_millis(slow);
            let more = {
                let reads = reads.clone();
                move || Some(Ramp::new(frames, seeks, slow, &reads) as Box<dyn AudioSource>)
            };
            // the first decoder is never slow: it reads from the start
            let first = Ramp::new(frames, seeks, Duration::ZERO, &reads);
            let (pcm, _loading) =
                PcmBuffer::load_with(first, Arc::new(more), 0, 4, Duration::from_millis(100));
            assert_eq!(pcm.len(), frames);
            wait(&pcm);
            is_ramp(&pcm, 0, frames);
        }
    }

    #[test]
    fn decoders_that_seek_share_the_track() {
        let frames = 4_000_000;
        let reads = Arc::new(AtomicUsize::new(0));
        let (opened, more) = (Arc::new(AtomicUsize::new(0)), {
            let reads = reads.clone();
            move |opened: &AtomicUsize| {
                opened.fetch_add(1, Ordering::Relaxed);
                Some(Ramp::new(frames, true, Duration::ZERO, &reads) as Box<dyn AudioSource>)
            }
        });
        let first = Ramp::new(frames, true, Duration::ZERO, &reads);
        let (pcm, _loading) = PcmBuffer::load_with(
            first,
            Arc::new({
                let opened = opened.clone();
                move || more(&opened)
            }),
            0,
            4,
            Duration::from_secs(5),
        );
        wait(&pcm);
        is_ramp(&pcm, 0, frames);
        // each sample was read once, but for the part starts tried by a decoder that came late
        let chunks = (frames as usize).div_ceil(CHUNK);
        let read = reads.load(Ordering::Relaxed);
        assert!(
            (chunks..=chunks + 4 + 3).contains(&read),
            "{read} reads of {chunks}"
        );
        assert!(opened.load(Ordering::Relaxed) <= 3);
    }

    #[test]
    fn a_decoder_that_does_not_seek_is_left_out() {
        let frames = 2_000_000;
        let reads = Arc::new(AtomicUsize::new(0));
        let opened = Arc::new(AtomicUsize::new(0));
        let more = {
            let (reads, opened) = (reads.clone(), opened.clone());
            move || {
                opened.fetch_add(1, Ordering::Relaxed);
                Some(Ramp::new(frames, false, Duration::ZERO, &reads) as Box<dyn AudioSource>)
            }
        };
        let first = Ramp::new(frames, false, Duration::ZERO, &reads);
        let (pcm, _loading) =
            PcmBuffer::load_with(first, Arc::new(more), 0, 4, Duration::from_secs(5));
        wait(&pcm);
        is_ramp(&pcm, 0, frames);
        // each tried its part and found it had not seeked there: the first one read them all
        assert!(opened.load(Ordering::Relaxed) <= 3);
        let chunks: usize = (0..4).map(|_| (frames as usize / 4).div_ceil(CHUNK)).sum();
        let read = reads.load(Ordering::Relaxed);
        assert!(
            (chunks..=chunks + 3).contains(&read),
            "{read} reads of {chunks}"
        );
    }

    #[test]
    fn a_delay_adds_silence_or_cuts_the_start() {
        for shift in [0, 48, -1000, 2_500, -5_000_000] {
            let frames = 300_000;
            let reads = Arc::new(AtomicUsize::new(0));
            let first = Ramp::new(frames, true, Duration::ZERO, &reads);
            let (pcm, _loading) = PcmBuffer::load_with(first, Arc::new(none), shift, 1, PATIENCE);
            assert_eq!(pcm.len(), (frames + shift).max(0));
            wait(&pcm);
            is_ramp(&pcm, shift, frames);
        }
        // in milliseconds of the track's rate
        let src = MemorySource::new(48000, 1, vec![0.0; 10]);
        let (pcm, _loading) = PcmBuffer::load_in_background(Box::new(src), || None, 1);
        assert_eq!(pcm.len(), 58);
    }

    #[test]
    fn what_is_not_decoded_is_silence_and_a_dropped_load_stops() {
        let frames = 3_000_000;
        let reads = Arc::new(AtomicUsize::new(0));
        // every read after the first chunk takes its time
        let first = Ramp::new(frames, true, Duration::from_millis(30), &reads);
        let (pcm, loading) =
            PcmBuffer::load_with(first, Arc::new(none), 0, 1, Duration::from_secs(5));
        let since = Instant::now();
        while pcm.ready() == 0 {
            assert!(since.elapsed() < Duration::from_secs(30));
            std::thread::sleep(Duration::from_millis(1));
        }
        let ready = pcm.ready();
        assert!(ready < frames && !pcm.is_complete());
        assert_eq!(pcm.len(), frames);
        assert!(pcm.samples().len() as i64 >= ready);
        assert_eq!(pcm.get(CHUNK as i64 - 1), Ramp::at(CHUNK as i64 - 1));
        assert_eq!(pcm.get(frames - 1), 0);
        assert!(pcm.slice(frames - 10, frames).is_empty());
        drop(loading);
        std::thread::sleep(Duration::from_millis(200));
        let stopped = pcm.ready();
        std::thread::sleep(Duration::from_millis(200));
        assert_eq!(pcm.ready(), stopped);
        assert!(stopped < frames);
    }
}
