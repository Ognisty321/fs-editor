#include "fsmedia.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <compare>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include <audiosource.h>
#include <tracklist.h>
#include <videosource.h>

#include "script.h"

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

// The first frame's colour metadata, as Aegisub reads it on open.
void read_colour(VideoInfo &i, const AVFrame *f) {
    i.matrix = matrix_name(f->colorspace, f->color_range);
    i.colorspace = f->colorspace;
    i.color_range = f->color_range;
    i.hdr = f->color_trc == AVCOL_TRC_SMPTE2084 || f->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
            f->color_primaries == AVCOL_PRI_BT2020 || f->color_primaries == AVCOL_PRI_SMPTE431 ||
            f->color_primaries == AVCOL_PRI_SMPTE432;
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
    if (script::kind_of(file) != script::Kind::None)
        return script::tracks(file);
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

namespace {

// A subtitle track of a Matroska file read from the file's index (Cues): FFmpeg and mkvmerge both
// write an entry there for every block of a subtitle track, with the block's place in its cluster.
// It is read as far as that takes: the index, each cluster's time, each block. Anything that is not
// exactly what is expected here (no index, a block without a place or a duration, compressed or
// laced blocks, a block that is not where or when the index says) throws NotThis, and the packets
// are read through FFmpeg instead, which knows the rest of the format.
class MatroskaIndex {
public:
    struct NotThis {};
    struct Block {
        int64_t pts = 0, duration = 0; // in the track's ticks, as FFmpeg's packets have them
        std::string data;
    };

    // `kinds`: Matroska's type of each track FFmpeg made a stream of, in their order; `which` of
    // them is the one to read. FFmpeg does not say which track a stream is, so the file's tracks
    // have to be those, one for one.
    MatroskaIndex(AVIOContext *pb, std::vector<uint64_t> kinds, size_t which)
        : pb_(pb), kinds_(std::move(kinds)), which_(which), size_(avio_size(pb)) {
        if (size_ <= 0 || which_ >= kinds_.size())
            throw NotThis{};
    }

    // How many blocks the index names for the track; what blocks() then reads.
    size_t find() {
        seek(0);
        Element ebml = head();
        if (ebml.id != 0x1A45DFA3 || ebml.size < 0)
            throw NotThis{};
        seek(ebml.end());
        Element segment = head();
        if (segment.id != 0x18538067)
            throw NotThis{};
        if (segment.size < 0 || segment.end() > size_) // written to no end, or cut short
            segment.size = size_ - segment.data;
        int64_t cues = -1;
        std::vector<int64_t> more; // further SeekHeads the first one names
        bool tracks = false;
        const auto seek_head = [&](const Element &sh) {
            children(sh, [&](const Element &entry) {
                if (entry.id != 0x4DBB)
                    return true;
                uint64_t id = 0;
                int64_t at = -1;
                children(entry, [&](const Element &e) {
                    if (e.id == 0x53AB)
                        id = number(e);
                    else if (e.id == 0x53AC)
                        at = segment.data + int64_t(number(e));
                    return true;
                });
                if (id == 0x1C53BB6B && cues < 0)
                    cues = at;
                else if (id == 0x114D9B74 && at >= 0 && more.size() < 4)
                    more.push_back(at);
                return true;
            });
        };
        children(segment, [&](const Element &e) {
            switch (e.id) {
            case 0x114D9B74: seek_head(e); break;
            case 0x1654AE6B:
                read_tracks(e);
                tracks = true;
                break;
            case 0x1C53BB6B: cues = e.at; break;
            case 0x1F43B675: return false; // the clusters: what describes the file is before them
            }
            return true;
        });
        if (!tracks)
            throw NotThis{};
        for (size_t i = 0; cues < 0 && i < more.size(); ++i) {
            seek(more[i]);
            const Element sh = head();
            if (sh.id != 0x114D9B74 || sh.size < 0 || sh.end() > segment.end())
                throw NotThis{};
            seek_head(sh);
        }
        if (cues < 0 || cues >= segment.end())
            throw NotThis{};
        seek(cues);
        const Element c = head();
        if (c.id != 0x1C53BB6B || c.size < 0 || c.end() > segment.end())
            throw NotThis{};
        children(c, [&](const Element &point) {
            if (point.id != 0xBB)
                return true;
            int64_t time = -1;
            const size_t first = found_.size();
            children(point, [&](const Element &e) {
                if (e.id == 0xB3) {
                    time = int64_t(number(e));
                } else if (e.id == 0xB7) {
                    uint64_t track = 0;
                    int64_t cluster = -1, place = -1;
                    children(e, [&](const Element &p) {
                        if (p.id == 0xF7)
                            track = number(p);
                        else if (p.id == 0xF1)
                            cluster = segment.data + int64_t(number(p));
                        else if (p.id == 0xF0)
                            place = int64_t(number(p));
                        return true;
                    });
                    if (track == track_) {
                        if (cluster < 0 || place < 0)
                            throw NotThis{};
                        found_.push_back({cluster, place, 0});
                    }
                }
                return true;
            });
            if (time < 0 && found_.size() > first)
                throw NotThis{};
            for (size_t i = first; i < found_.size(); ++i)
                found_[i].time = time;
            return true;
        });
        end_ = segment.end();
        std::ranges::sort(found_); // as they are in the file: the order FFmpeg gives the packets in
        found_.erase(std::unique(found_.begin(), found_.end()), found_.end());
        return found_.size();
    }

    std::vector<Block> blocks(const Progress &progress) {
        std::vector<Block> out;
        out.reserve(found_.size());
        Element cluster;
        int64_t cluster_time = -1;
        for (size_t i = 0; i < found_.size(); ++i) {
            if (progress && i % 64 == 0 && !progress(int64_t(i), int64_t(found_.size())))
                throw MediaError("reading the subtitles canceled");
            const Found &f = found_[i];
            if (cluster.at != f.cluster) {
                seek(f.cluster);
                cluster = head();
                if (cluster.id != 0x1F43B675)
                    throw NotThis{};
                if (cluster.size < 0)
                    cluster.size = end_ - cluster.data;
                if (cluster.end() > end_)
                    throw NotThis{};
                // its time, which a block's is counted from, comes before the blocks
                cluster_time = -1;
                children(cluster, [&](const Element &e) {
                    if (e.id == 0xE7)
                        cluster_time = int64_t(number(e));
                    else if (e.id == 0xA0 || e.id == 0xA3)
                        throw NotThis{};
                    return cluster_time < 0;
                });
                if (cluster_time < 0)
                    throw NotThis{};
            }
            // A block's place is counted from where the cluster's contents start. FFmpeg's muxer
            // counts from the cluster's id instead: the file's first block tells which it is, being
            // there, of the track and at the index's time at one of the two only.
            if (from_id_ < 0) {
                try {
                    out.push_back(block(cluster, cluster.data + f.place, cluster_time, f.time));
                    from_id_ = 0;
                } catch (const NotThis &) {
                    out.push_back(block(cluster, cluster.at + f.place, cluster_time, f.time));
                    from_id_ = 1;
                }
            } else {
                out.push_back(block(cluster, (from_id_ ? cluster.at : cluster.data) + f.place, cluster_time, f.time));
            }
        }
        return out;
    }

private:
    struct Element {
        uint32_t id = 0;
        int64_t at = -1;   // where its id starts
        int64_t data = 0;  // where its contents do
        int64_t size = -1; // -1: not written (a stream that was never seeked back in)
        int64_t end() const { return data + size; }
    };
    struct Found {
        int64_t cluster, place, time;
        auto operator<=>(const Found &) const = default;
    };

    void seek(int64_t at) {
        if (at < 0 || at > size_ || avio_seek(pb_, at, SEEK_SET) < 0)
            throw NotThis{};
    }

    int byte() {
        const int b = avio_r8(pb_);
        if (pb_->eof_reached || pb_->error)
            throw NotThis{};
        return b;
    }

    // A size-style number: its first byte's leading zeros say how many bytes follow.
    uint64_t vint(bool *unknown = nullptr) {
        const int first = byte();
        if (first == 0)
            throw NotThis{};
        int len = 1;
        for (int mask = 0x80; !(first & mask); mask >>= 1)
            ++len;
        uint64_t v = uint64_t(first) & (0xFFu >> len);
        bool ones = v == (0xFFu >> len);
        for (int i = 1; i < len; ++i) {
            const int b = byte();
            v = v << 8 | uint64_t(b);
            ones = ones && b == 0xFF;
        }
        if (unknown)
            *unknown = ones;
        else if (ones)
            throw NotThis{};
        return v;
    }

    Element head() {
        Element e;
        e.at = avio_tell(pb_);
        const int first = byte();
        int len = 1;
        for (int mask = 0x80; len <= 4 && !(first & mask); mask >>= 1)
            ++len;
        if (len > 4)
            throw NotThis{};
        e.id = uint32_t(first);
        for (int i = 1; i < len; ++i)
            e.id = e.id << 8 | uint32_t(byte());
        bool unknown = false;
        const uint64_t size = vint(&unknown);
        e.data = avio_tell(pb_);
        if (!unknown) {
            if (size > uint64_t(size_))
                throw NotThis{};
            e.size = int64_t(size);
        }
        return e;
    }

    uint64_t number(const Element &e) {
        if (e.size < 0 || e.size > 8)
            throw NotThis{};
        seek(e.data);
        uint64_t v = 0;
        for (int64_t i = 0; i < e.size; ++i)
            v = v << 8 | uint64_t(byte());
        return v;
    }

    // The block the index says is at `at`, at `time`.
    Block block(const Element &cluster, int64_t at, int64_t cluster_time, int64_t time) {
        if (at < cluster.data || at >= cluster.end())
            throw NotThis{};
        seek(at);
        const Element group = head();
        // a SimpleBlock has no duration: a subtitle is written as a BlockGroup
        if (group.id != 0xA0 || group.size < 0 || group.end() > cluster.end())
            throw NotThis{};
        Block b;
        bool have = false;
        int64_t duration = -1;
        children(group, [&](const Element &e) {
            if (e.id == 0x9B) {
                duration = int64_t(number(e));
            } else if (e.id == 0xA1) {
                if (have)
                    throw NotThis{};
                const uint64_t track = vint();
                const int16_t rel = int16_t(avio_rb16(pb_));
                const int flags = byte();
                const int64_t n = e.end() - avio_tell(pb_);
                // 0x06: several frames laced into one block
                if (track != track_ || (flags & 0x06) || n < 0 || n > (16 << 20))
                    throw NotThis{};
                b.data.resize(size_t(n));
                if (n > 0 && avio_read(pb_, reinterpret_cast<unsigned char *>(b.data.data()), int(n)) != int(n))
                    throw NotThis{};
                b.pts = cluster_time + rel;
                have = true;
            }
            return true;
        });
        if (!have || duration < 0 || b.pts != time)
            throw NotThis{};
        b.duration = duration;
        return b;
    }

    // `each` for every element in `parent` till it returns false; it may read in the element.
    template <class F>
    void children(const Element &parent, F &&each) {
        for (int64_t at = parent.data; at < parent.end();) {
            seek(at);
            const Element e = head();
            if (e.size < 0 || e.end() > parent.end())
                throw NotThis{};
            if (!each(e))
                return;
            at = e.end();
        }
    }

    void read_tracks(const Element &tracks) {
        size_t n = 0; // of those FFmpeg makes streams of: video, audio, subtitles, metadata with a codec
        children(tracks, [&](const Element &entry) {
            if (entry.id != 0xAE)
                return true;
            uint64_t num = 0, kind = 0;
            bool plain = true, codec = false;
            children(entry, [&](const Element &e) {
                if (e.id == 0xD7)
                    num = number(e);
                else if (e.id == 0x83)
                    kind = number(e);
                else if (e.id == 0x86)
                    codec = e.size > 0;
                // compressed or encrypted blocks, a time scale of its own, a delay: FFmpeg's to work out
                else if (e.id == 0x6D80 || e.id == 0x23314F || e.id == 0x56AA)
                    plain = false;
                return true;
            });
            if (!codec || (kind != 1 && kind != 2 && kind != 0x11 && kind != 0x21))
                return true;
            if (n >= kinds_.size() || kinds_[n] != kind || num == 0)
                throw NotThis{};
            if (n == which_) {
                if (!plain)
                    throw NotThis{};
                track_ = num;
            }
            ++n;
            return true;
        });
        if (n != kinds_.size())
            throw NotThis{};
    }

    AVIOContext *pb_;
    std::vector<uint64_t> kinds_;
    size_t which_;
    uint64_t track_ = 0;
    int64_t size_;
    int64_t end_ = 0;  // of the segment
    int from_id_ = -1; // where a block's place is counted from: -1 not known yet
    std::vector<Found> found_;
};

// Whether the index of a subtitle track names every block of it: FFmpeg's muxer adds an entry for
// each subtitle packet it writes, and mkvmerge counts the track's blocks in a tag of its own.
bool index_is_whole(const AVFormatContext *ctx, const AVStream *st, size_t entries) {
    if (const AVDictionaryEntry *e = av_dict_get(ctx->metadata, "encoder", nullptr, 0);
        e && std::string_view(e->value).starts_with("Lavf"))
        return true;
    const AVDictionaryEntry *e = nullptr;
    while ((e = av_dict_iterate(st->metadata, e)))
        if (std::string_view(e->key).starts_with("NUMBER_OF_FRAMES") && std::to_string(entries) == e->value)
            return true;
    return false;
}

} // namespace

std::string subtitle_packets(const std::filesystem::path &file, int index, std::vector<SubPacket> &out,
                             const Progress &progress, SubtitleRead how) {
    const std::u8string url = file.u8string();
    // Read as packets, the other streams are dropped inside FFmpeg: a call for the next packet comes
    // back at the next subtitle only, after the last one at the end of the file. So the progress is
    // told, and a cancel taken, from the reads of the file themselves, every 64th of them.
    struct Reads {
        const Progress &progress;
        AVFormatContext *ctx = nullptr;
        int64_t size = 0, count = 0;
        bool on = false, stop = false;
    } reads{progress};
    AVFormatContext *ctx = avformat_alloc_context();
    if (!ctx)
        throw MediaError("cannot open " + file.filename().string());
    ctx->interrupt_callback = {[](void *opaque) -> int {
                                   auto &r = *static_cast<Reads *>(opaque);
                                   if (r.on && !r.stop && r.progress && r.count++ % 64 == 0 && r.ctx->pb)
                                       r.stop = !r.progress(r.ctx->pb->pos, r.size);
                                   return r.stop;
                               },
                               &reads};
    if (avformat_open_input(&ctx, reinterpret_cast<const char *>(url.c_str()), nullptr, nullptr) < 0)
        throw MediaError("cannot open " + file.filename().string());
    std::unique_ptr<AVFormatContext *, void (*)(AVFormatContext **)> guard(&ctx, avformat_close_input);
    if (avformat_find_stream_info(ctx, nullptr) < 0 || index < 0 || unsigned(index) >= ctx->nb_streams ||
        ctx->streams[index]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
        throw MediaError("no subtitle stream " + std::to_string(index));
    const AVStream *st = ctx->streams[index];
    const auto *x = st->codecpar->extradata;
    std::string header(x ? reinterpret_cast<const char *>(x) : "", x ? size_t(st->codecpar->extradata_size) : 0);
    const AVRational ms{1, 1000};

    // text as the blocks hold it: FFmpeg takes WebVTT's apart and puts it together again
    const AVCodecID codec = st->codecpar->codec_id;
    const bool as_stored = codec == AV_CODEC_ID_ASS || codec == AV_CODEC_ID_SSA || codec == AV_CODEC_ID_SUBRIP ||
                           codec == AV_CODEC_ID_TEXT;
    // the tracks as FFmpeg has them: its streams but the attachments, which it adds after them
    std::vector<uint64_t> kinds;
    size_t which = 0;
    bool known = true;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        const AVMediaType type = ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_ATTACHMENT)
            continue;
        if (int(i) == index)
            which = kinds.size();
        known = known && (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO || type == AVMEDIA_TYPE_SUBTITLE ||
                          type == AVMEDIA_TYPE_DATA);
        kinds.push_back(type == AVMEDIA_TYPE_VIDEO ? 1 : type == AVMEDIA_TYPE_AUDIO ? 2 : type == AVMEDIA_TYPE_SUBTITLE ? 0x11 : 0x21);
    }
    if (how != SubtitleRead::Packets && as_stored && known &&
        std::string_view(ctx->iformat->name).find("matroska") != std::string_view::npos) {
        AVIOContext *pb = nullptr;
        if (avio_open(&pb, reinterpret_cast<const char *>(url.c_str()), AVIO_FLAG_READ) >= 0) {
            std::unique_ptr<AVIOContext *, int (*)(AVIOContext **)> close(&pb, avio_closep);
            try {
                MatroskaIndex mkv(pb, std::move(kinds), which);
                if (const size_t n = mkv.find(); n > 0 && index_is_whole(ctx, st, n)) {
                    for (auto &b : mkv.blocks(progress))
                        out.push_back(SubPacket{av_rescale_q(b.pts, st->time_base, ms),
                                                av_rescale_q(b.duration, st->time_base, ms), std::move(b.data)});
                    return header;
                }
            } catch (const MatroskaIndex::NotThis &) {
                out.clear();
            }
        }
    }
    if (how == SubtitleRead::Index)
        throw MediaError("the subtitles cannot be read from the file's index");

    for (unsigned i = 0; i < ctx->nb_streams; ++i)
        if (int(i) != index) ctx->streams[i]->discard = AVDISCARD_ALL;
    reads.ctx = ctx;
    reads.size = avio_size(ctx->pb);
    reads.on = true;
    AVPacket *pkt = av_packet_alloc();
    const std::unique_ptr<AVPacket *, void (*)(AVPacket **)> free_pkt(&pkt, av_packet_free);
    // a file small enough to be in memory already is read no more: asked here once
    reads.stop = progress && !progress(avio_tell(ctx->pb), reads.size);
    while (!reads.stop && av_read_frame(ctx, pkt) >= 0) {
        if (pkt->stream_index == index && pkt->pts != AV_NOPTS_VALUE)
            out.push_back(SubPacket{av_rescale_q(pkt->pts, st->time_base, ms),
                                    av_rescale_q(pkt->duration, st->time_base, ms),
                                    std::string(reinterpret_cast<const char *>(pkt->data), size_t(pkt->size))});
        av_packet_unref(pkt);
    }
    reads.on = false;
    if (reads.stop)
        throw MediaError("reading the subtitles canceled");
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
    // The frames decoded last, by number, as BestSource keeps them once it takes over: a step back
    // is then no seek to the keyframe and all its frames again (190 ms a step in an episode).
    struct Held {
        AVFrame *frame;
        size_t bytes;
    };
    std::map<int64_t, Held> held;
    std::deque<int64_t> held_order; // oldest first
    size_t held_bytes = 0;
    static constexpr size_t held_max = 256u << 20; // BestSource's cache here (open_bs)

    void hold() {
        if (at < 0 || held.contains(at))
            return;
        AVFrame *f = av_frame_clone(frame); // a reference: the decoder takes another buffer
        if (!f)
            return;
        const int size = av_image_get_buffer_size(static_cast<AVPixelFormat>(f->format), f->width, f->height, 1);
        held.emplace(at, Held{f, size_t(std::max(size, 1))});
        held_order.push_back(at);
        held_bytes += held[at].bytes;
        while (held_bytes > held_max && held_order.size() > 1) {
            const auto old = held.find(held_order.front());
            held_bytes -= old->second.bytes;
            av_frame_free(&old->second.frame);
            held.erase(old);
            held_order.pop_front();
        }
    }

    ~Packets() {
        for (auto &[n, h] : held)
            av_frame_free(&h.frame);
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
                hold();
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
        if (const auto it = held.find(n); it != held.end())
            return it->second.frame; // the decoder stays where it is, for the frames after its own
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
    if (script::kind_of(file) != script::Kind::None) {
        // a script's clip: nothing to index, every frame is the one asked for
        script_ = script::open(file, script::Want::Video);
        const AVFrame *f = script_->frame(0);
        info_ = script_->video;
        info_.sar_num = f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio.num : 1;
        info_.sar_den = f->sample_aspect_ratio.num > 0 && f->sample_aspect_ratio.den > 0 ? f->sample_aspect_ratio.den : 1;
        if (const char *n = av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)))
            info_.pixel_format = n;
        read_colour(info_, f);
        if (script_->source_colorspace >= 0) {
            // frames the script library made RGB: the editor is told the clip's own colour space
            info_.colorspace = script_->source_colorspace;
            info_.color_range = script_->source_range;
            info_.matrix = matrix_name(static_cast<AVColorSpace>(info_.colorspace),
                                       static_cast<AVColorRange>(info_.color_range));
        }
        timestamps_ = std::move(script_->timestamps);
        keyframes_ = std::move(script_->keyframes);
        return;
    }
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
                read_colour(info_, f);
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
            read_colour(info_, f->GetAVFrame());
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
    return bs_ != nullptr || script_ != nullptr;
}

