//! Avisynth (.avs) and VapourSynth (.vpy) scripts as video and audio. Neither library is part
//! of the build, so each test runs against the one the machine has and, where it has none,
//! checks that opening a script says what is missing.

use std::path::{Path, PathBuf};

use fs_media::{Audio, ScriptKind, Video, list_tracks, load_script_library};

fn script(dir: &Path, name: &str, text: &str) -> PathBuf {
    let p = dir.join(name);
    std::fs::write(&p, text).unwrap();
    p
}

/// True when the library is there. When it is not, opening `probe` must fail naming it.
fn have(kind: ScriptKind, probe: &Path, name: &str) -> bool {
    match load_script_library(kind) {
        Ok(()) => true,
        Err(e) => {
            let e = e.to_string();
            eprintln!("{name} is not on this machine, only its error is tested: {e}");
            // not there: what to install; there and broken: what is wrong with it
            assert!(e.contains(name), "{e}");
            assert!(e.contains("Install") || e.contains("cannot be used"), "{e}");
            let opened = Video::open(probe, -1, None, |_, _| true);
            assert!(opened.is_err_and(|e| e.to_string().contains(name)));
            let opened = Audio::open(probe, -1, None, |_, _| true);
            assert!(opened.is_err_and(|e| e.to_string().contains(name)));
            assert!(list_tracks(probe).is_err());
            false
        }
    }
}

fn pixel(v: &Video, rgba: &[u8], x: usize, y: usize) -> [u8; 4] {
    let at = (y * v.width() + x) * 4;
    rgba[at..at + 4].try_into().unwrap()
}

fn near(got: [u8; 4], want: [u8; 3], tolerance: i32, what: &str) {
    let off = (0..3).any(|c| (got[c] as i32 - want[c] as i32).abs() > tolerance);
    assert!(!off && got[3] == 255, "{what}: {got:?}, expected {want:?}");
}

/// Four quarters of flat colour, so a flipped picture or swapped planes show: red and green
/// over blue and white in the first second, the same turned upside down in the next.
const VPY: &str = r#"
import vapoursynth as vs
core = vs.core

def quarter(color):
    return core.std.BlankClip(width=160, height=120, format=vs.RGB24, length=24,
                              fpsnum=24000, fpsden=1001, color=color)

top = core.std.StackHorizontal([quarter([255, 0, 0]), quarter([0, 255, 0])])
bottom = core.std.StackHorizontal([quarter([0, 0, 255]), quarter([255, 255, 255])])
clip = core.std.StackVertical([top, bottom]) + core.std.StackVertical([bottom, top])
CONVERT
clip.set_output(0)
core.std.BlankAudio(channels=[vs.FRONT_LEFT, vs.FRONT_RIGHT], bits=BITS,
                    sampletype=SAMPLES, samplerate=48000, length=96000).set_output(1)
__aegi_keyframes = [0, 24]
"#;

fn vpy(convert: &str, bits: u32, float: bool) -> String {
    VPY.replace("CONVERT", convert)
        .replace("BITS", &bits.to_string())
        .replace("SAMPLES", if float { "vs.FLOAT" } else { "vs.INTEGER" })
}

#[test]
fn vapoursynth_script_video() {
    let dir = tempfile::tempdir().unwrap();
    let plain = script(dir.path(), "rgb.vpy", &vpy("", 16, false));
    if !have(ScriptKind::VapourSynth, &plain, "VapourSynth") {
        return;
    }
    // planar RGB, 4:2:0 at 8 and 10 bits with a matrix of its own, and formats swscale does
    // not read (float), which the script's core turns into 16 bits first
    for (convert, format, matrix, tolerance) in [
        ("", "gbrp", "None", 0),
        (
            "clip = core.resize.Bicubic(clip, format=vs.YUV420P8, matrix_s='709')",
            "yuv420p",
            "TV.709",
            3,
        ),
        (
            "clip = core.resize.Bicubic(clip, format=vs.YUV420P10, matrix_s='170m', range_s='full')",
            "yuv420p10le",
            "PC.601",
            3,
        ),
        (
            "clip = core.resize.Bicubic(clip, format=vs.YUV444PS, matrix_s='709')",
            "yuv444p16le",
            "TV.709",
            2,
        ),
        (
            "clip = core.resize.Bicubic(clip, format=vs.RGBS)",
            "gbrpf32le",
            "None",
            1,
        ),
        (
            "clip = core.std.ShufflePlanes(clip, 1, vs.GRAY)",
            "gray",
            "",
            0,
        ),
    ] {
        let path = script(dir.path(), "clip.vpy", &vpy(convert, 16, false));
        let mut v = Video::open(&path, -1, None, |_, _| true).unwrap();
        let p = v.props().clone();
        assert_eq!(
            (p.width, p.height, p.num_frames),
            (320, 240, 48),
            "{format}"
        );
        assert_eq!((p.fps_num, p.fps_den), (24000, 1001));
        assert_eq!(
            (p.pixel_format.as_str(), p.matrix.as_str()),
            (format, matrix)
        );
        assert!((p.duration_s - 48.0 * 1001.0 / 24000.0).abs() < 1e-9);
        assert_eq!(v.timestamps_ms().len(), 48);
        assert_eq!(&v.timestamps_ms()[..3], &[0, 41, 83]);
        assert_eq!(v.keyframes(), &[0, 24]);
        assert!(v.is_exact() && !v.is_vfr() && !v.linear_decoding());

        let (red, green, blue, white) = ([255, 0, 0], [0, 255, 0], [0, 0, 255], [255; 3]);
        // the grey clip is the RGB one's green plane
        let grey = |c: [u8; 3]| if format == "gray" { [c[1]; 3] } else { c };
        let first = v.frame(0).unwrap();
        near(pixel(&v, &first, 80, 60), grey(red), tolerance, format);
        near(pixel(&v, &first, 240, 60), grey(green), tolerance, format);
        near(pixel(&v, &first, 80, 180), grey(blue), tolerance, format);
        near(pixel(&v, &first, 240, 180), grey(white), tolerance, format);
        let later = v.frame(30).unwrap();
        near(pixel(&v, &later, 80, 60), grey(blue), tolerance, format);
        near(pixel(&v, &later, 240, 180), grey(green), tolerance, format);
        // any frame again, in any order
        assert_eq!(v.frame(0).unwrap(), first);
    }
}

