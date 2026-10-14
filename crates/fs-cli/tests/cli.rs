//! End-to-end tests of the `fsed` binary on real files.

use std::path::{Path, PathBuf};
use std::process::Command;

fn fsed() -> Command {
    Command::new(env!("CARGO_BIN_EXE_fsed"))
}

fn corpus(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../tests/corpus")
        .join(name)
}

fn run(cmd: &mut Command) -> (bool, String, String) {
    let o = cmd.output().unwrap();
    (
        o.status.success(),
        String::from_utf8_lossy(&o.stdout).into_owned(),
        String::from_utf8_lossy(&o.stderr).into_owned(),
    )
}

#[test]
fn roundtrip_corpus() {
    let dir = corpus("aegisub");
    let files: Vec<PathBuf> = std::fs::read_dir(&dir)
        .unwrap()
        .flatten()
        .map(|e| e.path())
        .collect();
    let (ok, out, _) = run(fsed().arg("roundtrip").args(&files));
    assert!(ok, "{out}");
    assert_eq!(
        out.lines().filter(|l| l.starts_with("OK")).count(),
        files.len()
    );
}

#[test]
fn info_and_lines() {
    let f = corpus("aegisub/ass-format-tests.ass");
    let (ok, out, _) = run(fsed().arg("info").arg(&f));
    assert!(ok);
    assert!(out.contains("PlayRes:     640x480"), "{out}");
    assert!(out.contains("Styles:      2"));
    let (ok, out, _) = run(fsed().args(["lines", "--style", "Alt", "--strip"]).arg(&f));
    assert!(ok);
    assert_eq!(out.lines().count(), 4, "{out}");
}

#[test]
fn shift_then_shift_back_is_identity() {
    let dir = tempfile::tempdir().unwrap();
    let src = corpus("aegisub/ass-format-tests.ass");
    let a = dir.path().join("a.ass");
    let b = dir.path().join("b.ass");
    let (ok, _, err) = run(fsed()
        .args(["shift", "--by", "1.5s"])
        .arg(&src)
        .arg("-o")
        .arg(&a));
    assert!(ok, "{err}");
    assert!(err.contains("shifted"));
    let (ok, _, _) = run(fsed()
        .args(["shift", "--by", "-1.5s"])
        .arg(&a)
        .arg("-o")
        .arg(&b));
    assert!(ok);
    assert_eq!(std::fs::read(&b).unwrap(), std::fs::read(&src).unwrap());
}

#[test]
fn frame_shift_with_vfr_timecodes() {
    let dir = tempfile::tempdir().unwrap();
    let src = corpus("aegisub/ass-format-tests.ass");
    let tc = corpus("vfr/v1_mode5.txt");
    let a = dir.path().join("a.ass");
    let b = dir.path().join("b.ass");
    assert!(
        run(fsed()
            .args(["shift", "--frames", "10", "--timecodes"])
            .arg(&tc)
            .arg(&src)
            .arg("-o")
            .arg(&a))
        .0
    );
    assert!(
        run(fsed()
            .args(["shift", "--frames", "-10", "--timecodes"])
            .arg(&tc)
            .arg(&a)
            .arg("-o")
            .arg(&b))
        .0
    );
    // frame-exact shifting is reversible on frame-aligned times
    let orig = fs_formats::load_file(&src, &Default::default())
        .unwrap()
        .doc;
    let back = fs_formats::load_file(&b, &Default::default()).unwrap().doc;
    let fr =
        fs_core::vfr::Framerate::parse_timecodes(&std::fs::read_to_string(&tc).unwrap()).unwrap();
    use fs_core::vfr::TimeType::*;
    for (x, y) in orig.events().iter().zip(back.events()) {
        assert_eq!(
            fr.frame_at_time(x.event.start.ms(), Start),
            fr.frame_at_time(y.event.start.ms(), Start)
        );
        assert_eq!(
            fr.frame_at_time(x.event.end.ms(), End),
            fr.frame_at_time(y.event.end.ms(), End)
        );
    }
}

