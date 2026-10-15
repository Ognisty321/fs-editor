// FS Editor media layer: frame-exact video/audio decoding (BestSource) and
// subtitle rendering (libass). Plain C++20 so the Qt GUI can use it
// directly; crates/fs-media/src/ffi.rs exposes it to Rust through cxx.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

class BestVideoSource;
class BestAudioSource;
struct SwsContext;
struct AVFrame;
struct ass_library;
struct ass_renderer;
struct ass_track;

namespace fsmedia {

/// Called while indexing; return false to cancel.
using Progress = std::function<bool(int64_t done, int64_t total)>;

struct MediaError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Track {
    int index = 0;
    std::string type;   // "video", "audio", "subtitle", "attachment", ...
    std::string codec;
    std::string language;
    std::string title;
    bool is_default = false;
};

/// All streams of a file.
std::vector<Track> list_tracks(const std::filesystem::path &file);

/// Font attachments of a file (Matroska), as (filename, bytes).
std::vector<std::pair<std::string, std::vector<uint8_t>>> attached_fonts(const std::filesystem::path &file);

/// Chapters of a file (Matroska, MP4), as (start in ms, title), by start.
std::vector<std::pair<int64_t, std::string>> chapters(const std::filesystem::path &file);

/// Packets of one subtitle stream: the codec header (ASS: the script up to the
/// [Events] Format line) and each packet's text with times in milliseconds.
struct SubPacket {
    int64_t start_ms = 0;
    int64_t duration_ms = 0;
    std::string text;
};
std::string subtitle_packets(const std::filesystem::path &file, int index, std::vector<SubPacket> &out);

/// Colour matrix used to convert YUV to RGB.
enum class Matrix { Auto, Bt601, Bt709, Fcc, Smpte240m, Bt2020 };

struct VideoInfo {
    int width = 0;
    int height = 0;
    int64_t num_frames = 0;
    int fps_num = 0;
    int fps_den = 1;
    int sar_num = 1;
    int sar_den = 1;
    double duration_s = 0;
    std::string pixel_format;
    std::string matrix;  // the first frame's own, e.g. "TV.709"; "" when unset
};

class VideoSource {
public:
    /// Open (and index, or read the cached index of) a video track.
    /// `track` < 0 selects the n-th video track (-1 = first).
    /// With a cache directory, a Matroska or MP4 file BestSource has not indexed yet opens from its
    /// packets as Aegisub's FFMS2 does (a second for an episode, where BestSource decodes and hashes
    /// every frame for half a minute); BestSource indexes it in the background and takes over when
    /// its index has the same frames.
    VideoSource(const std::filesystem::path &file, int track, const std::filesystem::path &cache_dir,
                const Progress &progress = {});
    ~VideoSource();
    VideoSource(const VideoSource &) = delete;
    VideoSource &operator=(const VideoSource &) = delete;

    const VideoInfo &info() const { return info_; }
    /// Presentation time of every frame in milliseconds (VFR aware).
    const std::vector<int64_t> &timestamps_ms() const { return timestamps_; }
    /// Frame numbers of keyframes.
    const std::vector<int64_t> &keyframes() const { return keyframes_; }
    /// True once BestSource gave up on seeking (very slow random access).
    bool linear_decoding() const;
    /// True when frames come from BestSource's exact index, false while the packets serve them.
    bool exact() const;

    void set_matrix(Matrix m, bool full_range_override = false, bool use_override_range = false);

    /// Decode frame `n` and convert it to RGBA8 (width*height*4 bytes,
    /// opaque alpha) into `out`.
    void frame_rgba(int64_t n, std::span<uint8_t> out);

private:
    struct Packets; // the packet index and its decoder (fsmedia.cpp)
    void convert(const AVFrame *frame, std::span<uint8_t> out, int64_t n);
    void index_in_background(const std::filesystem::path &file, int track, const std::filesystem::path &cache);

    std::unique_ptr<BestVideoSource> bs_;
    std::unique_ptr<Packets> packets_; // until bs_ takes over
    mutable std::mutex m_;             // bs_ and packets_, swapped by the background indexer
    std::atomic_bool stop_{false};
    std::thread indexer_;
    VideoInfo info_;
    std::vector<int64_t> timestamps_;
    std::vector<int64_t> keyframes_;
    SwsContext *sws_ = nullptr;
    Matrix matrix_ = Matrix::Auto;
    bool range_override_ = false;
    bool full_range_ = false;
};

struct AudioInfo {
    int sample_rate = 0;
    int channels = 0;
    int64_t num_samples = 0;  // per channel
    bool is_float = false;
    int bits = 0;
    double start_time_s = 0;
};

class AudioSource {
public:
    /// `track` < 0 selects the n-th audio track. The delay is adjusted
    /// relative to the first video track, like Aegisub.
    AudioSource(const std::filesystem::path &file, int track, const std::filesystem::path &cache_dir,
                const Progress &progress = {});
    ~AudioSource();
    AudioSource(const AudioSource &) = delete;
    AudioSource &operator=(const AudioSource &) = delete;

    const AudioInfo &info() const { return info_; }
    /// Interleaved samples in [-1, 1] from sample `start`; `out.size()` must
    /// be a multiple of the channel count. Outside the stream is silence.
    void fill_f32(int64_t start, std::span<float> out);

private:
    std::unique_ptr<BestAudioSource> bs_;
    AudioInfo info_;
    int bytes_per_sample_ = 0;
    std::vector<uint8_t> buf_;
};

/// libass renderer for one script.
class SubtitleRenderer {
public:
    SubtitleRenderer();
    ~SubtitleRenderer();
    SubtitleRenderer(const SubtitleRenderer &) = delete;
    SubtitleRenderer &operator=(const SubtitleRenderer &) = delete;

    /// Default font family and an optional extra fonts directory.
    void set_fonts(const std::string &default_family, const std::filesystem::path &fonts_dir = {});
    /// Register a font (e.g. a Matroska attachment); seen after refresh_fonts().
    void add_font(const std::string &name, std::span<const uint8_t> data);
    /// Rebuild the font selection with the fonts added so far.
    void refresh_fonts();
    /// Replace the script (UTF-8 ASS text).
    void set_script(std::string_view ass);
    bool has_script() const { return track_ != nullptr; }

    /// Render the subtitles at `ms` for a `width x height` frame of a video
    /// whose native size is `video_w x video_h`, compositing onto `rgba`
    /// (straight RGBA8, alpha kept opaque). Returns true if anything was drawn.
    bool blend(int64_t ms, int width, int height, int video_w, int video_h, std::span<uint8_t> rgba);
    /// Same, but into a transparent premultiplied RGBA8 overlay (cleared first).
    bool overlay(int64_t ms, int width, int height, int video_w, int video_h, std::span<uint8_t> rgba_premul);
    /// The pixels the subtitles cover at `ms` in a `width x height` frame (storage the same):
    /// `box` gets left, top, right, bottom (exclusive) of the text with its borders, no shadows,
    /// then the same of the text alone (the first again when only borders show). False when
    /// nothing shows.
    bool bounds(int64_t ms, int width, int height, std::span<int32_t> box);

private:
    template <bool Premul>
    bool draw(int64_t ms, int width, int height, int video_w, int video_h, std::span<uint8_t> rgba);
    ass_library *lib_ = nullptr;
    ass_renderer *renderer_ = nullptr;
    ass_track *track_ = nullptr;
    std::string default_family_ = "Arial";
};

}  // namespace fsmedia
