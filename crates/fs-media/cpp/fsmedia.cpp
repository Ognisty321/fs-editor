#include "fsmedia.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <audiosource.h>
#include <tracklist.h>
#include <videosource.h>

extern "C" {
#include <ass/ass.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace fsmedia {

namespace {

// MKV font attachments log "Could not find codec parameters" on every open; keep only errors.
[[maybe_unused]] const int quiet_ffmpeg = (av_log_set_level(AV_LOG_ERROR), 0);

std::string meta(const std::map<std::string, std::string> &m, const char *key) {
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

/// Index file name like Aegisub's: <name>_<hash of path>_<mtime>, so moving
/// or modifying the file never reuses a stale index.
std::filesystem::path cache_file(const std::filesystem::path &file, const std::filesystem::path &dir) {
    if (dir.empty())
        return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto abs = std::filesystem::absolute(file, ec).u8string();
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (char8_t c : abs) {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ull;
    }
    auto mtime = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    char tail[64];
    std::snprintf(tail, sizeof tail, "_%016llx_%lld", static_cast<unsigned long long>(h), static_cast<long long>(mtime));
    return dir / (file.filename().u8string() + std::u8string(tail, tail + std::strlen(tail)));
}

ProgressFunction wrap(const Progress &p) {
    if (!p)
        return nullptr;
    return [p](int, int64_t cur, int64_t total) { return p(cur, total); };
}

/// Rethrow BestSource/FFmpeg failures as MediaError with the file name.
template <class F>
auto guarded(const std::filesystem::path &file, F &&f) {
    try {
        return f();
    } catch (const MediaError &) {
        throw;
    } catch (const std::exception &e) {
        throw MediaError(file.filename().string() + ": " + e.what());
    }
}

// As a YCbCr Matrix header; "" when the matrix or the range is unset (Aegisub's real colour space).
const char *matrix_name(AVColorSpace cs, AVColorRange range) {
    if (range != AVCOL_RANGE_MPEG && range != AVCOL_RANGE_JPEG)
        return "";
    bool pc = range == AVCOL_RANGE_JPEG;
    switch (cs) {
    case AVCOL_SPC_BT709: return pc ? "PC.709" : "TV.709";
    case AVCOL_SPC_FCC: return pc ? "PC.FCC" : "TV.FCC";
    case AVCOL_SPC_SMPTE240M: return pc ? "PC.240M" : "TV.240M";
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return pc ? "PC.2020" : "TV.2020";
    case AVCOL_SPC_RGB: return "None";
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return pc ? "PC.601" : "TV.601";
    default: return "";
    }
}

/// libass logs everything to stdout by default; keep only errors and
/// warnings (levels 0-2), on stderr.
void ass_log(int level, const char *fmt, va_list args, void *) {
    if (level > 2)
        return;
    std::fputs("libass: ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
}

}  // namespace

std::vector<Track> list_tracks(const std::filesystem::path &file) {
    return guarded(file, [&] {
        BestTrackList tl(file, nullptr);
        std::vector<Track> out;
        for (int i = 0; i < tl.GetNumTracks(); ++i) {
            const auto &ti = tl.GetTrackInfo(i);
            auto md = tl.GetTrackMetadata(i);
            out.push_back(Track{
                .index = i,
                .type = ti.MediaTypeString,
                .codec = ti.CodecString,
                .language = meta(md, "language"),
                .title = meta(md, "title"),
                .is_default = (ti.Disposition & AV_DISPOSITION_DEFAULT) != 0,
            });
        }
        return out;
    });
}

std::vector<std::pair<std::string, std::vector<uint8_t>>> attached_fonts(const std::filesystem::path &file) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, reinterpret_cast<const char *>(file.u8string().c_str()), nullptr, nullptr) < 0)
        throw MediaError("cannot open " + file.filename().string());
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        const AVStream *st = ctx->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT || !st->codecpar->extradata)
            continue;
        const AVDictionaryEntry *name = av_dict_get(st->metadata, "filename", nullptr, 0);
        const AVDictionaryEntry *mime = av_dict_get(st->metadata, "mimetype", nullptr, 0);
        std::string n = name ? name->value : "";
        std::string m = mime ? mime->value : "";
        std::string lower = n;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool is_font = m.find("font") != std::string::npos || lower.ends_with(".ttf") || lower.ends_with(".otf") ||
                       lower.ends_with(".ttc") || lower.ends_with(".otc");
        if (!is_font)
            continue;
        const uint8_t *d = st->codecpar->extradata;
        out.emplace_back(n, std::vector<uint8_t>(d, d + st->codecpar->extradata_size));
    }
    avformat_close_input(&ctx);
    return out;
}

