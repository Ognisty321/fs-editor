#include "fs-media/cpp/bridge.h"

#include "fs-media/src/ffi.rs.h"

namespace fsmedia::ffi {

namespace {

std::filesystem::path to_path(rust::Str s) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

rust::String str(const std::string &s) { return rust::String::lossy(s.data(), s.size()); }

Progress progress_of(ProgressSink &sink) {
    return [&sink](int64_t done, int64_t total) { return sink.report(done, total); };
}

}  // namespace

rust::Vec<TrackInfo> list_tracks(rust::Str path) {
    rust::Vec<TrackInfo> out;
    for (const auto &t : fsmedia::list_tracks(to_path(path)))
        out.push_back(TrackInfo{t.index, str(t.type), str(t.codec), str(t.language), str(t.title), t.is_default});
    return out;
}

rust::Vec<FontFile> attached_fonts(rust::Str path) {
    rust::Vec<FontFile> out;
    for (const auto &[name, data] : fsmedia::attached_fonts(to_path(path))) {
        rust::Vec<uint8_t> bytes;
        bytes.reserve(data.size());
        for (uint8_t b : data)
            bytes.push_back(b);
        out.push_back(FontFile{str(name), std::move(bytes)});
    }
    return out;
}

rust::Vec<Chapter> chapters(rust::Str path) {
    rust::Vec<Chapter> out;
    for (const auto &[ms, title] : fsmedia::chapters(to_path(path)))
        out.push_back(Chapter{ms, str(title)});
    return out;
}

rust::String subtitle_packets(rust::Str path, int32_t index, rust::Vec<SubPacket> &out) {
    std::vector<fsmedia::SubPacket> v;
    const std::string header = fsmedia::subtitle_packets(to_path(path), index, v);
    for (const auto &p : v)
        out.push_back(SubPacket{p.start_ms, p.duration_ms, str(p.text)});
    return str(header);
}

std::unique_ptr<VideoSource> open_video(rust::Str path, int32_t track, rust::Str cache_dir, ProgressSink &progress) {
    return std::make_unique<VideoSource>(to_path(path), track, to_path(cache_dir), progress_of(progress));
}

VideoProps video_props(const VideoSource &v) {
    const auto &i = v.info();
    return VideoProps{i.width,   i.height,  i.num_frames,      i.fps_num,         i.fps_den,
                      i.sar_num, i.sar_den, i.duration_s, str(i.pixel_format), str(i.matrix)};
}

rust::Vec<int64_t> video_timestamps(const VideoSource &v) {
    rust::Vec<int64_t> out;
    out.reserve(v.timestamps_ms().size());
    for (auto t : v.timestamps_ms())
        out.push_back(t);
    return out;
}

rust::Vec<int64_t> video_keyframes(const VideoSource &v) {
    rust::Vec<int64_t> out;
    for (auto k : v.keyframes())
        out.push_back(k);
    return out;
}

bool video_linear(const VideoSource &v) { return v.linear_decoding(); }

bool video_exact(const VideoSource &v) { return v.exact(); }

void video_set_matrix(VideoSource &v, uint8_t matrix, uint8_t range) {
    // range: 0 = as decoded, 1 = force TV (limited), 2 = force PC (full)
    v.set_matrix(static_cast<Matrix>(matrix), range == 2, range != 0);
}

void video_frame_rgba(VideoSource &v, int64_t n, rust::Slice<uint8_t> out) {
    v.frame_rgba(n, std::span<uint8_t>(out.data(), out.size()));
}

std::unique_ptr<AudioSource> open_audio(rust::Str path, int32_t track, rust::Str cache_dir, ProgressSink &progress) {
    return std::make_unique<AudioSource>(to_path(path), track, to_path(cache_dir), progress_of(progress));
}

AudioProps audio_props(const AudioSource &a) {
    const auto &i = a.info();
    return AudioProps{i.sample_rate, i.channels, i.num_samples, i.is_float, i.bits, i.start_time_s};
}

void audio_fill(AudioSource &a, int64_t start, rust::Slice<float> out) {
    a.fill_f32(start, std::span<float>(out.data(), out.size()));
}

std::unique_ptr<SubtitleRenderer> new_subtitle_renderer() { return std::make_unique<SubtitleRenderer>(); }

void subs_set_fonts(SubtitleRenderer &r, rust::Str family, rust::Str dir) {
    r.set_fonts(std::string(family), to_path(dir));
}

void subs_add_font(SubtitleRenderer &r, rust::Str name, rust::Slice<const uint8_t> data) {
    r.add_font(std::string(name), std::span<const uint8_t>(data.data(), data.size()));
}

void subs_set_script(SubtitleRenderer &r, rust::Str ass) { r.set_script(std::string_view(ass.data(), ass.size())); }

bool subs_blend(SubtitleRenderer &r, int64_t ms, int32_t w, int32_t h, int32_t vw, int32_t vh,
                rust::Slice<uint8_t> rgba) {
    return r.blend(ms, w, h, vw, vh, std::span<uint8_t>(rgba.data(), rgba.size()));
}

bool subs_overlay(SubtitleRenderer &r, int64_t ms, int32_t w, int32_t h, int32_t vw, int32_t vh,
                  rust::Slice<uint8_t> rgba) {
    return r.overlay(ms, w, h, vw, vh, std::span<uint8_t>(rgba.data(), rgba.size()));
}

}  // namespace fsmedia::ffi
