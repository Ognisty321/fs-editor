//! Avisynth (.avs) and VapourSynth (.vpy) scripts as video and audio. Neither library is part
//! of the build, so each test runs against the one the machine has and, where it has none,
//! checks that opening a script says what is missing.

use std::path::{Path, PathBuf};

use fs_media::{
    Audio, ScriptKind, ScriptOptions, Video, list_tracks, load_script_library, set_script_options,
};

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

/// The same picture from Avisynth, and a tone in each channel: 440 Hz at half the level on the
/// left, 880 Hz at a quarter on the right, so swapped or mixed channels show.
const AVS: &str = r#"
function Quarter(int c) {
    return BlankClip(length=24, width=160, height=120, pixel_type="RGB32", fps=24000,
    \                fps_denominator=1001, color=c)
}
top = StackHorizontal(Quarter($FF0000), Quarter($00FF00))
bottom = StackHorizontal(Quarter($0000FF), Quarter($FFFFFF))
clip = StackVertical(top, bottom) + StackVertical(bottom, top)
CONVERT
left = Tone(length=2.0, frequency=440, samplerate=RATE, channels=1, type="Sine", level=0.5)
right = Tone(length=2.0, frequency=880, samplerate=RATE, channels=1, type="Sine", level=0.25)
return AudioDub(clip, MergeChannels(left, right))
"#;

fn avs(convert: &str, rate: u32) -> String {
    AVS.replace("CONVERT", convert)
        .replace("RATE", &rate.to_string())
}

/// What the script's tone is at sample `i` of a track at 48 kHz.
fn tone(i: usize, hz: f64, level: f64) -> f32 {
    (level * (std::f64::consts::TAU * hz * i as f64 / 48000.0).sin()) as f32
}