std::vector<std::pair<int64_t, std::string>> chapters(const std::filesystem::path &file) {
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, reinterpret_cast<const char *>(file.u8string().c_str()), nullptr, nullptr) < 0)
        throw MediaError("cannot open " + file.filename().string());
    std::unique_ptr<AVFormatContext *, void (*)(AVFormatContext **)> guard(&ctx, avformat_close_input);
    std::vector<std::pair<int64_t, std::string>> out;
    for (unsigned i = 0; i < ctx->nb_chapters; ++i) {
        const AVChapter *ch = ctx->chapters[i];
        const AVDictionaryEntry *title = av_dict_get(ch->metadata, "title", nullptr, 0);
        out.emplace_back(av_rescale_q(ch->start, ch->time_base, AVRational{1, 1000}), title ? title->value : "");
    }
    std::ranges::stable_sort(out, {}, [](const auto &c) { return c.first; });
    return out;
}

std::string subtitle_packets(const std::filesystem::path &file, int index, std::vector<SubPacket> &out) {
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, reinterpret_cast<const char *>(file.u8string().c_str()), nullptr, nullptr) < 0)
        throw MediaError("cannot open " + file.filename().string());
    std::unique_ptr<AVFormatContext *, void (*)(AVFormatContext **)> guard(&ctx, avformat_close_input);
    if (avformat_find_stream_info(ctx, nullptr) < 0 || index < 0 || unsigned(index) >= ctx->nb_streams ||
        ctx->streams[index]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
        throw MediaError("no subtitle stream " + std::to_string(index));
    const AVStream *st = ctx->streams[index];
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
        if (int(i) != index) ctx->streams[i]->discard = AVDISCARD_ALL;
    const auto *x = st->codecpar->extradata;
    std::string header(x ? reinterpret_cast<const char *>(x) : "", x ? size_t(st->codecpar->extradata_size) : 0);
    const AVRational ms{1, 1000};
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(ctx, pkt) >= 0) {
        if (pkt->stream_index == index && pkt->pts != AV_NOPTS_VALUE)
            out.push_back(SubPacket{av_rescale_q(pkt->pts, st->time_base, ms),
                                    av_rescale_q(pkt->duration, st->time_base, ms),
                                    std::string(reinterpret_cast<const char *>(pkt->data), size_t(pkt->size))});
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return header;
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<BestVideoSource> open_bs(const std::filesystem::path &file, int track,
                                         const std::filesystem::path &cache, const Progress &progress) {
    auto bs = std::make_unique<BestVideoSource>(file, false, std::nullopt, track, 0, 0,
                                                cache.empty() ? bcmDisable : bcmAutoAbsolutePath, cache, nullptr,
                                                wrap(progress));
    bs->SetMaxCacheSize(256ull << 20);
    return bs;
}

// BestSource's (videosource.cpp): 23.98 for 23.976..., so both indexes report the same rate
void nearest_common_rate(AVRational &fps) {
    constexpr std::array rates = {24, 25, 30, 48, 50, 60, 100, 120};
    const double d = av_q2d(fps);
    for (int r : rates) {
        const double delta = (r - r / 1.001) / 2.0;
        if (std::fabs(d - r) < delta) {
            fps = {r, 1};
            return;
        }
        if (r % 25 != 0 && std::fabs(d - r / 1.001) < delta) {
            fps = {r * 1000, 1001};
            return;
        }
    }
}

// BestSource's frame rate from the frame durations, on the sorted timestamps
AVRational guess_rate(const std::vector<int64_t> &pts, AVRational fps, AVRational tb) {
    std::map<int64_t, size_t> hist;
    for (size_t i = 0; i + 1 < pts.size(); ++i)
        ++hist[pts[i + 1] - pts[i]];
    if (hist.empty())
        return fps;
    const auto common = *std::ranges::max_element(hist, {}, [](const auto &h) { return h.second; });
    if (hist.size() == 1 && common.first > 0) {
        av_reduce(&fps.num, &fps.den, tb.den, common.first * tb.num, INT_MAX);
    } else if (pts.size() >= 20 && hist.size() > 1) {
        // drop the rarest durations while 95% of the frames stay
        const size_t total = pts.size() - 1;
        size_t used = total;
        while (hist.size() > 1) {
            const auto rare = std::ranges::min_element(hist, {}, [](const auto &h) { return h.second; });
            if ((used - rare->second) * 100 / total < 95)
                break;
            used -= rare->second;
            hist.erase(rare);
        }
        int64_t dur = 0;
        for (const auto &[d, n] : hist)
            dur += d * int64_t(n);
        av_reduce(&fps.num, &fps.den, int64_t(used) * tb.den, dur * tb.num, INT_MAX);
        nearest_common_rate(fps);
    }
    return fps;
}

} // namespace

