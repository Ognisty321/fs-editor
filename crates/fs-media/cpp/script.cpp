#include "script.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <type_traits>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

// VapourSynth's API headers (LGPL-2.1+): the structs of function pointers VSScript hands out.
#include "vapoursynth/VSScript4.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fsmedia {

namespace {

std::mutex options_m;
ScriptOptions options_;

ScriptOptions options() {
    std::lock_guard g(options_m);
    return options_;
}

std::string utf8(const std::filesystem::path &p) {
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

// --- libraries loaded at run time ---

void *load_library(const std::filesystem::path &p, std::string &why) {
#ifdef _WIN32
    // a library named with its folder finds what it depends on there (libvapoursynth beside vsscript)
    HMODULE h = LoadLibraryExW(p.c_str(), nullptr, p.is_absolute() ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
    if (!h)
        why = "Windows error " + std::to_string(GetLastError());
    return h;
#else
    // global: Python's extension modules look their symbols up in the libpython VSScript loads
    void *h = dlopen(p.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!h) {
        const char *e = dlerror();
        why = e ? e : "it cannot be loaded";
    }
    return h;
#endif
}

template <class F>
F symbol(void *lib, const char *name) {
#ifdef _WIN32
    return reinterpret_cast<F>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return reinterpret_cast<F>(dlsym(lib, name));
#endif
}

// --- what both kinds of clip share ---

/// A pixel format by FFmpeg's name for it: `base` with the bit depth and the byte order.
AVPixelFormat named(const char *base, int bits, bool is_float) {
    std::string n = base;
    if (is_float) {
        if (bits != 32)
            return AV_PIX_FMT_NONE;
        n += "f32";
    } else if (bits > 16 || bits < 8) {
        return AV_PIX_FMT_NONE;
    } else if (bits > 8) {
        n += std::to_string(bits);
    }
    if (bits > 8)
        n += std::endian::native == std::endian::little ? "le" : "be";
    const AVPixelFormat f = av_get_pix_fmt(n.c_str());
    return f != AV_PIX_FMT_NONE && sws_isSupportedInput(f) ? f : AV_PIX_FMT_NONE;
}

/// Planar YUV by how many times the chroma is halved across and down.
const char *yuv_base(int ssw, int ssh) {
    static const struct {
        int w, h;
        const char *name;
    } known[] = {{0, 0, "yuv444p"}, {1, 0, "yuv422p"}, {1, 1, "yuv420p"},
                 {2, 0, "yuv411p"}, {0, 1, "yuv440p"}, {2, 2, "yuv410p"}};
    for (const auto &k : known)
        if (k.w == ssw && k.h == ssh)
            return k.name;
    return nullptr;
}

/// The colour of a frame from its properties. Both libraries name them alike (AviSynth+ took
/// VapourSynth's names) and count as ITU-T H.273 does, which FFmpeg's enums follow.
/// `get(key)` gives an integer property or nothing.
template <class Get>
void tag(AVFrame *f, bool rgb, Get get) {
    f->colorspace = AVCOL_SPC_UNSPECIFIED;
    f->color_range = AVCOL_RANGE_UNSPECIFIED;
    f->color_trc = AVCOL_TRC_UNSPECIFIED;
    f->color_primaries = AVCOL_PRI_UNSPECIFIED;
    f->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    f->sample_aspect_ratio = {0, 1};
    if (auto v = get("_Transfer"); v && *v >= 0 && *v < AVCOL_TRC_NB)
        f->color_trc = static_cast<AVColorTransferCharacteristic>(*v);
    if (auto v = get("_Primaries"); v && *v >= 0 && *v < AVCOL_PRI_NB)
        f->color_primaries = static_cast<AVColorPrimaries>(*v);
    const auto num = get("_SARNum"), den = get("_SARDen");
    if (num && den && *num > 0 && *den > 0 && *num <= INT_MAX && *den <= INT_MAX)
        f->sample_aspect_ratio = {static_cast<int>(*num), static_cast<int>(*den)};
    if (rgb) {
        f->colorspace = AVCOL_SPC_RGB;
        f->color_range = AVCOL_RANGE_JPEG;
        return;
    }
    // 0 is RGB, which a YUV frame cannot be
    if (auto v = get("_Matrix"); v && *v > 0 && *v < AVCOL_SPC_NB)
        f->colorspace = static_cast<AVColorSpace>(*v);
    if (auto v = get("_ColorRange"); v && (*v == 0 || *v == 1))
        f->color_range = *v == 0 ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    if (auto v = get("_ChromaLocation"); v && *v >= 0 && *v <= 5)
        f->chroma_location = static_cast<AVChromaLocation>(*v + 1);
}

/// A frame rate as two ints, 30 when the clip has none.
void set_rate(VideoInfo &i, int64_t num, int64_t den) {
    int n = 30, d = 1;
    if (num > 0 && den > 0)
        av_reduce(&n, &d, num, den, INT_MAX);
    i.fps_num = n;
    i.fps_den = d;
}

/// Constant frame rate timestamps and the duration they make.
void set_cfr(script::Clip &c) {
    const VideoInfo &i = c.video;
    c.timestamps.clear();
    c.timestamps.reserve(static_cast<size_t>(i.num_frames));
    for (int64_t n = 0; n < i.num_frames; ++n)
        // whole milliseconds, cut as a container's timestamps are for the frames of a file
        c.timestamps.push_back(av_rescale_rnd(n, int64_t(1000) * i.fps_den, i.fps_num, AV_ROUND_DOWN));
    c.video.duration_s = double(i.num_frames) * i.fps_den / i.fps_num;
}

// ---------------------------------------------------------------------------
// VapourSynth, through VSScript
// ---------------------------------------------------------------------------

struct Vs {
    const VSSCRIPTAPI *vss = nullptr;
    const VSAPI *api = nullptr;
};

// Loaded once and kept: VSScript starts Python, which cannot be shut down and started again.
std::mutex vs_m;
Vs vs_;

/// Loads `lib` as VSScript; the reason when it cannot be used, "" when it is.
std::string try_vsscript(const std::filesystem::path &lib, Vs &out) {
    std::string why;
    void *h = load_library(lib, why);
    if (!h)
        return utf8(lib) + " cannot be loaded (" + why + ")";
    const auto get = symbol<const VSSCRIPTAPI *(VS_CC *)(int)>(h, "getVSScriptAPI");
    if (!get)
        return utf8(lib) + " is not the VSScript library of VapourSynth R55 or newer";
    const VSSCRIPTAPI *vss = get(VSSCRIPT_API_VERSION);
    if (!vss) {
        std::string msg = utf8(lib) + " could not start";
        const char *e = nullptr;
        if (const auto last = symbol<const char *(VS_CC *)()>(h, "getVSScriptAPILastError"))
            e = last();
        if (e && *e)
            msg += std::string(": ") + e;
        else
            msg += ": Python may not be set up for it (run `vapoursynth config`)";
        return msg;
    }
    const VSAPI *api = vss->getVSAPI(VAPOURSYNTH_API_VERSION);
    if (!api)
        return utf8(lib) + " has no VapourSynth API 4";
    out = {vss, api};
    return {};
}

/// Where a VSScript library may be, most deliberate first. Named with their folder only when
/// the file is there: an old installation leaves VSSCRIPT_PATH and the registry behind.
std::vector<std::filesystem::path> vsscript_candidates() {
    std::vector<std::filesystem::path> c;
    const auto existing = [&](const std::filesystem::path &p) {
        std::error_code ec;
        if (!p.empty() && std::filesystem::is_regular_file(p, ec))
            c.push_back(p);
    };
#ifdef _WIN32
    wchar_t buf[2048];
    if (const DWORD n = GetEnvironmentVariableW(L"VSSCRIPT_PATH", buf, DWORD(std::size(buf))); n > 0 && n < std::size(buf))
        existing(buf);
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        DWORD size = sizeof buf;
        if (RegGetValueW(root, L"SOFTWARE\\VapourSynth", L"VSScriptDLL", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr,
                         buf, &size) == ERROR_SUCCESS)
            existing(buf);
    }
    c.emplace_back("vsscript.dll");
#else
    if (const char *e = std::getenv("VSSCRIPT_PATH"))
        existing(e);
#ifdef __APPLE__
    for (const char *n : {"libvapoursynth-script.0.dylib", "libvapoursynth-script.dylib", "libvsscript.dylib"})
#else
    for (const char *n : {"libvapoursynth-script.so.0", "libvapoursynth-script.so", "libvsscript.so"})
#endif
        c.emplace_back(n);
#endif
    return c;
}

const Vs &vapoursynth(const std::filesystem::path &library = {}) {
    std::lock_guard g(vs_m);
    if (vs_.api)
        return vs_;
    const std::filesystem::path named_lib = library.empty() ? options().vsscript : library;
    if (!named_lib.empty()) {
        if (const std::string e = try_vsscript(named_lib, vs_); !e.empty())
            throw MediaError("VapourSynth cannot be used: " + e);
        return vs_;
    }
    std::string failed; // an installation that is there and does not work says more than "not found"
    for (const auto &c : vsscript_candidates()) {
        const std::string e = try_vsscript(c, vs_);
        if (e.empty())
            return vs_;
        if (c.is_absolute() && failed.empty())
            failed = e;
    }
    if (!failed.empty())
        throw MediaError("VapourSynth cannot be used: " + failed);
    throw MediaError("VapourSynth was not found. Install VapourSynth (with Python) to open .vpy scripts, or name "
                     "its VSScript library in the settings.");
}

class VsClip final : public script::Clip {
public:
    VsClip(const std::filesystem::path &file, script::Want want) : vss_(vapoursynth().vss), vs_(vapoursynth().api) {
        try {
            evaluate(file);
            if (want == script::Want::Video)
                setup_video();
            else if (want == script::Want::Audio)
                setup_audio();
        } catch (...) {
            close();
            throw;
        }
    }
    ~VsClip() override { close(); }

    const AVFrame *frame(int64_t n) override {
        char err[1024] = {};
        const VSFrame *f = vs_->getFrame(static_cast<int>(n), vnode_, err, sizeof err);
        if (!f)
            throw MediaError(std::string("VapourSynth: ") + (err[0] ? err : "the frame could not be made"));
        if (held_)
            vs_->freeFrame(held_);
        held_ = f;
        if (vs_->getFrameWidth(f, 0) != video.width || vs_->getFrameHeight(f, 0) != video.height)
            throw MediaError("VapourSynth: frame " + std::to_string(n) + " has another size than the clip");
        view_->format = pix_;
        view_->width = video.width;
        view_->height = video.height;
        static const int gbr[3] = {1, 2, 0}; // FFmpeg's planar RGB is G, B, R
        for (int p = 0; p < planes_; ++p) {
            const int src = gbr_ ? gbr[p] : p;
            view_->data[p] = const_cast<uint8_t *>(vs_->getReadPtr(f, src));
            view_->linesize[p] = static_cast<int>(vs_->getStride(f, src));
        }
        const VSMap *props = vs_->getFramePropertiesRO(f);
        tag(view_, gbr_, [&](const char *key) -> std::optional<int64_t> {
            int e = 0;
            const int64_t v = vs_->mapGetInt(props, key, 0, &e);
            return e ? std::nullopt : std::optional<int64_t>(v);
        });
        return view_;
    }

    void samples(int64_t start, int64_t count, float *out) override {
        const int ch = audio.channels;
        while (count > 0) {
            const int64_t n = start / VS_AUDIO_FRAME_SAMPLES, from = start % VS_AUDIO_FRAME_SAMPLES;
            if (aheld_n_ != n) {
                aheld_n_ = -1;
                if (aheld_)
                    vs_->freeFrame(aheld_);
                char err[1024] = {};
                aheld_ = vs_->getFrame(static_cast<int>(n), anode_, err, sizeof err);
                if (!aheld_)
                    throw MediaError(std::string("VapourSynth: ") + (err[0] ? err : "the audio could not be made"));
                aheld_n_ = n;
            }
            const int64_t take = std::min<int64_t>(count, vs_->getFrameLength(aheld_) - from);
            if (take <= 0)
                throw MediaError("VapourSynth: an audio frame is shorter than the track says");
            for (int c = 0; c < ch; ++c) {
                const uint8_t *p = vs_->getReadPtr(aheld_, c) + from * sample_bytes_;
                float *o = out + c;
                if (audio.is_float) {
                    for (int64_t i = 0; i < take; ++i, o += ch)
                        std::memcpy(o, p + i * 4, 4);
                } else if (sample_bytes_ == 2) {
                    for (int64_t i = 0; i < take; ++i, o += ch) {
                        int16_t v;
                        std::memcpy(&v, p + i * 2, 2);
                        *o = v / 32768.f;
                    }
                } else { // 24 and 32 bits are in the upper bits of 32
                    for (int64_t i = 0; i < take; ++i, o += ch) {
                        int32_t v;
                        std::memcpy(&v, p + i * 4, 4);
                        *o = static_cast<float>(v / 2147483648.0);
                    }
                }
            }
            out += take * ch;
            start += take;
            count -= take;
        }
    }

private:
    void evaluate(const std::filesystem::path &file) {
        se_ = vss_->createScript(nullptr);
        if (!se_)
            throw MediaError("VapourSynth could not make a script environment");
        // as a script expects when it names its sources beside itself
        vss_->evalSetWorkingDir(se_, 1);
        std::error_code ec;
        const auto abs = std::filesystem::absolute(file, ec);
        if (vss_->evaluateFile(se_, utf8(ec ? file : abs).c_str())) {
            const char *e = vss_->getError(se_);
            throw MediaError(std::string("VapourSynth: ") + (e && *e ? e : "the script failed"));
        }
        // the video is output 0 by convention and the audio another; take the first of each
        for (int i = 0; i < 16 && !(vnode_ && anode_); ++i) {
            VSNode *n = vss_->getOutputNode(se_, i);
            if (!n)
                continue;
            const int type = vs_->getNodeType(n);
            if (type == mtVideo && !vnode_)
                vnode_ = n;
            else if (type == mtAudio && !anode_)
                anode_ = n;
            else
                vs_->freeNode(n);
        }
        has_video = vnode_ != nullptr;
        has_audio = anode_ != nullptr;
        if (!has_video && !has_audio)
            throw MediaError("VapourSynth: the script sets no output (clip.set_output() is missing)");
    }

    /// An integer array the script left in a variable, as the Aegisub builds with VapourSynth
    /// read `__aegi_keyframes` and `__aegi_timecodes`.
    std::vector<int64_t> variable(const char *name) {
        std::vector<int64_t> out;
        VSMap *m = vs_->createMap();
        if (vss_->getVariable(se_, name, m) == 0 && vs_->mapGetType(m, name) == ptInt) {
            const int n = vs_->mapNumElements(m, name);
            int e = 0;
            if (const int64_t *v = vs_->mapGetIntArray(m, name, &e); v && !e && n > 0)
                out.assign(v, v + n);
        }
        vs_->freeMap(m);
        return out;
    }

    void setup_video() {
        if (!vnode_)
            throw MediaError("VapourSynth: the script outputs no video");
        const VSVideoInfo *vi = vs_->getVideoInfo(vnode_);
        if (vi->width <= 0 || vi->height <= 0 || vi->format.colorFamily == cfUndefined)
            throw MediaError("VapourSynth: the clip has no fixed size and format");
        if (vi->numFrames <= 0)
            throw MediaError("VapourSynth: the clip has no frames");
        pix_ = pixel_format(vi->format);
        if (pix_ == AV_PIX_FMT_NONE) {
            // nothing swscale reads (float YUV, half floats, odd depths): 16 bits of the same family,
            // which leaves the matrix for the editor to apply
            const int to = vi->format.colorFamily == cfRGB    ? pfRGB48
                           : vi->format.colorFamily == cfGray ? pfGray16
                                                              : pfYUV444P16;
            VSPlugin *resize = vs_->getPluginByID("com.vapoursynth.resize", vss_->getCore(se_));
            VSMap *args = vs_->createMap();
            vs_->mapSetNode(args, "clip", vnode_, maReplace);
            vs_->mapSetInt(args, "format", to, maReplace);
            VSMap *res = resize ? vs_->invoke(resize, "Bicubic", args) : nullptr;
            vs_->freeMap(args);
            VSNode *converted = nullptr;
            std::string why = "the resize plugin is missing";
            if (res) {
                if (const char *e = vs_->mapGetError(res))
                    why = e;
                else
                    converted = vs_->mapGetNode(res, "clip", 0, nullptr);
                vs_->freeMap(res);
            }
            if (!converted) {
                char name[32] = "?";
                vs_->getVideoFormatName(&vi->format, name);
                throw MediaError(std::string("VapourSynth: the clip's format ") + name + " cannot be shown (" + why + ")");
            }
            vs_->freeNode(vnode_);
            vnode_ = converted;
            vi = vs_->getVideoInfo(vnode_);
            pix_ = pixel_format(vi->format);
            if (pix_ == AV_PIX_FMT_NONE)
                throw MediaError("VapourSynth: the clip's format cannot be shown");
        }
        planes_ = vi->format.numPlanes;
        video.width = vi->width;
        video.height = vi->height;
        video.num_frames = vi->numFrames;
        view_ = av_frame_alloc();

        int64_t num = vi->fpsNum, den = vi->fpsDen;
        if (num <= 0 || den <= 0) {
            // no rate of its own (variable): the first frame's duration stands for all of them
            frame(0);
            const VSMap *props = vs_->getFramePropertiesRO(held_);
            int e1 = 0, e2 = 0;
            den = vs_->mapGetInt(props, "_DurationNum", 0, &e1);
            num = vs_->mapGetInt(props, "_DurationDen", 0, &e2);
            if (e1 || e2)
                num = den = 0;
        }
        set_rate(video, num, den);
        set_cfr(*this);
        // what a script for Aegisub says of its clip: times in ms of every frame, and the keyframes
        if (auto tc = variable("__aegi_timecodes");
            tc.size() == static_cast<size_t>(video.num_frames) && std::ranges::is_sorted(tc)) {
            timestamps = std::move(tc);
            const int64_t last = timestamps.size() > 1 ? timestamps.back() - timestamps[timestamps.size() - 2]
                                                       : int64_t(1000) * video.fps_den / video.fps_num;
            video.duration_s = double(timestamps.back() - timestamps.front() + last) / 1000.0;
        }
        keyframes = variable("__aegi_keyframes");
        std::erase_if(keyframes, [&](int64_t k) { return k < 0 || k >= video.num_frames; });
        std::ranges::sort(keyframes);
        keyframes.erase(std::ranges::unique(keyframes).begin(), keyframes.end());
    }

    void setup_audio() {
        if (!anode_)
            throw MediaError("VapourSynth: the script outputs no audio");
        const VSAudioInfo *ai = vs_->getAudioInfo(anode_);
        audio.sample_rate = ai->sampleRate;
        audio.channels = ai->format.numChannels;
        audio.num_samples = ai->numSamples;
        audio.is_float = ai->format.sampleType == stFloat;
        audio.bits = ai->format.bitsPerSample;
        sample_bytes_ = ai->format.bytesPerSample;
        if ((sample_bytes_ != 2 && sample_bytes_ != 4) || (audio.is_float && sample_bytes_ != 4))
            throw MediaError("VapourSynth: unsupported audio sample format");
    }

    AVPixelFormat pixel_format(const VSVideoFormat &f) {
        const bool is_float = f.sampleType == stFloat;
        gbr_ = false;
        switch (f.colorFamily) {
        case cfGray: return named("gray", f.bitsPerSample, is_float);
        case cfRGB:
            gbr_ = true;
            return named("gbrp", f.bitsPerSample, is_float);
        case cfYUV:
            if (const char *base = is_float ? nullptr : yuv_base(f.subSamplingW, f.subSamplingH))
                return named(base, f.bitsPerSample, false);
            return AV_PIX_FMT_NONE;
        default: return AV_PIX_FMT_NONE;
        }
    }

    void close() {
        if (held_)
            vs_->freeFrame(held_);
        if (aheld_)
            vs_->freeFrame(aheld_);
        if (vnode_)
            vs_->freeNode(vnode_);
        if (anode_)
            vs_->freeNode(anode_);
        if (se_)
            vss_->freeScript(se_);
        av_frame_free(&view_);
        held_ = aheld_ = nullptr;
        vnode_ = anode_ = nullptr;
        se_ = nullptr;
    }

    const VSSCRIPTAPI *vss_;
    const VSAPI *vs_;
    VSScript *se_ = nullptr;
    VSNode *vnode_ = nullptr;
    VSNode *anode_ = nullptr;
    const VSFrame *held_ = nullptr; // the frame view_ points into
    AVFrame *view_ = nullptr;
    AVPixelFormat pix_ = AV_PIX_FMT_NONE;
    bool gbr_ = false;
    int planes_ = 0;
    const VSFrame *aheld_ = nullptr; // the audio frame read last: the next read starts in it
    int64_t aheld_n_ = -1;
    int sample_bytes_ = 0;
};

// ---------------------------------------------------------------------------
// Avisynth, through its C interface (avisynth_c.h), every function found by name
// ---------------------------------------------------------------------------

#ifdef _WIN32
#define FS_AVSC_CC __stdcall
#else
#define FS_AVSC_CC
#endif

struct AVS_ScriptEnvironment;
struct AVS_Clip;
struct AVS_Map;

struct AVS_VideoInfo {
    int width, height; // width 0: no video
    unsigned fps_numerator, fps_denominator;
    int num_frames;
    int pixel_type;
    int audio_samples_per_second; // 0: no audio
    int sample_type;
    int64_t num_audio_samples;
    int nchannels;
    int image_type;
};

// What every version's frame starts with; read directly only from an Avisynth older than 2.6,
// which has no functions for it.
struct AVS_VideoFrameBuffer {
    uint8_t *data;
};
struct AVS_VideoFrame {
    volatile long refcount;
    AVS_VideoFrameBuffer *vfb;
    int offset;
    int pitch, row_size, height;
};

struct AVS_Value {
    short type; // 'v' void, 'c' clip, 'b', 'i', 'f', 's', 'a' array, 'e' error
    short array_size;
    union {
        void *clip;
        char boolean;
        int integer;
        float floating_pt;
        const char *string;
        const AVS_Value *array;
        void *function;
        int64_t longlong;
        double double_pt;
    } d;
};

// AVS_VideoInfo::pixel_type
constexpr uint32_t kCsYuva = 1u << 27, kCsBgr = 1u << 28, kCsYuv = 1u << 29, kCsInterleaved = 1u << 30,
                   kCsPlanar = 1u << 31;
constexpr uint32_t kCsBgr24 = 1u | kCsBgr | kCsInterleaved, kCsBgr32 = 2u | kCsBgr | kCsInterleaved,
                   kCsYuy2 = 4u | kCsYuv | kCsInterleaved;
constexpr uint32_t kCsBits16 = 1u << 16;
// planes of a frame
constexpr int kPlaneY = 1, kPlaneU = 2, kPlaneV = 4, kPlaneR = 32, kPlaneG = 64, kPlaneB = 128;
constexpr int kSampleInt16 = 2;

struct Avs {
    AVS_ScriptEnvironment *(FS_AVSC_CC *create_script_environment)(int version) = nullptr;
    void (FS_AVSC_CC *delete_script_environment)(AVS_ScriptEnvironment *) = nullptr;
    int (FS_AVSC_CC *check_version)(AVS_ScriptEnvironment *, int version) = nullptr;
    int (FS_AVSC_CC *function_exists)(AVS_ScriptEnvironment *, const char *name) = nullptr;
    AVS_Value (FS_AVSC_CC *invoke)(AVS_ScriptEnvironment *, const char *name, AVS_Value args,
                                   const char **arg_names) = nullptr;
    int (FS_AVSC_CC *set_memory_max)(AVS_ScriptEnvironment *, int mb) = nullptr;
    AVS_Clip *(FS_AVSC_CC *take_clip)(AVS_Value, AVS_ScriptEnvironment *) = nullptr;
    void (FS_AVSC_CC *release_clip)(AVS_Clip *) = nullptr;
    void (FS_AVSC_CC *release_value)(AVS_Value) = nullptr;
    const AVS_VideoInfo *(FS_AVSC_CC *get_video_info)(AVS_Clip *) = nullptr;
    int (FS_AVSC_CC *get_version)(AVS_Clip *) = nullptr;
    const char *(FS_AVSC_CC *clip_get_error)(AVS_Clip *) = nullptr;
    AVS_VideoFrame *(FS_AVSC_CC *get_frame)(AVS_Clip *, int n) = nullptr;
    int (FS_AVSC_CC *get_audio)(AVS_Clip *, void *buf, int64_t start, int64_t count) = nullptr;
    void (FS_AVSC_CC *release_video_frame)(AVS_VideoFrame *) = nullptr;
    // Avisynth 2.6 and AviSynth+
    const char *(FS_AVSC_CC *get_error)(AVS_ScriptEnvironment *) = nullptr;
    int (FS_AVSC_CC *get_pitch_p)(const AVS_VideoFrame *, int plane) = nullptr;
    const uint8_t *(FS_AVSC_CC *get_read_ptr_p)(const AVS_VideoFrame *, int plane) = nullptr;
    // frame properties, AviSynth+ 3.6
    const AVS_Map *(FS_AVSC_CC *get_frame_props_ro)(AVS_ScriptEnvironment *, const AVS_VideoFrame *) = nullptr;
    int64_t (FS_AVSC_CC *prop_get_int)(AVS_ScriptEnvironment *, const AVS_Map *, const char *key, int index,
                                       int *error) = nullptr;
    char (FS_AVSC_CC *prop_get_type)(AVS_ScriptEnvironment *, const AVS_Map *, const char *key) = nullptr;

    bool loaded = false;
    /// 2.6 or AviSynth+: errors are told and a frame's planes are asked for.
    bool modern() const { return get_error && get_pitch_p && get_read_ptr_p; }
};

// One call into Avisynth at a time, as Aegisub has it; the library stays loaded.
std::mutex avs_m;
Avs avs_;

/// The library, loaded at the first call. `avs_m` is held.
const Avs &avisynth() {
    if (avs_.loaded)
        return avs_;
    void *h = nullptr;
#ifdef _WIN32
    // where Aegisub looks: beside the program and in the system folder, where its installer puts it
    h = LoadLibraryExW(L"avisynth.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h)
        throw MediaError("Avisynth was not found: avisynth.dll is neither in the program's folder nor in the Windows "
                         "system folder. Install AviSynth+ (64-bit) to open .avs scripts.");
#else
#ifdef __APPLE__
    const std::vector<std::string> names = {"libavisynth.dylib"};
#else
    std::vector<std::string> names = {"libavisynth.so"};
    for (int v = 14; v >= 8; --v) // without the development link: by the interface version in the name
        names.push_back("libavisynth.so." + std::to_string(v));
#endif
    std::string why;
    for (size_t i = 0; i < names.size() && !h; ++i)
        h = load_library(names[i], why);
    if (!h)
        throw MediaError("Avisynth was not found: libavisynth is not on the library path. Install AviSynth+ to open "
                         ".avs scripts.");
#endif
    Avs a;
    bool whole = true;
    const auto need = [&](auto &f, const char *name) {
        f = symbol<std::remove_reference_t<decltype(f)>>(h, name);
        whole = whole && f;
    };
    const auto may = [&](auto &f, const char *name) { f = symbol<std::remove_reference_t<decltype(f)>>(h, name); };
    need(a.create_script_environment, "avs_create_script_environment");
    need(a.delete_script_environment, "avs_delete_script_environment");
    need(a.check_version, "avs_check_version");
    need(a.function_exists, "avs_function_exists");
    need(a.invoke, "avs_invoke");
    need(a.set_memory_max, "avs_set_memory_max");
    need(a.take_clip, "avs_take_clip");
    need(a.release_clip, "avs_release_clip");
    need(a.release_value, "avs_release_value");
    need(a.get_video_info, "avs_get_video_info");
    need(a.get_version, "avs_get_version");
    need(a.clip_get_error, "avs_clip_get_error");
    need(a.get_frame, "avs_get_frame");
    need(a.get_audio, "avs_get_audio");
    need(a.release_video_frame, "avs_release_video_frame");
    may(a.get_error, "avs_get_error");
    may(a.get_pitch_p, "avs_get_pitch_p");
    may(a.get_read_ptr_p, "avs_get_read_ptr_p");
    may(a.get_frame_props_ro, "avs_get_frame_props_ro");
    may(a.prop_get_int, "avs_prop_get_int");
    may(a.prop_get_type, "avs_prop_get_type");
    if (!whole)
        throw MediaError("The Avisynth library found has no C interface (avs_invoke and the like). Install "
                         "AviSynth+ to open .avs scripts.");
    a.loaded = true;
    avs_ = a;
    return avs_;
}

AVS_Value avs_value(char type) {
    AVS_Value v;
    std::memset(&v, 0, sizeof v);
    v.type = type;
    return v;
}

class AvsClip final : public script::Clip {
public:
    AvsClip(const std::filesystem::path &file, script::Want want) {
        std::lock_guard g(avs_m);
        a_ = &avisynth();
        try {
            evaluate(file);
            if (want == script::Want::Video)
                setup_video();
            else if (want == script::Want::Audio)
                setup_audio();
        } catch (...) {
            close();
            throw;
        }
    }
    ~AvsClip() override {
        std::lock_guard g(avs_m);
        close();
    }

    void set_matrix(Matrix m, bool full_range, bool override_range) override {
        std::lock_guard g(avs_m);
        matrix_ = m;
        full_ = override_range && full_range;
    }

    const AVFrame *frame(int64_t n) override {
        std::lock_guard g(avs_m);
        AVS_Clip *from = clip_;
        if (by_avisynth_) {
            const std::string m = source_rgb_ ? std::string() : rgb_matrix();
            if (!rgb_clip_ || m != rgb_matrix_)
                convert_to_rgb(m);
            from = rgb_clip_;
        }
        AVS_VideoFrame *f = a_->get_frame(from, static_cast<int>(n));
        const char *e = a_->clip_get_error(from);
        if (e || !f) {
            const std::string msg = e ? e : "the frame could not be made";
            if (f)
                a_->release_video_frame(f);
            throw MediaError("Avisynth: " + msg);
        }
        if (held_)
            a_->release_video_frame(held_);
        held_ = f;
        view_->format = pix_;
        view_->width = video.width;
        view_->height = video.height;
        for (int p = 0; p < nplanes_; ++p) {
            const uint8_t *data;
            int pitch;
            if (a_->modern()) {
                data = a_->get_read_ptr_p(f, planes_[p]);
                pitch = a_->get_pitch_p(f, planes_[p]);
            } else { // RGB32 from an Avisynth before 2.6
                data = f->vfb->data + f->offset;
                pitch = f->pitch;
            }
            if (!data || pitch <= 0)
                throw MediaError("Avisynth: frame " + std::to_string(n) + " has no picture");
            if (flipped_) { // Avisynth's packed RGB is bottom-up
                data += static_cast<ptrdiff_t>(video.height - 1) * pitch;
                pitch = -pitch;
            }
            view_->data[p] = const_cast<uint8_t *>(data);
            view_->linesize[p] = pitch;
        }
        // properties are whole from interface 9 on (FFmpeg's avisynth demuxer reads them from there too)
        const AVS_Map *props = a_->get_frame_props_ro && a_->prop_get_int && a_->prop_get_type && version_ >= 9
                                   ? a_->get_frame_props_ro(env_, f)
                                   : nullptr;
        tag(view_, rgb_, [&](const char *key) -> std::optional<int64_t> {
            if (!props || a_->prop_get_type(env_, props, key) != 'i')
                return std::nullopt;
            int err = 0;
            const int64_t v = a_->prop_get_int(env_, props, key, 0, &err);
            return err ? std::nullopt : std::optional<int64_t>(v);
        });
        return view_;
    }

    void samples(int64_t start, int64_t count, float *out) override {
        std::lock_guard g(avs_m);
        const size_t n = static_cast<size_t>(count) * static_cast<size_t>(audio.channels);
        abuf_.resize(n);
        a_->get_audio(aclip_, abuf_.data(), start, count);
        if (const char *e = a_->clip_get_error(aclip_))
            throw MediaError(std::string("Avisynth: ") + e);
        for (size_t i = 0; i < n; ++i)
            out[i] = abuf_[i] / 32768.f;
    }

private:
    /// A function of the script language; an error it returns is thrown.
    AVS_Value invoke(const char *name, AVS_Value args, const char **names = nullptr) {
        AVS_Value v = a_->invoke(env_, name, args, names);
        if (v.type == 'e') {
            const std::string msg = v.d.string ? v.d.string : name;
            throw MediaError("Avisynth: " + msg);
        }
        if (v.type != 'c') {
            a_->release_value(v);
            throw MediaError(std::string("Avisynth: ") + name + " did not return a clip");
        }
        return v;
    }

    /// `script_` becomes what `name(script_, more...)` returns.
    void apply(const char *name, std::initializer_list<AVS_Value> more = {}) {
        std::vector<AVS_Value> args{script_};
        args.insert(args.end(), more);
        AVS_Value all = avs_value('a');
        all.d.array = args.data();
        all.array_size = static_cast<short>(args.size());
        const AVS_Value v = invoke(name, all);
        a_->release_value(script_);
        script_ = v;
    }

    void evaluate(const std::filesystem::path &file) {
        const ScriptOptions o = options();
        if (!a_->modern() && !o.avs_allow_ancient)
            throw MediaError("This Avisynth is older than 2.6. Install AviSynth+, or allow old versions in the "
                             "settings.");
        env_ = a_->create_script_environment(a_->modern() ? 6 : 2);
        if (!env_)
            throw MediaError("Avisynth could not make a script environment");
        if (a_->get_error)
            if (const char *e = a_->get_error(env_))
                throw MediaError(std::string("Avisynth: ") + e);
        if (o.avs_memory_max_mb > 0)
            a_->set_memory_max(env_, o.avs_memory_max_mb);

        std::error_code ec;
        const auto abs = std::filesystem::absolute(file, ec);
        const std::filesystem::path &path = ec ? file : abs;
        AVS_Value name = avs_value('s');
#ifdef _WIN32
        std::string text;
        if (a_->modern() && a_->check_version(env_, 7) == 0) {
            // AviSynth+ takes the name as UTF-8 when told so
            text = utf8(path);
            name.d.string = text.c_str();
            AVS_Value args[2] = {name, avs_value('b')};
            args[1].d.boolean = 1;
            AVS_Value both = avs_value('a');
            both.d.array = args;
            both.array_size = 2;
            script_ = invoke("Import", both);
        } else {
            // older ones in the system's code page: the short name, as Aegisub gives it, has no
            // letters outside it
            std::wstring w = path.wstring();
            wchar_t shortened[1024];
            if (const DWORD n = GetShortPathNameW(w.c_str(), shortened, DWORD(std::size(shortened)));
                n > 0 && n < std::size(shortened))
                w = shortened;
            text.resize(w.size() * 4 + 1);
            const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, text.data(), int(text.size()), nullptr, nullptr);
            if (n <= 0)
                throw MediaError("the script's name cannot be given to Avisynth");
            name.d.string = text.c_str();
            script_ = invoke("Import", name);
        }
#else
        const std::string text = path.string();
        name.d.string = text.c_str();
        script_ = invoke("Import", name);
#endif
        clip_ = a_->take_clip(script_, env_);
        if (!clip_)
            throw MediaError("Avisynth: the script returns no clip");
        version_ = a_->get_version(clip_);
        vi_ = *a_->get_video_info(clip_);
        has_video = vi_.width != 0;
        has_audio = vi_.audio_samples_per_second != 0;
        if (!has_video && !has_audio)
            throw MediaError("Avisynth: the script's clip has neither video nor audio");
    }

    /// The format of `vi_` for swscale, and which planes it is made of.
    void map_format() {
        const uint32_t t = static_cast<uint32_t>(vi_.pixel_type);
        static const int depth[8] = {8, 16, 32, 0, 0, 10, 12, 14};
        const int bits = depth[(t >> 16) & 7];
        pix_ = AV_PIX_FMT_NONE;
        flipped_ = rgb_ = false;
        nplanes_ = 1;
        planes_[0] = 0;
        if (t == kCsBgr24 || t == kCsBgr32 || t == (kCsBgr24 | kCsBits16)) {
            // BGR32's fourth byte is no picture: read as padding, or the frame would come out transparent
            pix_ = t == kCsBgr24   ? AV_PIX_FMT_BGR24
                   : t == kCsBgr32 ? AV_PIX_FMT_BGR0
                                   : av_get_pix_fmt(std::endian::native == std::endian::little ? "bgr48le" : "bgr48be");
            flipped_ = rgb_ = true;
        } else if (t == kCsYuy2) {
            pix_ = AV_PIX_FMT_YUYV422;
        } else if (!(t & kCsPlanar) || bits == 0) {
            // RAW32, 64-bit RGB with its alpha, or something newer: Avisynth converts it
        } else if (t & kCsBgr) { // planar RGB, with or without alpha
            pix_ = named("gbrp", bits, bits == 32);
            rgb_ = true;
            nplanes_ = 3;
            planes_[0] = kPlaneG, planes_[1] = kPlaneB, planes_[2] = kPlaneR;
        } else if (t & kCsInterleaved) { // one plane: Y, or Y with alpha
            pix_ = named("gray", bits, bits == 32);
            planes_[0] = kPlaneY;
        } else if ((t & (kCsYuv | kCsYuva)) && bits != 32) {
            // chroma width: 3 full, 0 half, 1 a quarter; the height's the same from bit 8
            static const int halved[8] = {1, 2, -1, 0, -1, -1, -1, -1};
            const char *base = yuv_base(halved[t & 7], halved[(t >> 8) & 7]);
            pix_ = base ? named(base, bits, false) : AV_PIX_FMT_NONE;
            nplanes_ = 3;
            planes_[0] = kPlaneY, planes_[1] = kPlaneU, planes_[2] = kPlaneV;
        }
    }

    void setup_video() {
        if (!has_video)
            throw MediaError("Avisynth: the script has no video");
        if (vi_.num_frames <= 0)
            throw MediaError("Avisynth: the clip has no frames");
        const uint32_t t = static_cast<uint32_t>(vi_.pixel_type);
        source_rgb_ = (t & kCsBgr) != 0;
        if (a_->modern()) {
            map_format();
            if (pix_ == AV_PIX_FMT_NONE && (t & (kCsYuv | kCsYuva)) && a_->function_exists(env_, "ConvertBits")) {
                // float YUV: 16 bits of it keep the matrix for the editor to apply
                AVS_Value sixteen = avs_value('i');
                sixteen.d.integer = 16;
                apply("ConvertBits", {sixteen});
                a_->release_clip(clip_);
                clip_ = a_->take_clip(script_, env_);
                vi_ = *a_->get_video_info(clip_);
                map_format();
            }
        }
        if (pix_ == AV_PIX_FMT_NONE) {
            // Aegisub's way for everything: ConvertToRGB32 by Avisynth, with the matrix the editor
            // asks for, made at the first frame and again when that changes
            by_avisynth_ = true;
            pix_ = AV_PIX_FMT_BGR0;
            flipped_ = rgb_ = true;
            nplanes_ = 1;
            planes_[0] = 0;
            if (!source_rgb_) { // untagged YUV to the editor, which guesses its matrix by size as Aegisub does
                source_colorspace = AVCOL_SPC_UNSPECIFIED;
                source_range = AVCOL_RANGE_MPEG;
            }
        }
        video.width = vi_.width;
        video.height = vi_.height;
        video.num_frames = vi_.num_frames;
        set_rate(video, vi_.fps_numerator, vi_.fps_denominator);
        set_cfr(*this);
        view_ = av_frame_alloc();
    }

    /// Avisynth's name of the matrix the editor decodes with; without one, the guess by size
    /// that Aegisub calls the clip's real colour space.
    std::string rgb_matrix() const {
        const bool hd = vi_.width > 1024 || vi_.height >= 600;
        const bool bt709 = matrix_ == Matrix::Auto ? hd : matrix_ != Matrix::Bt601;
        return full_ ? (bt709 ? "PC.709" : "PC.601") : (bt709 ? "Rec709" : "Rec601");
    }

    void convert_to_rgb(const std::string &matrix) {
        AVS_Value v;
        if (matrix.empty()) {
            v = invoke("ConvertToRGB32", script_);
        } else {
            AVS_Value args[2] = {script_, avs_value('s')};
            args[1].d.string = matrix.c_str();
            const char *names[2] = {nullptr, "matrix"};
            AVS_Value both = avs_value('a');
            both.d.array = args;
            both.array_size = 2;
            v = invoke("ConvertToRGB32", both, names);
        }
        AVS_Clip *c = a_->take_clip(v, env_);
        a_->release_value(v);
        if (!c)
            throw MediaError("Avisynth: ConvertToRGB32 returned no clip");
        if (held_)
            a_->release_video_frame(held_);
        held_ = nullptr;
        if (rgb_clip_)
            a_->release_clip(rgb_clip_);
        rgb_clip_ = c;
        rgb_matrix_ = matrix;
    }

    /// As Aegisub's Avisynth audio provider: the down-mixer, 16 bits, then the sample rate.
    void setup_audio() {
        if (!has_audio)
            throw MediaError("Avisynth: the script has no audio");
        const ScriptOptions o = options();
        if (!o.avs_downmixer.empty())
            apply(o.avs_downmixer.c_str());
        apply("ConvertAudioTo16bit");
        aclip_ = a_->take_clip(script_, env_);
        int rate = o.avs_sample_rate;
        if (rate == 0 && a_->get_video_info(aclip_)->audio_samples_per_second < 32000)
            rate = 44100;
        if (rate != 0) {
            AVS_Value to = avs_value('i');
            to.d.integer = rate;
            apply("ResampleAudio", {to});
            a_->release_clip(aclip_);
            aclip_ = a_->take_clip(script_, env_);
        }
        const AVS_VideoInfo *vi = a_->get_video_info(aclip_);
        if (vi->sample_type != kSampleInt16 || vi->nchannels <= 0)
            throw MediaError("Avisynth: unsupported audio sample format");
        audio.sample_rate = vi->audio_samples_per_second;
        audio.channels = vi->nchannels;
        audio.num_samples = vi->num_audio_samples;
        audio.is_float = false;
        audio.bits = 16;
    }

    void close() {
        if (held_)
            a_->release_video_frame(held_);
        for (AVS_Clip *c : {rgb_clip_, aclip_, clip_})
            if (c)
                a_->release_clip(c);
        if (script_.type == 'c')
            a_->release_value(script_);
        if (env_)
            a_->delete_script_environment(env_);
        av_frame_free(&view_);
        held_ = nullptr;
        rgb_clip_ = aclip_ = clip_ = nullptr;
        script_ = avs_value('v');
        env_ = nullptr;
    }

    const Avs *a_ = nullptr;
    AVS_ScriptEnvironment *env_ = nullptr;
    AVS_Value script_ = avs_value('v'); // the script's clip, for the functions applied to it
    AVS_Clip *clip_ = nullptr;
    AVS_Clip *rgb_clip_ = nullptr; // clip_ as RGB32, when Avisynth converts
    AVS_Clip *aclip_ = nullptr;
    AVS_VideoInfo vi_{};
    int version_ = 0;
    AVS_VideoFrame *held_ = nullptr; // the frame view_ points into
    AVFrame *view_ = nullptr;
    AVPixelFormat pix_ = AV_PIX_FMT_NONE;
    int planes_[3] = {0, 0, 0};
    int nplanes_ = 1;
    bool flipped_ = false;
    bool rgb_ = false;
    bool source_rgb_ = false;
    bool by_avisynth_ = false;
    std::string rgb_matrix_;
    Matrix matrix_ = Matrix::Auto;
    bool full_ = false;
    std::vector<int16_t> abuf_;
};

// --- which tracks a script has, from the last time it was evaluated ---

struct Seen {
    std::filesystem::file_time_type changed;
    bool video = false;
    bool audio = false;
};
std::mutex seen_m;
std::map<std::filesystem::path, Seen> seen_;

std::filesystem::path key_of(const std::filesystem::path &file) {
    std::error_code ec;
    auto abs = std::filesystem::absolute(file, ec);
    return ec ? file : abs.lexically_normal();
}

/// What the last evaluation of the file as it is now found, if there was one.
std::optional<Seen> seen(const std::filesystem::path &file) {
    std::error_code ec;
    const auto changed = std::filesystem::last_write_time(file, ec);
    if (ec)
        return std::nullopt;
    std::lock_guard g(seen_m);
    if (const auto it = seen_.find(key_of(file)); it != seen_.end() && it->second.changed == changed)
        return it->second;
    return std::nullopt;
}

}  // namespace

void set_script_options(const ScriptOptions &o) {
    std::lock_guard g(options_m);
    options_ = o;
}

void load_vapoursynth(const std::filesystem::path &library) { vapoursynth(library); }

void load_avisynth() {
    std::lock_guard g(avs_m);
    avisynth();
}

bool script_evaluated(const std::filesystem::path &file) { return script::evaluated(file); }

namespace script {

Kind kind_of(const std::filesystem::path &file) {
    std::string ext = file.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; });
    return ext == ".avs" ? Kind::Avisynth : ext == ".vpy" ? Kind::VapourSynth : Kind::None;
}

