// Avisynth (.avs) and VapourSynth (.vpy) scripts as video and audio sources, behind VideoSource
// and AudioSource. Neither library is linked: Avisynth is loaded when the first .avs is opened
// and driven through its C interface, VapourSynth through VSScript, both found at run time.
#pragma once

#include "fsmedia.h"

struct AVFrame;

namespace fsmedia::script {

enum class Kind { None, Avisynth, VapourSynth };
/// By the file's extension.
Kind kind_of(const std::filesystem::path &file);

/// What a script is evaluated for: its video, its audio, or only to know which of them it has.
enum class Want { Video, Audio, Tracks };

/// One evaluated script.
class Clip {
public:
    virtual ~Clip() = default;

    bool has_video = false;
    bool has_audio = false;
    /// Size, frame count, rate and duration; the pixel format, colour and SAR are frame 0's.
    VideoInfo video;
    std::vector<int64_t> timestamps;
    std::vector<int64_t> keyframes;
    AudioInfo audio;
    /// Set when the frames are not in the clip's own colour space (the script library made them
    /// RGB): the AVColorSpace and AVColorRange the clip has, for what the editor reports.
    int source_colorspace = -1;
    int source_range = 0;

    /// Frame `n` as the script gives it, its planes and colour tags in an AVFrame that stays
    /// valid until the next call.
    virtual const AVFrame *frame(int64_t n) = 0;
    /// `count` samples from `start`, both inside the track, interleaved.
    virtual void samples(int64_t start, int64_t count, float *out) = 0;
    /// The matrix the editor decodes with, for a clip the script library itself converts to RGB.
    virtual void set_matrix(Matrix, bool /*full_range*/, bool /*override_range*/) {}
};

/// Evaluates the script; throws MediaError with the script's own message, or one naming what to
/// install when the library is not there.
std::unique_ptr<Clip> open(const std::filesystem::path &file, Want want);

/// A video and an audio track for what the script outputs, from the last evaluation of the file
/// as it is now, or from a new one.
std::vector<Track> tracks(const std::filesystem::path &file);

/// True when `tracks` would answer without evaluating the script.
bool evaluated(const std::filesystem::path &file);

}  // namespace fsmedia::script