// Aegisub's FFMS2 way: the index is the packets' timestamps and keyframe flags, read without
// decoding; a frame is found by seeking to the keyframe before it and decoding up to its timestamp.
struct VideoSource::Packets {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *dec = nullptr;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int stream = -1;
    std::vector<int64_t> pts;  // by frame, in presentation order
    std::vector<int64_t> keys; // keyframes' frame numbers
    int64_t last_duration = 0; // of the last frame, in the stream's time base; 0 unknown
    int64_t at = -1;           // the frame `frame` holds, -1 none; decoding on gives the ones after it
    int64_t shown = INT64_MIN; // its timestamp
    bool draining = false;

    ~Packets() {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    // The track's decoder; nullptr for a container whose packets are not a reliable index (AVI,
    // MPEG-TS: missing or split timestamps) or a track BestSource would not open the same way.
    static std::unique_ptr<Packets> open(const std::filesystem::path &file, int track) {
        auto p = std::make_unique<Packets>();
        if (avformat_open_input(&p->fmt, reinterpret_cast<const char *>(file.u8string().c_str()), nullptr, nullptr) < 0)
            return nullptr;
        const std::string_view format = p->fmt->iformat->name;
        if ((format.find("matroska") == std::string_view::npos && !format.starts_with("mov")) ||
            avformat_find_stream_info(p->fmt, nullptr) < 0)
            return nullptr;
        // BestSource's numbering: -1 the first video track, -2 the second...
        for (int i = 0, want = track; i < int(p->fmt->nb_streams) && p->stream < 0; ++i)
            if (p->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && (track >= 0 ? i == track : ++want == 0))
                p->stream = i;
        if (p->stream < 0)
            return nullptr;
        const AVStream *st = p->fmt->streams[p->stream];
        for (unsigned i = 0; i < p->fmt->nb_streams; ++i)
            if (int(i) != p->stream)
                p->fmt->streams[i]->discard = AVDISCARD_ALL;
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!codec || !(p->dec = avcodec_alloc_context3(codec)) ||
            avcodec_parameters_to_context(p->dec, st->codecpar) < 0)
            return nullptr;
        p->dec->thread_count = std::min(int(std::thread::hardware_concurrency()), 16); // as BestSource
        p->dec->pkt_timebase = st->time_base;
        p->dec->apply_cropping = 1;
        if (avcodec_open2(p->dec, codec, nullptr) < 0)
            return nullptr;
        return p;
    }

    // Reads every packet of the track; false when some have no or the same timestamp.
    bool scan(const Progress &progress) {
        const int64_t size = avio_size(fmt->pb);
        std::vector<std::pair<int64_t, bool>> frames; // pts, keyframe; in decode order
        int64_t last = INT64_MIN;
        for (int64_t n = 0; av_read_frame(fmt, pkt) >= 0; ++n) {
            if (pkt->stream_index == stream) {
                if (pkt->pts == AV_NOPTS_VALUE) {
                    av_packet_unref(pkt);
                    return false;
                }
                frames.emplace_back(pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0);
                if (pkt->pts > last) {
                    last = pkt->pts;
                    last_duration = pkt->duration;
                }
            }
            av_packet_unref(pkt);
            if (progress && n % 256 == 0 && !progress(avio_tell(fmt->pb), size))
                throw MediaError("indexing canceled");
        }
        std::ranges::sort(frames);
        for (size_t i = 0; i < frames.size(); ++i) {
            if (i > 0 && frames[i].first == frames[i - 1].first)
                return false;
            pts.push_back(frames[i].first);
            if (frames[i].second)
                keys.push_back(int64_t(i));
        }
        if (pts.empty())
            return false;
        seek_to(0);
        return true;
    }