std::unique_ptr<Clip> open(const std::filesystem::path &file, Want want) {
    std::error_code ec;
    const auto changed = std::filesystem::last_write_time(file, ec);
    if (ec)
        throw MediaError("the script cannot be read");
    std::unique_ptr<Clip> c;
    switch (kind_of(file)) {
    case Kind::Avisynth: c = std::make_unique<AvsClip>(file, want); break;
    case Kind::VapourSynth: c = std::make_unique<VsClip>(file, want); break;
    case Kind::None: throw MediaError("not an Avisynth or VapourSynth script");
    }
    std::lock_guard g(seen_m);
    seen_[key_of(file)] = Seen{changed, c->has_video, c->has_audio};
    return c;
}

bool evaluated(const std::filesystem::path &file) { return seen(file).has_value(); }

std::vector<Track> tracks(const std::filesystem::path &file) {
    std::error_code ec;
    const auto changed = std::filesystem::last_write_time(file, ec);
    std::optional<Seen> s = seen(file);
    if (!s) {
        const auto c = open(file, Want::Tracks);
        s = Seen{changed, c->has_video, c->has_audio};
    }
    const char *codec = kind_of(file) == Kind::Avisynth ? "avisynth" : "vapoursynth";
    std::vector<Track> out;
    if (s->video)
        out.push_back(Track{.index = int(out.size()), .type = "video", .codec = codec, .is_default = true});
    if (s->audio)
        out.push_back(Track{.index = int(out.size()), .type = "audio", .codec = codec, .is_default = true});
    return out;
}

}  // namespace script

}  // namespace fsmedia
