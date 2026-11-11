// FS Editor media layer: frame-exact video/audio decoding (BestSource) and
// subtitle rendering (libass). Plain C++20 so the Qt GUI can use it
// directly; crates/fs-media/src/ffi.rs exposes it to Rust through cxx.
#pragma once

#include <array>
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
class BestAudioFrame;
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
/// How the packets are found. A Matroska file has an index of its blocks (Cues), and a subtitle
/// track read from there costs a seek and a few bytes per line; read as packets it costs the whole
/// file, video and audio with it. Auto takes the index where it is known to name every block of
/// the track and each block is what the index says, and reads the packets otherwise.
enum class SubtitleRead { Auto, Packets, Index };
/// `progress(done, total)` returns false to cancel, which throws. Index throws where Auto would
/// read the packets.
std::string subtitle_packets(const std::filesystem::path &file, int index, std::vector<SubPacket> &out,
                             const Progress &progress = {}, SubtitleRead how = SubtitleRead::Auto);

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
    int colorspace = 2;  // its AVColorSpace (2: unspecified) and AVColorRange (0: unspecified)
    int color_range = 0;
    bool hdr = false; // PQ or HLG transfer, or BT.2020 / P3 primaries (Aegisub's IsHDRFrame)
};

/// A frame as its decoder gives it, for a shader to make RGB of: three planes of 8-bit or 16-bit
/// (native-endian) samples, and the numbers that take them to what frame_rgba makes of the frame.
struct FramePlanes {
    /// False for a frame that has no such planes here: a pixel format that is not planar YUV of 8 to
    /// 16 bits with 4:4:4, 4:2:2 or 4:2:0 chroma (packed, with alpha, RGB, grey, big-endian), a
    /// matrix swscale has no table for, an interlaced frame (swscale converts its fields apart), one
    /// with mastering display metadata (swscale maps its colours) or one of another size than the
    /// video. frame_rgba is the way for those; the rest is unset.
    bool planar = false;
    /// False when a buffer was too small for its plane, and nothing was copied.
    bool copied = false;
    int width = 0, height = 0;               // of the luma plane
    int chroma_width = 0, chroma_height = 0; // of each chroma plane
    int bytes = 1;                           // of a sample
    /// R, G and B in 0..1 (to be clamped), each the sum of its four factors times Y, U, V and 1,
    /// with a sample as 0..1 of the 8 or 16 bits it is stored in: swscale's coefficients for the
    /// frame's matrix and range, as set_matrix overrides them and guessed where the frame has none.
    std::array<float, 12> to_rgb{};
    /// Where the first chroma sample lies, in luma samples right of and below the first luma sample
    /// (0.5: halfway to the next one), as swscale places it for the frame's chroma location.
    float chroma_x = 0, chroma_y = 0;
    /// The most a chroma value interpolated along a row can be, as a sample is 0..1: swscale keeps
    /// the row it has scaled across in 15 bits, which cuts what a cubic overshoots past the top.
    float chroma_top = 1;
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
    /// Decode frame `n` and copy its planes, each row after row without padding, into `y`, `u` and
    /// `v`. The sizes they need are in the answer, which says too when the planes were not copied.
    FramePlanes frame_planes(int64_t n, std::span<uint8_t> y, std::span<uint8_t> u, std::span<uint8_t> v);

private:
    struct Packets; // the packet index and its decoder (fsmedia.cpp)
    void convert(const AVFrame *frame, std::span<uint8_t> out, int64_t n);
    FramePlanes planes(const AVFrame *frame, std::span<uint8_t> y, std::span<uint8_t> u, std::span<uint8_t> v) const;
    /// The AVColorSpace and AVColorRange a frame is converted with.
    void colour(const AVFrame *frame, int &colorspace, int &range) const;
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
    /// The same samples averaged over the channels, as 16-bit: what the editor keeps of a track.
    /// Straight from the decoder's frames, each decoded once, so reading a track front to back in
    /// pieces costs no more than decoding it.
    void fill_mono(int64_t start, std::span<int16_t> out);
    /// False once BestSource gave up on seeking in this track (AC-3, DTS-HD MA: what it decodes
    /// after a seek is not what the index has): every read away from the last one then decodes
    /// from the start of the file.
    bool seeks() const;
    /// Set from any thread, it makes a read under way throw at the next frame (a seek BestSource
    /// retries for seconds); the source is of no use afterwards.
    std::shared_ptr<std::atomic_bool> canceller() const { return cancel_; }

private:
    std::unique_ptr<BestAudioSource> bs_;
    std::shared_ptr<std::atomic_bool> cancel_ = std::make_shared<std::atomic_bool>(false);
    AudioInfo info_;
    int bytes_per_sample_ = 0;
    int64_t delay_ = 0;                    // samples of silence before the track (it starts after the video)
    std::unique_ptr<BestAudioFrame> held_; // the frame fill_mono read last: the next read starts in it
    int64_t held_n_ = -1;
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
    /// The overlay again, but only the part of the frame the subtitles cover, for a view that draws
    /// them over the picture itself: `box` gets its left, top, right and bottom (exclusive; a
    /// transparent pixel wider than the subtitles on each side the frame has one), and `rgba_premul`
    /// its pixels, row after row from the buffer's start. The buffer must hold a whole frame; what
    /// lies past the box's pixels is left as it was.
    /// 0: nothing shows. 1: drawn. 2: what the call before this one drew or found, with nothing
    /// written: libass saw no change, and no other call of this renderer came between.
    int overlay_box(int64_t ms, int width, int height, int video_w, int video_h, std::span<uint8_t> rgba_premul,
                    std::span<int32_t> box);
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
    // the sizes of the last overlay_box, while libass's "no change" still speaks of what it drew
    std::array<int, 4> boxed_{};
    bool boxed_valid_ = false;
};

}  // namespace fsmedia