void VideoSource::set_matrix(Matrix m, bool full_range, bool use_override_range) {
    matrix_ = m;
    full_range_ = full_range;
    range_override_ = use_override_range;
    if (script_)
        script_->set_matrix(m, full_range, use_override_range);
}

void VideoSource::frame_rgba(int64_t n, std::span<uint8_t> out) {
    const int w = info_.width, h = info_.height;
    if (out.size() < static_cast<size_t>(w) * h * 4)
        throw MediaError("frame buffer too small");
    n = std::clamp<int64_t>(n, 0, info_.num_frames - 1);
    std::lock_guard g(m_);
    if (script_)
        return convert(script_->frame(n), out, n);
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

FramePlanes VideoSource::frame_planes(int64_t n, std::span<uint8_t> y, std::span<uint8_t> u, std::span<uint8_t> v) {
    n = std::clamp<int64_t>(n, 0, info_.num_frames - 1);
    std::lock_guard g(m_);
    // a script's frame is in its clip's format, as a decoder's is in the file's
    if (script_)
        return planes(script_->frame(n), y, u, v);
    if (packets_) {
        const AVFrame *f = packets_->get(n);
        if (!f)
            throw MediaError("cannot decode frame " + std::to_string(n));
        return planes(f, y, u, v);
    }
    std::unique_ptr<BestVideoFrame> f(bs_->GetFrame(n));
    if (!f)
        throw MediaError("cannot decode frame " + std::to_string(n));
    return planes(f->GetAVFrame(), y, u, v);
}

void VideoSource::colour(const AVFrame *frame, int &colorspace, int &range) const {
    colorspace = frame->colorspace;
    range = frame->color_range;
    if (colorspace == AVCOL_SPC_UNSPECIFIED)
        colorspace = (frame->width > 1024 || frame->height >= 600) ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    if (range == AVCOL_RANGE_UNSPECIFIED)
        range = AVCOL_RANGE_MPEG;
    switch (matrix_) {
    case Matrix::Auto: break;
    case Matrix::Bt601: colorspace = AVCOL_SPC_BT470BG; break;
    case Matrix::Bt709: colorspace = AVCOL_SPC_BT709; break;
    case Matrix::Fcc: colorspace = AVCOL_SPC_FCC; break;
    case Matrix::Smpte240m: colorspace = AVCOL_SPC_SMPTE240M; break;
    case Matrix::Bt2020: colorspace = AVCOL_SPC_BT2020_NCL; break;
    }
    if (range_override_)
        range = full_range_ ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

// What convert() below has swscale do, left to a shader: the planes as they are, and the numbers
// swscale would convert them with.
FramePlanes VideoSource::planes(const AVFrame *frame, std::span<uint8_t> y, std::span<uint8_t> u,
                                std::span<uint8_t> v) const {
    FramePlanes p;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(format);
    if (!d || d->flags != AV_PIX_FMT_FLAG_PLANAR || d->nb_components != 3 || d->log2_chroma_w > 1 ||
        d->log2_chroma_h > 1 || (frame->flags & AV_FRAME_FLAG_INTERLACED) || frame->width != info_.width ||
        frame->height != info_.height)
        return p;
    const int depth = d->comp[0].depth, bytes = depth > 8 ? 2 : 1;
    if (depth < 8 || depth > 16)
        return p;
    for (int i = 0; i < 3; ++i)
        if (d->comp[i].plane != i || d->comp[i].step != bytes || d->comp[i].offset || d->comp[i].shift ||
            d->comp[i].depth != depth)
            return p;
    // An HDR10 frame says what display it was mastered on, and swscale then maps its colours from
    // that display's black level and gamut to the plain ones of the frame convert() gives it (a
    // 3D LUT, 40 ms a 4K frame). Such a frame keeps the picture it has had, on the CPU.
    if (av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA))
        return p;
    int cs, range;
    colour(frame, cs, range);
    switch (cs) { // the ones swscale converts (sws_test_colorspace)
    case AVCOL_SPC_RGB:
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_FCC:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
    case AVCOL_SPC_BT2020_NCL: break;
    default: return p;
    }
    // swscale takes the deprecated full range formats as full range whatever the frame says
    const bool full = range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
                      format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P;

    p.planar = true;
    p.width = frame->width;
    p.height = frame->height;
    p.chroma_width = AV_CEIL_RSHIFT(frame->width, d->log2_chroma_w);
    p.chroma_height = AV_CEIL_RSHIFT(frame->height, d->log2_chroma_h);
    p.bytes = bytes;

    // swscale's own numbers (ff_yuv2rgb_c_init_tables), in 8-bit levels: its table has the chroma
    // factors for limited range, times 65536; a deeper sample counts as its value over 2^(depth - 8)
    const int *t = sws_getCoefficients(cs);
    const double cy = full ? 1.0 : 255.0 / 219.0, oy = full ? 0.0 : 16.0;
    const double c = (full ? 224.0 / 255.0 : 1.0) / 65536.0;
    const double crv = t[0] * c, cbu = t[1] * c, cgu = -t[2] * c, cgv = -t[3] * c;
    // a sample as 0..1 of its stored bits, to 8-bit levels, and the result from levels to 0..1
    const double k = double((1 << (8 * bytes)) - 1) / double(1 << (depth - 8)) / 255.0;
    const double rows[3][4] = {{cy * k, 0, crv * k, -(cy * oy + crv * 128) / 255},
                               {cy * k, cgu * k, cgv * k, -(cy * oy + (cgu + cgv) * 128) / 255},
                               {cy * k, cbu * k, 0, -(cy * oy + cbu * 128) / 255}};
    for (int i = 0; i < 12; ++i)
        p.to_rgb[size_t(i)] = float(rows[i / 4][i % 4]);

    // ff_sws_chroma_pos: an unset location is the centre, and a plane as large as the luma has none
    int cx = 0, cy_pos = 0;
    if (av_chroma_location_enum_to_pos(&cx, &cy_pos, frame->chroma_location) < 0)
        cx = cy_pos = 128;
    p.chroma_x = d->log2_chroma_w ? float(cx) / 256.f : 0.f;
    p.chroma_y = d->log2_chroma_h ? float(cy_pos) / 256.f : 0.f;
    // hScale8To15_c, hScale16To15_c: a sample of `depth` bits times 2^(15 - depth), 32767 at most
    p.chroma_top = float(32767.0 / std::ldexp(1.0, 15 - depth) / double((1 << (8 * bytes)) - 1));

    const size_t row = size_t(p.width) * size_t(bytes), crow = size_t(p.chroma_width) * size_t(bytes);
    if (y.size() < row * size_t(p.height) || u.size() < crow * size_t(p.chroma_height) ||
        v.size() < crow * size_t(p.chroma_height))
        return p;
    av_image_copy_plane(y.data(), int(row), frame->data[0], frame->linesize[0], int(row), p.height);
    av_image_copy_plane(u.data(), int(crow), frame->data[1], frame->linesize[1], int(crow), p.chroma_height);
    av_image_copy_plane(v.data(), int(crow), frame->data[2], frame->linesize[2], int(crow), p.chroma_height);
    p.copied = true;
    return p;
}

void VideoSource::convert(const AVFrame *frame, std::span<uint8_t> out, int64_t n) {
    const int w = info_.width, h = info_.height;
    AVFrame *src = av_frame_clone(frame);
    int cs, range;
    colour(frame, cs, range);
    src->colorspace = static_cast<AVColorSpace>(cs);
    src->color_range = static_cast<AVColorRange>(range);

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
    if (script::kind_of(file) != script::Kind::None) {
        script_ = script::open(file, script::Want::Audio);
        info_ = script_->audio;
        if (info_.channels <= 0 || info_.sample_rate <= 0)
            throw MediaError("unsupported audio format");
        return;
    }
    guarded(file, [&] {
        auto cache = cache_file(file, cache_dir);
        // -1: align to the first video track; -2 would leave the delay alone
        bool has_video = false;
        for (const auto &t : list_tracks(file))
            has_video |= t.type == "video";
        bs_ = std::make_unique<BestAudioSource>(file, track, has_video ? -1 : -2, 0,
                                                cache.empty() ? bcmDisable : bcmAutoAbsolutePath, cache, nullptr, 0,
                                                wrap(progress));
        bs_->SetCancellationCallback([stop = cancel_] { return stop->load(); });
        const auto &ap = bs_->GetAudioProperties();
        // We read each track once, front to back, into our own buffer, so BestSource's frame cache
        // (1 GB by default) only has to hold the frame a read ended in. It is a list searched from
        // its head for every frame: with TrueHD's 40-sample frames a 16 MB one held 40,000 of them
        // and ten minutes took two to decode. Two of the longest frames, and a little for short ones.
        int64_t longest = 0;
        for (int64_t n = 0; n < ap.NumFrames; ++n)
            longest = std::max(longest, bs_->GetFrameInfo(n).Length);
        bs_->SetMaxCacheSize(std::max<size_t>(64u << 10, size_t(2 * longest) * size_t(std::max(1, ap.Channels)) *
                                                              size_t(std::max(1, ap.AF.BytesPerSample)) +
                                                             8192));
        // BestSource's own sum: where the track starts after the first video track does
        delay_ = has_video && ap.SampleRate > 0 ? static_cast<int64_t>(bs_->GetRelativeStartTime(-1) * ap.SampleRate) : 0;
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

bool AudioSource::seeks() const { return !bs_ || !bs_->GetLinearDecodingState(); }

namespace {

// The editor's sample of a frame's channels: their mean as fill_f32 gives them, to 16 bits.
int16_t mono16(float sum, int channels) {
    const float v = std::round(sum / float(channels) * 32767.f);
    return v != v ? int16_t{0} : static_cast<int16_t>(std::clamp(v, -32768.f, 32767.f));
}

template <class T, class F>
void mix(const AVFrame *f, int64_t from, int64_t count, int16_t *out, F to_float) {
    const int ch = f->ch_layout.nb_channels;
    if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(f->format))) {
        for (int64_t i = 0; i < count; ++i) {
            float sum = 0;
            for (int c = 0; c < ch; ++c) {
                T v;
                std::memcpy(&v, f->extended_data[c] + (from + i) * sizeof(T), sizeof(T));
                sum += to_float(v);
            }
            out[i] = mono16(sum, ch);
        }
    } else {
        const uint8_t *p = f->extended_data[0] + from * ch * sizeof(T);
        for (int64_t i = 0; i < count; ++i) {
            float sum = 0;
            for (int c = 0; c < ch; ++c, p += sizeof(T)) {
                T v;
                std::memcpy(&v, p, sizeof(T));
                sum += to_float(v);
            }
            out[i] = mono16(sum, ch);
        }
    }
}

} // namespace

void AudioSource::fill_mono(int64_t start, std::span<int16_t> out) {
    std::ranges::fill(out, int16_t{0});
    // in the track's own samples, as BestSource numbers its frames: without the delay
    int64_t s = start - delay_, count = static_cast<int64_t>(out.size());
    int16_t *o = out.data();
    if (s < 0) {
        const int64_t silent = std::min(count, -s);
        o += silent;
        count -= silent;
        s = 0;
    }
    count = std::min(count, info_.num_samples - delay_ - s);
    if (count <= 0)
        return;
    if (script_) {
        // the script's samples as fill_f32 gives them, a piece at a time
        const int ch = info_.channels;
        while (count > 0) {
            if (cancel_->load())
                throw MediaError("canceled");
            const int64_t take = std::min<int64_t>(count, 16384);
            script_buf_.resize(static_cast<size_t>(take) * ch);
            script_->samples(s, take, script_buf_.data());
            const float *p = script_buf_.data();
            for (int64_t i = 0; i < take; ++i) {
                float sum = 0;
                for (int c = 0; c < ch; ++c)
                    sum += *p++;
                o[i] = mono16(sum, ch);
            }
            o += take;
            s += take;
            count -= take;
        }
        return;
    }
    const auto range = bs_->GetFrameRangeBySamples(s, count);
    if (range.First < 0)
        return;
    int64_t first = range.FirstSamplePos; // of frame n
    for (int64_t n = range.First; n <= range.Last && count > 0; ++n) {
        if (held_n_ != n) {
            held_n_ = -1;
            held_.reset(bs_->GetFrame(n));
            if (!held_)
                throw MediaError("cannot decode audio frame " + std::to_string(n));
            held_n_ = n;
        }
        const AVFrame *f = held_->GetAVFrame();
        const int64_t from = s - first, take = std::min<int64_t>(count, f->nb_samples - from);
        first += f->nb_samples;
        if (from < 0 || take <= 0 || f->ch_layout.nb_channels <= 0)
            continue;
        switch (av_get_packed_sample_fmt(static_cast<AVSampleFormat>(f->format))) {
        case AV_SAMPLE_FMT_U8: mix<uint8_t>(f, from, take, o, [](uint8_t v) { return (int(v) - 128) / 128.f; }); break;
        case AV_SAMPLE_FMT_S16: mix<int16_t>(f, from, take, o, [](int16_t v) { return v / 32768.f; }); break;
        case AV_SAMPLE_FMT_S32:
            mix<int32_t>(f, from, take, o, [](int32_t v) { return static_cast<float>(v / 2147483648.0); });
            break;
        case AV_SAMPLE_FMT_FLT: mix<float>(f, from, take, o, [](float v) { return v; }); break;
        case AV_SAMPLE_FMT_DBL: mix<double>(f, from, take, o, [](double v) { return static_cast<float>(v); }); break;
        default: throw MediaError("unsupported sample size");
        }
        o += take;
        s += take;
        count -= take;
    }
}

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
    if (script_) {
        if (cancel_->load())
            throw MediaError("canceled");
        return script_->samples(s, e - s, out.data() + (s - start) * ch);
    }
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
    boxed_valid_ = false;
}

void SubtitleRenderer::add_font(const std::string &name, std::span<const uint8_t> data) {
    ass_add_font(lib_, name.c_str(), reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
}

void SubtitleRenderer::refresh_fonts() {
    // fonts added after ass_set_fonts are only seen after re-running it
    ass_set_fonts(renderer_, nullptr, default_family_.c_str(), ASS_FONTPROVIDER_AUTODETECT, nullptr, 1);
    boxed_valid_ = false;
}

void SubtitleRenderer::set_script(std::string_view ass) {
    boxed_valid_ = false;
    if (track_)
        ass_free_track(track_);
    std::string copy(ass);  // libass wants a mutable buffer
    track_ = ass_read_memory(lib_, copy.data(), copy.size(), nullptr);
    if (!track_)
        throw MediaError("libass could not parse the script");
}

namespace {

// libass's pictures onto the `width` x `height` pixels at (`left`, `top`) of the frame: straight
// onto opaque RGBA8, or as a premultiplied overlay. True when a pixel changed.
template <bool Premul>
bool paint(const ASS_Image *img, int left, int top, int width, int height, uint8_t *rgba) {
    bool drawn = false;
    for (; img; img = img->next) {
        if (img->w <= 0 || img->h <= 0)
            continue;
        const uint32_t r = img->color >> 24, g = (img->color >> 16) & 0xff, b = (img->color >> 8) & 0xff;
        const uint32_t opacity = 255 - (img->color & 0xff);
        for (int y = 0; y < img->h; ++y) {
            const int dy = img->dst_y + y - top;
            if (dy < 0 || dy >= height)
                continue;
            const uint8_t *src = img->bitmap + static_cast<ptrdiff_t>(y) * img->stride;
            uint8_t *row = rgba + static_cast<size_t>(dy) * width * 4;
            for (int x = 0; x < img->w; ++x) {
                const int dx = img->dst_x + x - left;
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

} // namespace

template <bool Premul>
bool SubtitleRenderer::draw(int64_t ms, int width, int height, int video_w, int video_h, std::span<uint8_t> rgba) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4)
        throw MediaError("subtitle buffer too small");
    if constexpr (Premul)
        std::ranges::fill(rgba, uint8_t{0});
    if (!track_ || width <= 0 || height <= 0)
        return false;
    boxed_valid_ = false;
    ass_set_frame_size(renderer_, width, height);
    ass_set_storage_size(renderer_, video_w > 0 ? video_w : width, video_h > 0 ? video_h : height);
    int changed = 0;
    return paint<Premul>(ass_render_frame(renderer_, track_, ms, &changed), 0, 0, width, height, rgba.data());
}

int SubtitleRenderer::overlay_box(int64_t ms, int width, int height, int video_w, int video_h,
                                  std::span<uint8_t> rgba_premul, std::span<int32_t> box) {
    if (width <= 0 || height <= 0 || box.size() < 4 || rgba_premul.size() < static_cast<size_t>(width) * height * 4)
        throw MediaError("subtitle buffer too small");
    const std::array<int, 4> sizes{width, height, video_w, video_h};
    const bool was = std::exchange(boxed_valid_, track_ != nullptr) && sizes == boxed_;
    boxed_ = sizes;
    if (!track_)
        return 0;
    ass_set_frame_size(renderer_, width, height);
    ass_set_storage_size(renderer_, video_w > 0 ? video_w : width, video_h > 0 ? video_h : height);
    int changed = 0;
    const ASS_Image *images = ass_render_frame(renderer_, track_, ms, &changed);
    if (was && !changed)
        return 2;
    int l = INT_MAX, t = INT_MAX, r = INT_MIN, b = INT_MIN;
    for (const ASS_Image *img = images; img; img = img->next) {
        if (img->w <= 0 || img->h <= 0 || (img->color & 0xff) == 0xff)
            continue;
        l = std::min(l, img->dst_x);
        t = std::min(t, img->dst_y);
        r = std::max(r, img->dst_x + img->w);
        b = std::max(b, img->dst_y + img->h);
    }
    if (l >= r || t >= b)
        return 0;
    // a transparent pixel around: a view that scales the box then fades its edge as the frame's did
    l = std::max(l - 1, 0);
    t = std::max(t - 1, 0);
    r = std::min(r + 1, width);
    b = std::min(b + 1, height);
    if (l >= r || t >= b)
        return 0;
    std::ranges::fill(rgba_premul.first(static_cast<size_t>(r - l) * (b - t) * 4), uint8_t{0});
    if (!paint<true>(images, l, t, r - l, b - t, rgba_premul.data()))
        return 0;
    box[0] = l;
    box[1] = t;
    box[2] = r;
    box[3] = b;
    return 1;
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
    boxed_valid_ = false;
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
