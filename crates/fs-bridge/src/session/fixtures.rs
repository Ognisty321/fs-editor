//! What the tests of the session's modules start from.

use super::Session;

pub(super) fn session(text: &str) -> Session {
    let dir = std::env::temp_dir().join(format!("fsb-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    // one file per call: tests run in parallel and share the fixtures
    static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let p = dir.join(format!(
        "{}.ass",
        N.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    ));
    std::fs::write(&p, text).unwrap();
    let mut s = Session::default();
    s.open(p.to_str().unwrap()).unwrap();
    s
}

pub(super) const ASS: &str = "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,one\nDialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,two\nDialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,three\n";