#[test]
fn convert_to_srt_and_microdvd() {
    let dir = tempfile::tempdir().unwrap();
    let src = corpus("aegisub/ass-format-tests.ass");
    let srt = dir.path().join("x.srt");
    let sub = dir.path().join("x.sub");
    assert!(run(fsed().arg("convert").arg(&src).arg("-o").arg(&srt)).0);
    assert!(
        run(fsed()
            .arg("convert")
            .arg(&src)
            .arg("-o")
            .arg(&sub)
            .args(["--output-fps", "25"]))
        .0
    );
    let s = std::fs::read_to_string(&srt).unwrap();
    assert!(s.contains("00:00:00,000 --> 00:00:05,000"), "{s}");
    let m = std::fs::read_to_string(&sub).unwrap();
    assert!(
        m.trim_start_matches('\u{feff}').starts_with("{1}{1}25"),
        "{m}"
    );
    let (ok, out, _) = run(fsed().arg("info").arg(&sub));
    assert!(ok);
    assert!(out.contains("MicroDVD"));
}

#[test]
fn translation_paste_and_status() {
    let dir = tempfile::tempdir().unwrap();
    let src = corpus("aegisub/ass-format-tests.ass");
    let txt = dir.path().join("tl.txt");
    let out = dir.path().join("tl.ass");
    std::fs::write(&txt, "Pierwsza\nDruga\n").unwrap();
    let (ok, _, err) = run(fsed()
        .args(["tl", "paste"])
        .arg(&src)
        .arg(&txt)
        .arg("-o")
        .arg(&out));
    assert!(ok, "{err}");
    let (ok, s, _) = run(fsed().args(["tl", "status"]).arg(&out));
    assert!(ok);
    assert!(s.contains("translated: 2"), "{s}");
    let text = std::fs::read_to_string(&out).unwrap();
    assert!(text.contains("TLMode: Yes"));
    assert!(text.contains("Style: TLmode,"));
}

#[test]
fn check_reports_problems() {
    let dir = tempfile::tempdir().unwrap();
    let f = dir.path().join("bad.ass");
    std::fs::write(
        &f,
        "[Script Info]\nScriptType: v4.00+\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:02.00,0:00:01.00,Nope,,0,0,0,,x\n",
    )
    .unwrap();
    let (ok, out, _) = run(fsed().arg("check").arg(&f));
    assert!(!ok);
    assert!(out.contains("end before start"));
    assert!(out.contains("unknown style 'Nope'"));
}

#[test]
fn frame_conversion() {
    let (ok, out, _) = run(fsed().args(["frame", "f24", "--fps", "23.976"]));
    assert!(ok);
    assert!(out.contains("exact: 0:00:01.001"), "{out}");
    let (ok, out, _) = run(fsed().args(["frame", "0:00:01.001", "--fps", "24000/1001"]));
    assert!(ok);
    assert!(out.contains("frame (exact): 24"), "{out}");
}

#[cfg(feature = "lua")]
#[test]
fn lua_runs_kara_templater() {
    let auto = Path::new(env!("CARGO_MANIFEST_DIR")).join("../fs-automation");
    let script = auto.join("automation/autoload/kara-templater.lua");
    let file = auto.join("tests/aegisub/automation/kara-templater-retime.ass");
    let user = tempfile::tempdir().unwrap();
    let (ok, out, err) = run(fsed()
        .arg("lua")
        .arg(&script)
        .arg("--user")
        .arg(user.path()));
    assert!(ok, "{err}");
    assert!(out.contains("Apply karaoke template"), "{out}");
    let (ok, out, err) = run(fsed()
        .arg("lua")
        .arg(&script)
        .arg(&file)
        .args(["--macro", "Apply karaoke template", "--user"])
        .arg(user.path()));
    assert!(ok, "{err}");
    assert_eq!(
        out.lines().filter(|l| l.contains(",fx,")).count(),
        27,
        "{out}"
    );
}