    void seek_to(int64_t key) {
        av_seek_frame(fmt, stream, pts[key], AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        draining = false;
        at = -1;
        shown = INT64_MIN;
    }

    bool decode_next() {
        while (true) {
            const int r = avcodec_receive_frame(dec, frame);
            if (r == 0) {
                shown = frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
                const auto it = std::ranges::lower_bound(pts, shown);
                at = it != pts.end() && *it == shown ? it - pts.begin() : -1;
                return true;
            }
            if (r != AVERROR(EAGAIN) || draining) { // the frame is unreferenced now
                at = -1;
                shown = INT64_MIN;
                return false;
            }
            if (av_read_frame(fmt, pkt) < 0) {
                avcodec_send_packet(dec, nullptr);
                draining = true;
                continue;
            }
            if (pkt->stream_index == stream)
                avcodec_send_packet(dec, pkt); // a broken packet is skipped, as FFMS2 does
            av_packet_unref(pkt);
        }
    }

    const AVFrame *get(int64_t n) {
        if (at == n)
            return frame;
        const auto key_of = [&](int64_t back) { // the keyframe at or before n, `back` keyframes earlier
            const auto it = std::ranges::upper_bound(keys, n) - keys.begin() - 1 - back;
            return it < 0 || keys.empty() ? int64_t(0) : keys[size_t(it)];
        };
        // decoding on reaches n when no keyframe lies between the frame held and it
        bool seek = !(at >= 0 && n > at && key_of(0) <= at);
        for (int64_t back = 0; back < 3; ++back, seek = true) {
            if (seek)
                seek_to(key_of(back));
            while (decode_next()) {
                if (at == n)
                    return frame;
                if (shown > pts[n]) // passed it: the seek landed late, retry from a keyframe before
                    break;
            }
        }
        // shortcut: a frame the decoder never outputs (an open GOP's leading frames at the start)
        // shows the next one; the next open reads BestSource's index, which has no such frame
        return shown > pts[n] ? frame : nullptr;
    }
};

VideoSource::VideoSource(const std::filesystem::path &file, int track, const std::filesystem::path &cache_dir,
                         const Progress &progress) {
    guarded(file, [&] {
        auto cache = cache_file(file, cache_dir);
        if (auto p = cache.empty() ? nullptr : Packets::open(file, track)) {
            auto index = cache;
            index += "." + std::to_string(p->stream) + ".bsindex"; // BestSource's own name for it
            std::error_code ec;
            if (!std::filesystem::exists(index, ec) && p->scan(progress) && p->get(0)) {
                const AVStream *st = p->fmt->streams[p->stream];
                const AVFrame *f = p->get(0);
                info_.width = f->width;
                info_.height = f->height;
                info_.num_frames = int64_t(p->pts.size());
                AVRational fps = p->dec->framerate.num > 0 && p->dec->framerate.den > 0 ? p->dec->framerate
                                                                                       : st->r_frame_rate;
                if (fps.num <= 0 || fps.den <= 0)
                    fps = {30, 1};
                fps = guess_rate(p->pts, fps, st->time_base);
                info_.fps_num = fps.num;
                info_.fps_den = fps.den;
                AVRational sar = p->dec->sample_aspect_ratio;
                if (sar.num <= 0 || sar.den <= 0)
                    sar = st->sample_aspect_ratio;
                info_.sar_num = sar.num > 0 ? sar.num : 1;
                info_.sar_den = sar.den > 0 ? sar.den : 1;
                int64_t last = p->last_duration;
                if (last <= 0) // BestSource takes the most common duration: about a frame at that rate
                    last = av_rescale_q(1, av_inv_q(fps), st->time_base);
                info_.duration_s = double(p->pts.back() - p->pts.front() + std::max<int64_t>(1, last)) *
                                   av_q2d(st->time_base);
                if (const char *n = av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)))
                    info_.pixel_format = n;
                info_.matrix = matrix_name(f->colorspace, f->color_range);
                for (size_t n = 0; n < p->pts.size(); ++n)
                    timestamps_.push_back(p->pts[n] * 1000 * st->time_base.num / st->time_base.den);
                keyframes_ = p->keys;
                packets_ = std::move(p);
                index_in_background(file, track, cache);
                return 0;
            }
        }
        bs_ = open_bs(file, track, cache, progress);
        const auto &vp = bs_->GetVideoProperties();
        if (vp.NumFrames <= 0)
            throw MediaError("video track has no frames");
        info_.width = vp.Width;
        info_.height = vp.Height;
        info_.num_frames = vp.NumFrames;
        info_.fps_num = vp.FPS.Num;
        info_.fps_den = vp.FPS.Den > 0 ? vp.FPS.Den : 1;
        info_.sar_num = vp.SAR.Num > 0 ? vp.SAR.Num : 1;
        info_.sar_den = vp.SAR.Den > 0 ? vp.SAR.Den : 1;
        info_.duration_s = vp.Duration * vp.TimeBase.ToDouble();
        if (const char *n = av_get_pix_fmt_name(static_cast<AVPixelFormat>(vp.Format)))
            info_.pixel_format = n;

        timestamps_.reserve(vp.NumFrames);
        for (int64_t n = 0; n < vp.NumFrames; ++n) {
            const auto &fi = bs_->GetFrameInfo(n);
            if (fi.KeyFrame)
                keyframes_.push_back(n);
            timestamps_.push_back(fi.PTS * 1000 * vp.TimeBase.Num / vp.TimeBase.Den);
        }
        // matrix as the first frame reports it, unguessed (frame_rgba guesses by size when unset)
        std::unique_ptr<BestVideoFrame> f(bs_->GetFrame(0));
        if (f)
            info_.matrix = matrix_name(f->GetAVFrame()->colorspace, f->GetAVFrame()->color_range);
        return 0;
    });
}