#[test]
fn vapoursynth_script_audio_and_tracks() {
    let dir = tempfile::tempdir().unwrap();
    let path = script(dir.path(), "av.vpy", &vpy("", 16, false));
    if !have(ScriptKind::VapourSynth, &path, "VapourSynth") {
        return;
    }
    let tracks = list_tracks(&path).unwrap();
    let kinds: Vec<_> = tracks.iter().map(|t| (t.index, t.kind.as_str())).collect();
    assert_eq!(kinds, [(0, "video"), (1, "audio")]);
    assert!(tracks.iter().all(|t| t.codec == "vapoursynth"));
    assert!(fs_media::subtitle_tracks(&path).unwrap().is_empty());
    assert!(fs_media::attached_fonts(&path).unwrap().is_empty());
    assert!(fs_media::chapters(&path).unwrap().is_empty());

    for (bits, float) in [(16, false), (24, false), (32, true)] {
        let path = script(dir.path(), "audio.vpy", &vpy("", bits, float));
        let a = Audio::open(&path, -1, None, |_, _| true).unwrap();
        let p = *a.props();
        assert_eq!(
            (p.sample_rate, p.channels, p.num_samples),
            (48000, 2, 96000)
        );
        assert_eq!((p.bits, p.is_float), (bits as i32, float));
        // BlankAudio is all VapourSynth can make without a plugin: silence, read across its
        // frames of 3072 samples and past both ends
        let mut all = vec![1.0f32; 2 * 96100];
        a.read(-50, &mut all).unwrap();
        assert!(all.iter().all(|&s| s == 0.0));
        let mut mono = vec![7i16; 5000];
        a.read_mono(94000, &mut mono).unwrap();
        assert!(mono.iter().all(|&s| s == 0));
        assert!(a.seeks());
    }
    // the editor's buffer of the track, as the window loads it
    let a = Audio::open(&path, -1, None, |_, _| true).unwrap();
    let pcm = fs_audio::PcmBuffer::load(&a, |_, _| true).unwrap();
    assert_eq!((pcm.rate(), pcm.len()), (48000, 96000));

    // a script with video alone has one track, and no audio to open
    let video_only = "import vapoursynth as vs\nvs.core.std.BlankClip(length=5).set_output()\n";
    let path = script(dir.path(), "video.vpy", video_only);
    assert_eq!(list_tracks(&path).unwrap().len(), 1);
    let e = Audio::open(&path, -1, None, |_, _| true).err().unwrap();
    assert!(e.to_string().contains("no audio"), "{e}");
}

#[test]
fn vapoursynth_script_errors() {
    let dir = tempfile::tempdir().unwrap();
    let broken = script(
        dir.path(),
        "broken.vpy",
        "import vapoursynth as vs\nnot_there()\n",
    );
    if !have(ScriptKind::VapourSynth, &broken, "VapourSynth") {
        return;
    }
    // the script's own error, as Python tells it
    let e = Video::open(&broken, -1, None, |_, _| true).err().unwrap();
    assert!(e.to_string().contains("not_there"), "{e}");
    let silent = script(dir.path(), "silent.vpy", "import vapoursynth as vs\n");
    let e = Video::open(&silent, -1, None, |_, _| true).err().unwrap();
    assert!(e.to_string().contains("set_output"), "{e}");
    let e = Video::open(&dir.path().join("none.vpy"), -1, None, |_, _| true);
    assert!(e.is_err());
    // a frame that fails when it is made, not when the script is read
    let late = "import vapoursynth as vs\ncore = vs.core\nc = core.std.BlankClip(length=5)\n\
                def fail(n):\n    raise RuntimeError('frame %d is not there' % n)\n\
                core.std.FrameEval(c, fail).set_output()\n";
    let late = script(dir.path(), "late.vpy", late);
    let e = Video::open(&late, -1, None, |_, _| true).err().unwrap();
    assert!(e.to_string().contains("VapourSynth"), "{e}");
}

