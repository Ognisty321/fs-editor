#include "fsmedia.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

#include <audiosource.h>
#include <tracklist.h>
#include <videosource.h>

extern "C" {
#include <ass/ass.h>
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

const char *matrix_name(AVColorSpace cs, AVColorRange range) {
    bool pc = range == AVCOL_RANGE_JPEG;
    switch (cs) {
    case AVCOL_SPC_BT709: return pc ? "PC.709" : "TV.709";
    case AVCOL_SPC_FCC: return pc ? "PC.FCC" : "TV.FCC";
    case AVCOL_SPC_SMPTE240M: return pc ? "PC.240M" : "TV.240M";
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return pc ? "PC.2020" : "TV.2020";
    case AVCOL_SPC_RGB: return "None";
    default: return pc ? "PC.601" : "TV.601";
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

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------

VideoSource::VideoSource(const std::filesystem::path &file, int track, const std::filesystem::path &cache_dir,
                         const Progress &progress) {
    guarded(file, [&] {
        auto cache = cache_file(file, cache_dir);
        bs_ = std::make_unique<BestVideoSource>(file, false, std::nullopt, track, 0, 0,
                                                cache.empty() ? bcmDisable : bcmAutoAbsolutePath, cache, nullptr,
                                                wrap(progress));
        bs_->SetMaxCacheSize(256ull << 20);
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
        // matrix as the first frame reports it (Aegisub guesses by size when unset)
        std::unique_ptr<BestVideoFrame> f(bs_->GetFrame(0));
        if (f) {
            const AVFrame *av = f->GetAVFrame();
            AVColorSpace cs = av->colorspace;
            if (cs == AVCOL_SPC_UNSPECIFIED)
                cs = (av->width > 1024 || av->height >= 600) ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
            info_.matrix = matrix_name(cs, av->color_range);
        }
        return 0;
    });
}

VideoSource::~VideoSource() { sws_free_context(&sws_); }

bool VideoSource::linear_decoding() const { return bs_->GetLinearDecodingState(); }

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
    std::unique_ptr<BestVideoFrame> f(bs_->GetFrame(n));
    if (!f)
        throw MediaError("cannot decode frame " + std::to_string(n));

    AVFrame *src = av_frame_clone(f->GetAVFrame());
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
    ass_set_extract_fonts(lib_, 1);
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

}  // namespace fsmedia