void VideoSource::index_in_background(const std::filesystem::path &file, int track,
                                      const std::filesystem::path &cache) {
    // one indexer per index file: a second tab with the same video stays with its packets
    static std::mutex busy_m;
    static std::set<std::filesystem::path> busy;
    {
        std::lock_guard g(busy_m);
        if (!busy.insert(cache).second)
            return;
    }
    indexer_ = std::thread([this, file, track, cache] {
        struct Done {
            const std::filesystem::path &cache;
            ~Done() {
                std::lock_guard g(busy_m);
                busy.erase(cache);
            }
        } done{cache};
        try {
            {
                // half the cores: playback and the editor keep up meanwhile
                const int threads = std::max(1, int(std::thread::hardware_concurrency()) / 2);
                BestVideoSource index(file, false, std::nullopt, track, 0, threads, bcmAutoAbsolutePath, cache, nullptr,
                                      [this](int, int64_t, int64_t) { return !stop_; });
            }
            auto bs = open_bs(file, track, cache, {}); // from the index just written, every core to seek with
            const auto &vp = bs->GetVideoProperties();
            bool same = vp.NumFrames == info_.num_frames;
            for (int64_t n = 0; same && n < vp.NumFrames; ++n)
                same = bs->GetFrameInfo(n).PTS * 1000 * vp.TimeBase.Num / vp.TimeBase.Den == timestamps_[size_t(n)];
            // shortcut: other frames than the packets showed stay with the packets until the next open,
            // which reads the exact index; frame numbers must not change under the open script
            if (!same || stop_)
                return;
            std::lock_guard g(m_);
            bs_ = std::move(bs);
            packets_.reset();
        } catch (const std::exception &) {
            // canceled, or BestSource cannot index it: the packets go on serving
        }
    });
}

VideoSource::~VideoSource() {
    stop_ = true;
    if (indexer_.joinable())
        indexer_.join();
    sws_free_context(&sws_);
}

bool VideoSource::linear_decoding() const {
    std::lock_guard g(m_);
    return bs_ && bs_->GetLinearDecodingState();
}

bool VideoSource::exact() const {
    std::lock_guard g(m_);
    return bs_ != nullptr;
}

void VideoSource::set_matrix(Matrix m, bool full_range, bool use_override_range) {
    matrix_ = m;
    full_range_ = full_range;
    range_override_ = use_override_range;
}

