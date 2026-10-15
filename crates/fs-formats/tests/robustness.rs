//! Mutated corpus files through every parser and the common document operations: a
//! panic here would abort the GUI (a Rust panic cannot unwind across cxx).
//! `FSED_FUZZ_ITERS=n` runs n mutations per file, big files included (default 50, files
//! under 64 kB).

use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use fs_core::ops::{self, JoinMode, Rows, SortKey};
use fs_core::vfr::Framerate;
use fs_core::{Document, karaoke::Karaoke, keyframes, tags, tlmode};
use fs_formats::{Format, LoadOptions, SaveOptions, load_bytes, markup, save_bytes};

const TOKENS: &[&str] = &[
    "{",
    "}",
    "\\",
    ",",
    ":",
    ".",
    "-",
    "\n",
    "\r\n",
    "\r",
    "[Events]\n",
    "[V4+ Styles]\n",
    "[Script Info]\n",
    "Format: ",
    "Dialogue: ",
    "Comment: ",
    "Style: ",
    "\\k",
    "\\kf-5",
    "\\t(",
    "\\t(0,",
    "\\pos(",
    "\\move(1,2,3,4,",
    "\\clip(m 0 0 l",
    "\\p1}",
    "\\p0",
    "m ",
    "b 1 2",
    "s",
    "c",
    "\\fad(",
    "\\N",
    "(",
    ")",
    "&H",
    "&HFFFFFFFF&",
    "9999999999",
    "-2147483649",
    "\u{FEFF}",
    "\u{FFFD}",
    "ą",
    "日本",
    "\x00",
    "0:00:00.00",
    "9:59:59.99",
    "-0:00:01.00",
    "-->",
    "00:00:01,000 --> ",
    "{1}{2}",
    "{1}{1}23.976",
    "[1][2]",
    "00:00:01:",
    "1\n",
    "Fontname",
    "PlayResX: 0\n",
    "PlayResY: -5\n",
    "ScriptType: v4.00\n",
    "\\fs0",
    "\\fscx-1",
];

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        // xorshift64*
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    fn below(&mut self, n: usize) -> usize {
        if n == 0 {
            0
        } else {
            (self.next() % n as u64) as usize
        }
    }
}

fn mutate(src: &[u8], rng: &mut Rng) -> Vec<u8> {
    let mut b = src.to_vec();
    for _ in 0..1 + rng.below(4) {
        let at = rng.below(b.len() + 1);
        match rng.below(6) {
            0 if !b.is_empty() => {
                let i = at.min(b.len() - 1);
                b[i] = rng.next() as u8;
            }
            1 | 2 => {
                let t = TOKENS[rng.below(TOKENS.len())].as_bytes();
                b.splice(at..at, t.iter().copied());
            }
            3 => {
                let end = (at + rng.below(64)).min(b.len());
                b.drain(at..end);
            }
            4 => {
                let end = (at + rng.below(256)).min(b.len());
                let chunk = b[at..end].to_vec();
                let to = rng.below(b.len() + 1);
                b.splice(to..to, chunk);
            }
            _ => b.truncate(at),
        }
    }
    b
}

fn exercise_text(text: &str) {
    let line = tags::Line::parse(text);
    let _ = line.to_text();
    let _ = line.plain_text();
    let _ = tags::strip_tags(text);
    let _ = tags::display_text(text);
    let _ = tags::visible_text(text);
    let _ = tags::cps(text, 1000);
    let _ = tags::char_count(text, true, true);
    let syl = tags::karaoke_syllables(text);
    let _ = tags::join_syllables(&syl);
    let _ = ops::drawing_of(text);
    let d = tags::parse_drawing(text);
    let _ = tags::write_drawing(&d);
    let _ = tags::drawing_bounds(&d);
    let (runs, an) = markup::ass_to_runs(text);
    let _ = markup::runs_to_ass(&runs, an);
    let _ = markup::runs_to_html(&runs, an);
    let _ = markup::html_to_runs(text);
    for tag in ["fs", "pos", "fscx", "frz", "t"] {
        let _ = fs_core::tag_adjust::adjust_tag(
            text,
            None,
            tag,
            fs_core::tag_adjust::TagAdjust::Add,
            &[1.5, 1.5],
            text.len() / 2,
            1,
        );
    }
}

