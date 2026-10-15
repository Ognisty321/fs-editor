//! cxx bridge to the C++ media layer (`cpp/fsmedia.h`, glue in `cpp/bridge.cpp`).

pub struct ProgressSink<'a>(pub Box<dyn FnMut(i64, i64) -> bool + 'a>);

impl ProgressSink<'_> {
    /// Called from C++; a panic cannot unwind through the FFI boundary, so
    /// it cancels the operation instead.
    fn report(&mut self, done: i64, total: i64) -> bool {
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (self.0)(done, total)))
            .unwrap_or(false)
    }
}

#[cxx::bridge(namespace = "fsmedia::ffi")]
pub(crate) mod bridge {
    #[derive(Debug, Clone)]
    struct TrackInfo {
        index: i32,
        kind: String,
        codec: String,
        language: String,
        title: String,
        is_default: bool,
    }

    struct FontFile {
        name: String,
        data: Vec<u8>,
    }

    struct Chapter {
        start_ms: i64,
        title: String,
    }

    struct SubPacket {
        start_ms: i64,
        duration_ms: i64,
        text: String,
    }

    #[derive(Debug, Clone)]
    struct VideoProps {
        width: i32,
        height: i32,
        num_frames: i64,
        fps_num: i32,
        fps_den: i32,
        sar_num: i32,
        sar_den: i32,
        duration_s: f64,
        pixel_format: String,
        matrix: String,
    }

    #[derive(Debug, Clone, Copy)]
    struct AudioProps {
        sample_rate: i32,
        channels: i32,
        num_samples: i64,
        is_float: bool,
        bits: i32,
        start_time_s: f64,
    }

    extern "Rust" {
        type ProgressSink<'a>;
        fn report(self: &mut ProgressSink<'_>, done: i64, total: i64) -> bool;
    }

    unsafe extern "C++" {
        include!("fs-media/cpp/bridge.h");

        #[namespace = "fsmedia"]
        type VideoSource;
        #[namespace = "fsmedia"]
        type AudioSource;
        #[namespace = "fsmedia"]
        type SubtitleRenderer;

        fn list_tracks(path: &str) -> Result<Vec<TrackInfo>>;
        fn attached_fonts(path: &str) -> Result<Vec<FontFile>>;
        fn chapters(path: &str) -> Result<Vec<Chapter>>;
        fn subtitle_packets(path: &str, index: i32, out: &mut Vec<SubPacket>) -> Result<String>;

        fn open_video(
            path: &str,
            track: i32,
            cache_dir: &str,
            progress: &mut ProgressSink<'_>,
        ) -> Result<UniquePtr<VideoSource>>;
        fn video_props(v: &VideoSource) -> VideoProps;
        fn video_timestamps(v: &VideoSource) -> Vec<i64>;
        fn video_keyframes(v: &VideoSource) -> Vec<i64>;
        fn video_linear(v: &VideoSource) -> bool;
        fn video_exact(v: &VideoSource) -> bool;
        fn video_set_matrix(v: Pin<&mut VideoSource>, matrix: u8, range: u8);
        fn video_frame_rgba(v: Pin<&mut VideoSource>, n: i64, out: &mut [u8]) -> Result<()>;

        fn open_audio(
            path: &str,
            track: i32,
            cache_dir: &str,
            progress: &mut ProgressSink<'_>,
        ) -> Result<UniquePtr<AudioSource>>;
        fn audio_props(a: &AudioSource) -> AudioProps;
        fn audio_fill(a: Pin<&mut AudioSource>, start: i64, out: &mut [f32]) -> Result<()>;

        fn new_subtitle_renderer() -> Result<UniquePtr<SubtitleRenderer>>;
        fn subs_set_fonts(r: Pin<&mut SubtitleRenderer>, family: &str, dir: &str);
        fn subs_add_font(r: Pin<&mut SubtitleRenderer>, name: &str, data: &[u8]);
        fn subs_set_script(r: Pin<&mut SubtitleRenderer>, ass: &str) -> Result<()>;
        fn subs_blend(
            r: Pin<&mut SubtitleRenderer>,
            ms: i64,
            w: i32,
            h: i32,
            vw: i32,
            vh: i32,
            rgba: &mut [u8],
        ) -> Result<bool>;
        fn subs_overlay(
            r: Pin<&mut SubtitleRenderer>,
            ms: i64,
            w: i32,
            h: i32,
            vw: i32,
            vh: i32,
            rgba: &mut [u8],
        ) -> Result<bool>;
    }
}

// The C++ objects are used from one thread at a time (callers wrap them in a
// Mutex or own them exclusively); none of them touch thread-local state.
unsafe impl Send for bridge::VideoSource {}
unsafe impl Send for bridge::AudioSource {}
unsafe impl Send for bridge::SubtitleRenderer {}