void VideoSource::frame_rgba(int64_t n, std::span<uint8_t> out) {
    const int w = info_.width, h = info_.height;
    if (out.size() < static_cast<size_t>(w) * h * 4)
        throw MediaError("frame buffer too small");
    n = std::clamp<int64_t>(n, 0, info_.num_frames - 1);
    std::lock_guard g(m_);
    if (packets_) {
        const AVFrame *f = packets_->get(n);
        if (!f)
            throw MediaError("cannot decode frame " + std::to_string(n));
        return convert(f, out, n);
    }
    std::unique_ptr<BestVideoFrame> f(bs_->GetFrame(n));
    if (!f)
        throw MediaError("cannot decode frame " + std::to_string(n));
    convert(f->GetAVFrame(), out, n);
}

void VideoSource::convert(const AVFrame *frame, std::span<uint8_t> out, int64_t n) {
    const int w = info_.width, h = info_.height;
    AVFrame *src = av_frame_clone(frame);
    if (src->colorspace == AVCOL_SPC_UNSPECIFIED)
        src->colorspace = (src->width > 1024 || src->height >= 600) ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    if (src->color_range == AVCOL_RANGE_UNSPECIFIED)
        src->color_range = AVCOL_RANGE_MPEG;
    switch (matrix_) {
    case Matrix::Auto: break;
    case Matrix::Bt601: src->colorspace = AVCOL_SPC_BT470BG; break;
    case Matrix::Bt709: src->colorspace = AVCOL_SPC_BT709; break;
    case Matrix::Fcc: src->colorspace = AVCOL_SPC_FCC; break;
    case Matrix::Smpte240m: src->colorspace = AVCOL_SPC_SMPTE240M; break;
    case Matrix::Bt2020: src->colorspace = AVCOL_SPC_BT2020_NCL; break;
    }
    if (range_override_)
        src->color_range = full_range_ ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

    AVFrame *dst = av_frame_alloc();
    dst->format = AV_PIX_FMT_RGBA;
    dst->width = w;
    dst->height = h;
    dst->color_range = AVCOL_RANGE_JPEG;
    dst->colorspace = AVCOL_SPC_RGB;
    // no gamut/transfer conversion: the editor shows the video as encoded
    dst->color_primaries = src->color_primaries;
    dst->color_trc = src->color_trc;
    av_image_fill_arrays(dst->data, dst->linesize, out.data(), AV_PIX_FMT_RGBA, w, h, 1);

    if (!sws_) {
        sws_ = sws_alloc_context();
        sws_->flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
        sws_->threads = 0;
    }
    int r = sws_scale_frame(sws_, dst, src);
    av_frame_free(&dst);
    av_frame_free(&src);
    if (r < 0)
        throw MediaError("swscale failed on frame " + std::to_string(n));
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

AudioSource::AudioSource(const std::filesystem::path &file, int track, const std::filesystem::path &cache_dir,
                         const Progress &progress) {
    guarded(file, [&] {
        auto cache = cache_file(file, cache_dir);
        // -1: align to the first video track; -2 would leave the delay alone
        bool has_video = false;
        for (const auto &t : list_tracks(file))
            has_video |= t.type == "video";
        bs_ = std::make_unique<BestAudioSource>(file, track, has_video ? -1 : -2, 0,
                                                cache.empty() ? bcmDisable : bcmAutoAbsolutePath, cache, nullptr, 0,
                                                wrap(progress));
        // we read each track once, front to back, into our own PCM buffer: BestSource's
        // default 1 GB frame cache would only copy the whole track a second time
        bs_->SetMaxCacheSize(16ull << 20);
        const auto &ap = bs_->GetAudioProperties();
        info_.sample_rate = ap.SampleRate;
        info_.channels = ap.Channels;
        info_.num_samples = ap.NumSamples;
        info_.is_float = ap.AF.Float;
        info_.bits = ap.AF.Bits;
        info_.start_time_s = ap.StartTime;
        bytes_per_sample_ = ap.AF.BytesPerSample;
        if (info_.channels <= 0 || info_.sample_rate <= 0 || bytes_per_sample_ <= 0)
            throw MediaError("unsupported audio format");
        return 0;
    });
}

AudioSource::~AudioSource() = default;

void AudioSource::fill_f32(int64_t start, std::span<float> out) {
    const int ch = info_.channels;
    const int64_t count = static_cast<int64_t>(out.size()) / ch;
    if (count <= 0)
        return;
    std::ranges::fill(out, 0.f);
    // BestSource zero-fills outside the stream itself, but clamp to avoid
    // huge requests past the end
    int64_t s = std::max<int64_t>(start, 0);
    int64_t e = std::min(start + count, info_.num_samples);
    if (e <= s)
        return;
    buf_.resize(static_cast<size_t>(e - s) * ch * bytes_per_sample_);
    bs_->GetPackedAudio(buf_.data(), s, e - s);
    float *o = out.data() + (s - start) * ch;
    const size_t n = static_cast<size_t>(e - s) * ch;
    const uint8_t *b = buf_.data();
    if (info_.is_float && bytes_per_sample_ == 4) {
        std::memcpy(o, b, n * 4);
    } else if (info_.is_float && bytes_per_sample_ == 8) {
        for (size_t i = 0; i < n; ++i) {
            double d;
            std::memcpy(&d, b + i * 8, 8);
            o[i] = static_cast<float>(d);
        }
    } else if (bytes_per_sample_ == 1) {
        for (size_t i = 0; i < n; ++i)
            o[i] = (static_cast<int>(b[i]) - 128) / 128.f;
    } else if (bytes_per_sample_ == 2) {
        for (size_t i = 0; i < n; ++i) {
            int16_t v;
            std::memcpy(&v, b + i * 2, 2);
            o[i] = v / 32768.f;
        }
    } else if (bytes_per_sample_ == 4) {
        for (size_t i = 0; i < n; ++i) {
            int32_t v;
            std::memcpy(&v, b + i * 4, 4);
            o[i] = static_cast<float>(v / 2147483648.0);
        }
    } else {
        throw MediaError("unsupported sample size");
    }
}

// ---------------------------------------------------------------------------
// Subtitles
// ---------------------------------------------------------------------------

SubtitleRenderer::SubtitleRenderer() {
    lib_ = ass_library_init();
    if (!lib_)
        throw MediaError("libass initialisation failed");
    ass_set_message_cb(lib_, ass_log, nullptr);
    // off: every ass_read_memory would copy the script's [Fonts] into the library again, never
    // freed; the embedding code registers them once through add_font instead
    ass_set_extract_fonts(lib_, 0);
    renderer_ = ass_renderer_init(lib_);
    if (!renderer_)
        throw MediaError("libass renderer initialisation failed");
    ass_set_fonts(renderer_, nullptr, default_family_.c_str(), ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
}

SubtitleRenderer::~SubtitleRenderer() {
    if (track_)
        ass_free_track(track_);
    if (renderer_)
        ass_renderer_done(renderer_);
    if (lib_)
        ass_library_done(lib_);
}

void SubtitleRenderer::set_fonts(const std::string &default_family, const std::filesystem::path &fonts_dir) {
    if (!default_family.empty())
        default_family_ = default_family;
    if (!fonts_dir.empty())
        ass_set_fonts_dir(lib_, reinterpret_cast<const char *>(fonts_dir.u8string().c_str()));
    ass_set_fonts(renderer_, nullptr, default_family_.c_str(), ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
}

void SubtitleRenderer::add_font(const std::string &name, std::span<const uint8_t> data) {
    ass_add_font(lib_, name.c_str(), reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
}

void SubtitleRenderer::refresh_fonts() {
    // fonts added after ass_set_fonts are only seen after re-running it
    ass_set_fonts(renderer_, nullptr, default_family_.c_str(), ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
}

void SubtitleRenderer::set_script(std::string_view ass) {
    if (track_)
        ass_free_track(track_);
    std::string copy(ass);  // libass wants a mutable buffer
    track_ = ass_read_memory(lib_, copy.data(), copy.size(), nullptr);
    if (!track_)
        throw MediaError("libass could not parse the script");
}

template <bool Premul>
bool SubtitleRenderer::draw(int64_t ms, int width, int height, int video_w, int video_h, std::span<uint8_t> rgba) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4)
        throw MediaError("subtitle buffer too small");
    if constexpr (Premul)
        std::ranges::fill(rgba, uint8_t{0});
    if (!track_ || width <= 0 || height <= 0)
        return false;
    ass_set_frame_size(renderer_, width, height);
    ass_set_storage_size(renderer_, video_w > 0 ? video_w : width, video_h > 0 ? video_h : height);
    int changed = 0;
    bool drawn = false;
    for (ASS_Image *img = ass_render_frame(renderer_, track_, ms, &changed); img; img = img->next) {
        if (img->w <= 0 || img->h <= 0)
            continue;
        const uint32_t r = img->color >> 24, g = (img->color >> 16) & 0xff, b = (img->color >> 8) & 0xff;
        const uint32_t opacity = 255 - (img->color & 0xff);
        for (int y = 0; y < img->h; ++y) {
            const int dy = img->dst_y + y;
            if (dy < 0 || dy >= height)
                continue;
            const uint8_t *src = img->bitmap + static_cast<ptrdiff_t>(y) * img->stride;
            uint8_t *row = rgba.data() + static_cast<size_t>(dy) * width * 4;
            for (int x = 0; x < img->w; ++x) {
                const int dx = img->dst_x + x;
                if (dx < 0 || dx >= width)
                    continue;
                const uint32_t a = src[x] * opacity / 255;  // 0..255
                if (!a)
                    continue;
                drawn = true;
                uint8_t *p = row + dx * 4;
                const uint32_t ia = 255 - a;
                if constexpr (Premul) {
                    p[0] = static_cast<uint8_t>((r * a + p[0] * ia + 127) / 255);
                    p[1] = static_cast<uint8_t>((g * a + p[1] * ia + 127) / 255);
                    p[2] = static_cast<uint8_t>((b * a + p[2] * ia + 127) / 255);
                    p[3] = static_cast<uint8_t>(a + (p[3] * ia + 127) / 255);
                } else {
                    p[0] = static_cast<uint8_t>((r * a + p[0] * ia + 127) / 255);
                    p[1] = static_cast<uint8_t>((g * a + p[1] * ia + 127) / 255);
                    p[2] = static_cast<uint8_t>((b * a + p[2] * ia + 127) / 255);
                }
            }
        }
    }
    return drawn;
}

bool SubtitleRenderer::blend(int64_t ms, int width, int height, int video_w, int video_h, std::span<uint8_t> rgba) {
    return draw<false>(ms, width, height, video_w, video_h, rgba);
}

bool SubtitleRenderer::overlay(int64_t ms, int width, int height, int video_w, int video_h,
                               std::span<uint8_t> rgba_premul) {
    return draw<true>(ms, width, height, video_w, video_h, rgba_premul);
}

bool SubtitleRenderer::bounds(int64_t ms, int width, int height, std::span<int32_t> box) {
    if (!track_ || width <= 0 || height <= 0 || box.size() < 8)
        return false;
    ass_set_frame_size(renderer_, width, height);
    ass_set_storage_size(renderer_, width, height);
    int changed = 0;
    // [0] text and borders, [1] text alone
    std::array<std::array<int, 4>, 2> b{{{INT_MAX, INT_MAX, INT_MIN, INT_MIN}, {INT_MAX, INT_MAX, INT_MIN, INT_MIN}}};
    for (ASS_Image *img = ass_render_frame(renderer_, track_, ms, &changed); img; img = img->next) {
        if (img->type == ASS_Image::IMAGE_TYPE_SHADOW || img->w <= 0 || img->h <= 0)
            continue;
        const bool text = img->type == ASS_Image::IMAGE_TYPE_CHARACTER;
        for (int y = 0; y < img->h; ++y) {
            const uint8_t *src = img->bitmap + static_cast<ptrdiff_t>(y) * img->stride;
            int l = 0, r = img->w - 1;
            while (l < img->w && !src[l])
                ++l;
            if (l == img->w)
                continue;
            while (!src[r])
                --r;
            for (auto &k : text ? std::span(b) : std::span(b).first(1)) {
                k[0] = std::min(k[0], img->dst_x + l);
                k[1] = std::min(k[1], img->dst_y + y);
                k[2] = std::max(k[2], img->dst_x + r + 1);
                k[3] = std::max(k[3], img->dst_y + y + 1);
            }
        }
    }
    if (b[0][0] > b[0][2])
        return false;
    if (b[1][0] > b[1][2])
        b[1] = b[0];
    std::ranges::copy(b[0], box.begin());
    std::ranges::copy(b[1], box.begin() + 4);
    return true;
}

}  // namespace fsmedia
