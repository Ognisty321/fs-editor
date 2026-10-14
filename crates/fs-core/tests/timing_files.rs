//! Timecode and keyframe files from Aegisub's test suite (tests/corpus).

use std::path::PathBuf;

use fs_core::keyframes;
use fs_core::vfr::{Framerate, TimeType, VfrError};

fn data(sub: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../tests/corpus")
        .join(sub)
}

fn load_tc(name: &str) -> Result<Framerate, VfrError> {
    let text = std::fs::read_to_string(data("vfr").join(name)).unwrap();
    Framerate::parse_timecodes(&text)
}

fn numbers(name: &str) -> Vec<f64> {
    let text = std::fs::read_to_string(data("vfr").join(name)).unwrap();
    text.lines()
        .skip(1)
        .filter_map(|l| l.trim().parse().ok())
        .collect()
}

#[test]
fn good_v1_files() {
    for f in [
        "v1_start_equals_end.txt",
        "v1_whitespace.txt",
        "v1_assume_int.txt",
        "v1_out_of_order.txt",
    ] {
        assert!(load_tc(f).is_ok(), "{f}: {:?}", load_tc(f).err());
    }
}

#[test]
fn bad_v1_files() {
    for f in [
        "v1_bad_seperators.txt",
        "v1_too_few_parts.txt",
        "v1_too_many_parts.txt",
        "v1_float_frame_number.txt",
    ] {
        assert!(
            matches!(load_tc(f), Err(VfrError::MalformedLine(_))),
            "{f}: {:?}",
            load_tc(f)
        );
    }
    for f in [
        "v1_start_end_overlap.txt",
        "v1_fully_contained.txt",
        "v1_assume_over_1000.txt",
        "v1_override_over_1000.txt",
        "v1_override_zero.txt",
        "v1_negative_start_of_range.txt",
        "v1_end_less_than_start.txt",
        "v1_range_too_large.txt",
        "v1_nan_fps.txt",
        "v1_tiny_fps.txt",
    ] {
        assert!(
            matches!(load_tc(f), Err(VfrError::InvalidFramerate(_))),
            "{f}: {:?}",
            load_tc(f)
        );
    }
}

#[test]
fn bad_v2_files() {
    assert!(matches!(
        load_tc("v2_empty.txt"),
        Err(VfrError::InvalidFramerate(_))
    ));
    assert!(matches!(
        load_tc("v2_out_of_order.txt"),
        Err(VfrError::InvalidFramerate(_))
    ));
    assert!(matches!(
        load_tc("empty.txt"),
        Err(VfrError::UnknownFormat(_))
    ));
}

#[test]
fn load_v2() {
    for f in ["v2_1fps.txt", "v2_comments.txt", "v2_number_in_comment.txt"] {
        let fps = load_tc(f).unwrap();
        for i in 0..30 {
            assert_eq!(fps.time_at_frame(i, TimeType::Exact), i * 1000, "{f}");
        }
    }
}

/// Compare a saved v2 file against the reference, like Aegisub's validate_save.
fn validate(good: &str, check: &str, v2_lines: Option<usize>, allow_longer: bool) -> bool {
    let g = numbers(good);
    let c: Vec<f64> = check
        .lines()
        .skip(1)
        .filter_map(|l| l.trim().parse().ok())
        .collect();
    if c.len() < g.len() || (!allow_longer && c.len() != g.len()) {
        return false;
    }
    g.iter()
        .zip(&c)
        .enumerate()
        .all(|(i, (a, b))| v2_lines.is_some_and(|n| i >= n) || a == b)
}

#[test]
fn save_v2() {
    let fps = Framerate::from_timecodes(vec![0, 100, 200]).unwrap();
    assert!(validate("v2_nolen.txt", &fps.to_v2_string(0), None, false));
    assert!(validate(
        "v2_len_3_10.txt",
        &fps.to_v2_string(10),
        Some(3),
        false
    ));

    let fps = load_tc("v1_mode5.txt").unwrap();
    assert!(validate("v2_mode5.txt", &fps.to_v2_string(0), None, true));

    let fps = load_tc("v1_assume_30.txt").unwrap();
    assert!(validate(
        "v2_100_frames_30_fps.txt",
        &fps.to_v2_string(100),
        None,
        false
    ));

    let fps = load_tc("v1_assume_30_with_override.txt").unwrap();
    assert!(validate(
        "v2_100_frames_30_with_override.txt",
        &fps.to_v2_string(100),
        None,
        false
    ));
}

fn load_kf(name: &str) -> Result<Vec<i64>, keyframes::KeyframeError> {
    keyframes::parse(&String::from_utf8_lossy(
        &std::fs::read(data("keyframe").join(name)).unwrap(),
    ))
}

#[test]
fn keyframe_files() {
    assert_eq!(
        load_kf("x264.log").unwrap(),
        (0..41).map(|i| i * 250).collect::<Vec<_>>()
    );
    assert_eq!(
        load_kf("aegi.txt").unwrap(),
        (0..135).map(|i| i * 250).collect::<Vec<_>>()
    );
    let xvid = load_kf("xvid.txt").unwrap();
    assert_eq!(
        &xvid[..15],
        &[
            0, 52, 53, 55, 57, 80, 114, 168, 182, 234, 240, 242, 244, 250, 251
        ]
    );
    assert!(load_kf("empty.txt").is_err());
    assert!(load_kf("garbage.txt").is_err());
}
