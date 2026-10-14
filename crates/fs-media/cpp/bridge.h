// cxx glue between fsmedia (fsmedia.h) and Rust (src/ffi.rs).
#pragma once

#include <cstdint>
#include <memory>

#include "fsmedia.h"
#include "rust/cxx.h"

namespace fsmedia::ffi {

struct TrackInfo;
struct FontFile;
struct VideoProps;
struct AudioProps;
struct ProgressSink;

rust::Vec<TrackInfo> list_tracks(rust::Str path);
rust::Vec<FontFile> attached_fonts(rust::Str path);

std::unique_ptr<VideoSource> open_video(rust::Str path, int32_t track, rust::Str cache_dir, ProgressSink &progress);
VideoProps video_props(const VideoSource &v);
rust::Vec<int64_t> video_timestamps(const VideoSource &v);
rust::Vec<int64_t> video_keyframes(const VideoSource &v);
bool video_linear(const VideoSource &v);
void video_set_matrix(VideoSource &v, uint8_t matrix, uint8_t range);
void video_frame_rgba(VideoSource &v, int64_t n, rust::Slice<uint8_t> out);

std::unique_ptr<AudioSource> open_audio(rust::Str path, int32_t track, rust::Str cache_dir, ProgressSink &progress);
AudioProps audio_props(const AudioSource &a);
void audio_fill(AudioSource &a, int64_t start, rust::Slice<float> out);

std::unique_ptr<SubtitleRenderer> new_subtitle_renderer();
void subs_set_fonts(SubtitleRenderer &r, rust::Str family, rust::Str dir);
void subs_add_font(SubtitleRenderer &r, rust::Str name, rust::Slice<const uint8_t> data);
void subs_set_script(SubtitleRenderer &r, rust::Str ass);
bool subs_blend(SubtitleRenderer &r, int64_t ms, int32_t w, int32_t h, int32_t vw, int32_t vh, rust::Slice<uint8_t> rgba);
bool subs_overlay(SubtitleRenderer &r, int64_t ms, int32_t w, int32_t h, int32_t vw, int32_t vh,
                  rust::Slice<uint8_t> rgba);

}  // namespace fsmedia::ffi