/// Variable frame rate has no rate in the clip: the first frame's duration stands in, and the
/// script's own times are taken when it gives them.
#[test]
fn vapoursynth_script_timecodes() {
    let dir = tempfile::tempdir().unwrap();
    let text = "import vapoursynth as vs\ncore = vs.core\n\
                a = core.std.BlankClip(length=3, fpsnum=25, fpsden=1)\n\
                b = core.std.BlankClip(length=3, fpsnum=50, fpsden=1)\n\
                (a + b).set_output()\n";
    let path = script(dir.path(), "vfr.vpy", text);
    if !have(ScriptKind::VapourSynth, &path, "VapourSynth") {
        return;
    }
    let v = Video::open(&path, -1, None, |_, _| true).unwrap();
    assert_eq!((v.props().fps_num, v.props().fps_den), (25, 1));
    assert_eq!(v.timestamps_ms(), &[0, 40, 80, 120, 160, 200]);
    let with = format!("{text}__aegi_timecodes = [0, 40, 80, 120, 140, 160]\n");
    let path = script(dir.path(), "timed.vpy", &with);
    let v = Video::open(&path, -1, None, |_, _| true).unwrap();
    assert_eq!(v.timestamps_ms(), &[0, 40, 80, 120, 140, 160]);
    assert!(v.is_vfr());
    assert!((v.props().duration_s - 0.18).abs() < 1e-9);
}

const AVS: &str = r#"
function Quarter(int c) {
    return BlankClip(length=24, width=160, height=120, pixel_type="RGB32", fps=24000,
    \                fps_denominator=1001, color=c, audio_rate=48000, channels=2,
    \                sample_type="float")
}
top = StackHorizontal(Quarter($FF0000), Quarter($00FF00))
bottom = StackHorizontal(Quarter($0000FF), Quarter($FFFFFF))
clip = StackVertical(top, bottom) + StackVertical(bottom, top)
CONVERT
return clip
"#;

#[test]
fn avisynth_script() {
    let dir = tempfile::tempdir().unwrap();
    let plain = script(dir.path(), "rgb.avs", &AVS.replace("CONVERT", ""));
    if !have(ScriptKind::Avisynth, &plain, "Avisynth") {
        return;
    }
    // bottom-up packed RGB, planar YUV with Avisynth's default matrix for it (Rec601, which a
    // clip this small is taken for), and the same at 10 bits
    for (convert, format, tolerance) in [
        ("", "bgr0", 0),
        ("clip = clip.ConvertToRGB24()", "bgr24", 0),
        ("clip = clip.ConvertToYV12()", "yuv420p", 4),
        (
            "clip = clip.ConvertToYUV422().ConvertBits(10)",
            "yuv422p10le",
            4,
        ),
        ("clip = clip.ConvertToPlanarRGB()", "gbrp", 0),
    ] {
        let path = script(dir.path(), "clip.avs", &AVS.replace("CONVERT", convert));
        let mut v = Video::open(&path, -1, None, |_, _| true).unwrap();
        let p = v.props().clone();
        assert_eq!(
            (p.width, p.height, p.num_frames),
            (320, 240, 48),
            "{format}"
        );
        assert_eq!((p.fps_num, p.fps_den), (24000, 1001));
        assert_eq!(p.pixel_format, format);
        assert_eq!(&v.timestamps_ms()[..3], &[0, 41, 83]);
        assert!(v.keyframes().is_empty() && v.is_exact());
        let (red, green, blue, white) = ([255, 0, 0], [0, 255, 0], [0, 0, 255], [255; 3]);
        let first = v.frame(0).unwrap();
        near(pixel(&v, &first, 80, 60), red, tolerance, format);
        near(pixel(&v, &first, 240, 60), green, tolerance, format);
        near(pixel(&v, &first, 80, 180), blue, tolerance, format);
        near(pixel(&v, &first, 240, 180), white, tolerance, format);
        let later = v.frame(30).unwrap();
        near(pixel(&v, &later, 80, 60), blue, tolerance, format);
        near(pixel(&v, &later, 240, 180), green, tolerance, format);
    }

    let tracks = list_tracks(&plain).unwrap();
    let kinds: Vec<_> = tracks.iter().map(|t| t.kind.as_str()).collect();
    assert_eq!(kinds, ["video", "audio"]);
    // Aegisub's audio: 16 bits whatever the script has, at the script's rate from 32 kHz up
    let a = Audio::open(&plain, -1, None, |_, _| true).unwrap();
    let p = *a.props();
    assert_eq!(
        (p.sample_rate, p.channels, p.bits, p.is_float),
        (48000, 2, 16, false)
    );
    assert_eq!(p.num_samples, 2 * 48048);
    let mut all = vec![1.0f32; 2 * 5000];
    a.read(-50, &mut all).unwrap();
    assert!(all.iter().all(|&s| s == 0.0));

    let broken = script(dir.path(), "broken.avs", "NotThere()\n");
    let e = Video::open(&broken, -1, None, |_, _| true).err().unwrap();
    assert!(e.to_string().contains("NotThere"), "{e}");
}