fn exercise_doc(mut doc: Document) {
    let _ = doc.to_ass_string();
    let n = doc.event_count();
    for i in 0..n.min(40) {
        let e = doc.event(i).unwrap().clone();
        exercise_text(&e.text);
        let mut k = Karaoke::from_event(&e);
        let _ = k.markers();
        let _ = k.text();
        if k.syls.len() > 1 {
            k.split(0, 1);
            k.join(0);
        }
    }
    let _ = ops::stats(&doc, 15.0);
    let _ = ops::overlapping_pairs(&doc);
    let _ = fs_core::fonts::used_fonts(&doc);
    let _ = tlmode::rows(&doc);
    let all: Vec<usize> = (0..n).collect();
    let fr = Framerate::from_rational(24000, 1001, true).unwrap();
    ops::change_fps(&mut doc, &fr, &Framerate::from_fps(25.0).unwrap());
    ops::snap_to_frames(&mut doc, &Rows::All, &fr);
    ops::sort_events(&mut doc, SortKey::Start, None);
    let pairs: Vec<(usize, usize)> = all.windows(2).map(|w| (w[0], w[1])).collect();
    ops::make_continuous(&mut doc, &pairs, true);
    ops::fix_styles(&mut doc);
    let mut d2 = doc.clone();
    let _ = ops::split_by_karaoke(&mut d2, &all);
    let mut d3 = doc.clone();
    let _ = ops::recombine(&mut d3, &all);
    if n >= 2 {
        let mut d4 = doc.clone();
        let _ = ops::join(&mut d4, &[0, 1], JoinMode::AsKaraoke);
        let mut d5 = doc.clone();
        let len = d5.event(0).unwrap().text.len();
        let _ = ops::split_at(&mut d5, 0, len / 2, None);
    }
    tlmode::enable(&mut doc);
    let _ = tlmode::rows(&doc);
    tlmode::disable(&mut doc);
    ops::remove_unused_styles(&mut doc);
    let _ = doc.to_ass_string();
}

fn exercise(bytes: &[u8], ext: Option<&str>) {
    let _ = keyframes::parse(&String::from_utf8_lossy(bytes));
    let _ = Framerate::parse_timecodes(&String::from_utf8_lossy(bytes));
    let forced = [
        None,
        Some(Format::Ass),
        Some(Format::Srt),
        Some(Format::Mpl2),
        Some(Format::MicroDvd),
        Some(Format::Tmp),
    ];
    for format in forced {
        let opts = LoadOptions {
            format,
            ..LoadOptions::default()
        };
        let Ok(l) = load_bytes(bytes, ext, &opts) else {
            continue;
        };
        for f in [
            Format::Ass,
            Format::Ssa,
            Format::Srt,
            Format::Mpl2,
            Format::MicroDvd,
            Format::Tmp,
        ] {
            let _ = save_bytes(&l.doc, f, &SaveOptions::default());
        }
        exercise_doc(l.doc);
    }
}

fn corpus() -> Vec<PathBuf> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests");
    let mut v = Vec::new();
    for d in [
        "corpus/aegisub",
        "corpus/charset",
        "corpus/keyframe",
        "corpus/vfr",
        "corpus-local",
    ] {
        if let Ok(rd) = std::fs::read_dir(root.join(d)) {
            v.extend(rd.flatten().map(|e| e.path()).filter(|p| p.is_file()));
        }
    }
    v.sort();
    v
}

#[test]
fn mutated_inputs_do_not_panic() {
    let set: Option<usize> = std::env::var("FSED_FUZZ_ITERS")
        .ok()
        .and_then(|s| s.parse().ok());
    let iters = set.unwrap_or(50);
    let mut failures = Vec::new();
    // the hook keeps the panic's location and message (the payload alone has no location)
    static LAST: Mutex<String> = Mutex::new(String::new());
    let hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|info| {
        *LAST.lock().unwrap_or_else(|e| e.into_inner()) = info.to_string().replace('\n', " ")
    }));
    for path in corpus() {
        let src = std::fs::read(&path).unwrap();
        // the big real scripts of corpus-local take seconds per mutation: only when asked
        if src.len() > if set.is_some() { 2_000_000 } else { 64_000 } {
            continue;
        }
        let ext = path.extension().and_then(|e| e.to_str()).map(str::to_owned);
        let mut rng = Rng(0x9E37_79B9_7F4A_7C15 ^ src.len() as u64);
        for i in 0..iters {
            let m = mutate(&src, &mut rng);
            if catch_unwind(AssertUnwindSafe(|| exercise(&m, ext.as_deref()))).is_err() {
                let msg = LAST.lock().unwrap_or_else(|e| e.into_inner()).clone();
                let out = std::env::temp_dir().join(format!(
                    "fsed-fuzz-{}-{i}",
                    path.file_name().unwrap().to_string_lossy()
                ));
                let _ = std::fs::write(&out, &m);
                failures.push(format!(
                    "{}: {msg} (input: {})",
                    path.display(),
                    out.display()
                ));
            }
        }
    }
    std::panic::set_hook(hook);
    assert!(
        failures.is_empty(),
        "{} panics:\n{}",
        failures.len(),
        failures.join("\n")
    );
}