#[test]
fn avisynth_script() {
    let dir = tempfile::tempdir().unwrap();
    let plain = script(dir.path(), "rgb.avs", &avs("", 48000));
    if !have(ScriptKind::Avisynth, &plain, "Avisynth") {
        return;
    }
    // frame properties (the matrix a conversion leaves in them) and the deeper formats are
    // AviSynth+ 3.7's; an older Avisynth is asked for what it has
    let plus = script(
        dir.path(),
        "plus.avs",
        "BlankClip(length=1).propSet(\"_Matrix\", 1)\n",
    );
    let plus = Video::open(&plus, -1, None, |_, _| true).is_ok();
    if !plus {
        eprintln!("this Avisynth has no frame properties: only what Avisynth 2.6 has is tested");
    }
    // bottom-up packed RGB at 8 and 16 bits, planar RGB, YUV of every layout with the matrix
    // and range it was made with, and what swscale does not read: 64-bit RGB, which Avisynth
    // turns into RGB32, and float YUV, which it turns into 16 bits
    let formats = [
        ("", "bgr0", "None", true),
        ("clip = clip.ConvertToRGB24()", "bgr24", "None", true),
        ("clip = clip.ConvertToYV12()", "yuv420p", "TV.601", true),
        ("clip = clip.ConvertToYUY2()", "yuyv422", "TV.601", true),
        ("clip = clip.ConvertToRGB48()", "bgr48le", "None", false),
        ("clip = clip.ConvertToRGB64()", "bgr0", "None", false),
        ("clip = clip.ConvertToPlanarRGB()", "gbrp", "None", false),
        (
            "clip = clip.ConvertToPlanarRGBA().ConvertBits(32)",
            "gbrpf32le",
            "None",
            false,
        ),
        (
            "clip = clip.ConvertToYV12(matrix=\"Rec709\")",
            "yuv420p",
            "TV.709",
            false,
        ),
        (
            "clip = clip.ConvertToYV12(matrix=\"PC.709\")",
            "yuv420p",
            "PC.709",
            false,
        ),
        ("clip = clip.ConvertToYV411()", "yuv411p", "TV.601", false),
        (
            "clip = clip.ConvertToYUV422().ConvertBits(10)",
            "yuv422p10le",
            "TV.601",
            false,
        ),
        (
            "clip = clip.ConvertToYUV444(matrix=\"Rec709\").ConvertBits(32)",
            "yuv444p16le",
            "TV.709",
            false,
        ),
        ("clip = clip.ConvertToYUVA420()", "yuv420p", "TV.601", false),
    ];
    // a folder no code page has all the letters of
    let named = dir.path().join("zażółć クリップ");
    std::fs::create_dir(&named).unwrap();
    for (convert, format, matrix, old) in formats {
        if !plus && !old {
            continue;
        }
        let at = if plus { &named } else { dir.path() };
        let path = script(at, "clip.avs", &avs(convert, 48000));
        let mut v = Video::open(&path, -1, None, |_, _| true).unwrap();
        let p = v.props().clone();
        assert_eq!(
            (p.width, p.height, p.num_frames),
            (320, 240, 48),
            "{convert}"
        );
        assert_eq!((p.fps_num, p.fps_den), (24000, 1001));
        assert_eq!(p.pixel_format, format, "{convert}");
        if plus {
            assert_eq!(p.matrix, matrix, "{convert}");
        }
        assert_eq!(&v.timestamps_ms()[..3], &[0, 41, 83]);
        assert!(v.keyframes().is_empty() && v.is_exact() && !v.is_vfr());
        let (red, green, blue, white) = ([255, 0, 0], [0, 255, 0], [0, 0, 255], [255; 3]);
        let first = v.frame(0).unwrap();
        near(pixel(&v, &first, 80, 60), red, 2, convert);
        near(pixel(&v, &first, 240, 60), green, 2, convert);
        near(pixel(&v, &first, 80, 180), blue, 2, convert);
        near(pixel(&v, &first, 240, 180), white, 2, convert);
        let later = v.frame(30).unwrap();
        near(pixel(&v, &later, 80, 60), blue, 2, convert);
        near(pixel(&v, &later, 240, 180), green, 2, convert);
        assert_eq!(v.frame(0).unwrap(), first);
    }
    if plus {
        let anamorphic =
            "clip = clip.ConvertToYV12().propSet(\"_SARNum\", 4).propSet(\"_SARDen\", 3)";
        let path = script(dir.path(), "sar.avs", &avs(anamorphic, 48000));
        let v = Video::open(&path, -1, None, |_, _| true).unwrap();
        assert_eq!((v.props().sar_num, v.props().sar_den), (4, 3));
    }

    let tracks = list_tracks(&plain).unwrap();
    let kinds: Vec<_> = tracks.iter().map(|t| (t.index, t.kind.as_str())).collect();
    assert_eq!(kinds, [(0, "video"), (1, "audio")]);
    assert!(tracks.iter().all(|t| t.codec == "avisynth"));

    // Aegisub's audio: 16 bits whatever the script has (Tone's is float), the channels and
    // the rate as they are from 32 kHz up
    let a = Audio::open(&plain, -1, None, |_, _| true).unwrap();
    let p = *a.props();
    assert_eq!(
        (p.sample_rate, p.channels, p.bits, p.is_float, p.num_samples),
        (48000, 2, 16, false, 96000)
    );
    let mut all = vec![1.0f32; 2 * 96100];
    a.read(-50, &mut all).unwrap();
    assert!(all[..100].iter().all(|&s| s == 0.0));
    assert!(all[2 * 96050..].iter().all(|&s| s == 0.0));
    for i in (0..96000).step_by(7) {
        let (l, r) = (all[100 + 2 * i], all[101 + 2 * i]);
        assert!((l - tone(i, 440.0, 0.5)).abs() < 1e-3, "left {i}: {l}");
        assert!((r - tone(i, 880.0, 0.25)).abs() < 1e-3, "right {i}: {r}");
    }
    let mut mono = vec![7i16; 500];
    a.read_mono(1000, &mut mono).unwrap();
    for (i, &m) in mono.iter().enumerate() {
        let want = (tone(1000 + i, 440.0, 0.5) + tone(1000 + i, 880.0, 0.25)) / 2.0 * 32768.0;
        assert!((m as f32 - want).abs() < 40.0, "mono {i}: {m}, {want}");
    }
    let pcm = fs_audio::PcmBuffer::load(&a, |_, _| true).unwrap();
    assert_eq!((pcm.rate(), pcm.len()), (48000, 96000));

    // under 32 kHz it is resampled to 44100 Hz
    let low = script(dir.path(), "low.avs", &avs("", 22050));
    let a = Audio::open(&low, -1, None, |_, _| true).unwrap();
    assert_eq!(
        (a.props().sample_rate, a.props().num_samples),
        (44100, 88200)
    );
    let mut all = vec![0.0f32; 2 * 88200];
    a.read(0, &mut all).unwrap();
    let peak = |c: usize| {
        let of = all.iter().skip(c).step_by(2);
        of.fold(0.0f32, |m, &s| m.max(s.abs()))
    };
    assert!((0.4..0.55).contains(&peak(0)) && (0.2..0.28).contains(&peak(1)));

    // the settings: a rate for every script, and one channel made of the two or taken from them
    let with = |rate: i32, mixer: &str| {
        set_script_options(&ScriptOptions {
            avs_sample_rate: rate,
            avs_downmixer: mixer.into(),
            avs_memory_max_mb: 128,
            ..Default::default()
        });
        let a = Audio::open(&plain, -1, None, |_, _| true);
        set_script_options(&ScriptOptions::default());
        let a = a.unwrap();
        let mut all = vec![0.0f32; a.props().num_samples as usize];
        assert_eq!(a.props().channels, 1, "{mixer}");
        a.read(0, &mut all).unwrap();
        (a.props().sample_rate, all)
    };
    let (rate, all) = with(32000, "ConvertToMono");
    assert_eq!((rate, all.len()), (32000, 64000));
    let (rate, all) = with(0, "GetLeftChannel");
    assert_eq!((rate, all.len()), (48000, 96000));
    assert!((0..96000).all(|i| (all[i] - tone(i, 440.0, 0.5)).abs() < 1e-3));
    let (_, all) = with(0, "GetRightChannel");
    assert!((0..96000).all(|i| (all[i] - tone(i, 880.0, 0.25)).abs() < 1e-3));

    // a script with one of the two has one track, and the other does not open
    let video_only = script(dir.path(), "video.avs", "BlankClip(length=5).KillAudio()\n");
    assert_eq!(list_tracks(&video_only).unwrap().len(), 1);
    let e = Audio::open(&video_only, -1, None, |_, _| true)
        .err()
        .unwrap();
    assert!(e.to_string().contains("no audio"), "{e}");
    let audio_only = script(dir.path(), "audio.avs", "Tone(length=1.0)\n");
    let tracks = list_tracks(&audio_only).unwrap();
    assert!(tracks.len() == 1 && tracks[0].kind == "audio");
    let e = Video::open(&audio_only, -1, None, |_, _| true)
        .err()
        .unwrap();
    assert!(e.to_string().contains("no video"), "{e}");
    assert!(Audio::open(&audio_only, -1, None, |_, _| true).is_ok());

    // the script's own error, as Avisynth tells it, and a script that ends in no clip
    let broken = script(dir.path(), "broken.avs", "NotThere()\n");
    let e = Video::open(&broken, -1, None, |_, _| true).err().unwrap();
    assert!(e.to_string().contains("NotThere"), "{e}");
    let number = script(dir.path(), "number.avs", "return 5\n");
    let e = Video::open(&number, -1, None, |_, _| true).err().unwrap();
    assert!(e.to_string().contains("does not return a clip"), "{e}");
    assert!(Video::open(&dir.path().join("none.avs"), -1, None, |_, _| true).is_err());
}
