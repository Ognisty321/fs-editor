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

/// The editing helpers of the text field: highlighting, call tips, searching and replacing,
/// tag adjusting and the kanji timer.
fn exercise_text_tools(text: &str) {
    use fs_core::ops::{CaseRule, Field, Replace, TextMatcher};
    use fs_core::tag_adjust::{TagAdjust, adjust_tag, line_values};

    let bounds: Vec<usize> = (0..=text.len())
        .filter(|&i| text.is_char_boundary(i))
        .collect();
    let some: Vec<usize> = bounds
        .iter()
        .copied()
        .step_by(1 + bounds.len() / 8)
        .chain([text.len(), text.len() + 3])
        .collect();
    for template in [false, true] {
        let runs = fs_core::syntax::highlight(text, template);
        assert_eq!(runs.iter().map(|r| r.1).sum::<usize>(), text.len());
    }
    for &p in &some {
        let _ = fs_core::calltip::calltip(text, p);
    }
    let _ = tags::translatable_ranges(text);
    let _ = tags::without_plain_text(text);
    let _ = tags::wrap_len(text);
    for span in tags::block_spans(text) {
        let _ = &text[span];
    }
    let turn = |x: f64, y: f64| (-y, x);
    let _ = ops::transform_place(text, &turn, true, (10.0, 20.0));
    let _ = ops::transform_place(text, &turn, false, (10.0, 20.0));

    let modes = [
        TagAdjust::Add,
        TagAdjust::Scale,
        TagAdjust::Set,
        TagAdjust::Insert { at: None },
        TagAdjust::Insert {
            at: Some((bounds[bounds.len() / 3], bounds[bounds.len() / 2])),
        },
        TagAdjust::Multiply,
        TagAdjust::GradientLine { increasing: true },
        TagAdjust::GradientText { increasing: false },
    ];
    for tag in [
        "alpha", "1a", "c", "3c", "fad", "move", "clip", "bord", "fax", "org", "t", "blur", "k",
    ] {
        let _ = line_values(text, None, tag, 4);
        for mode in modes {
            let _ = adjust_tag(text, None, tag, mode, &[2.0, -3.5, 1e12, f64::NAN], 1, 3);
            let _ = adjust_tag(text, None, tag, mode, &[], 0, 1);
        }
    }

    let patterns = [
        ("", false),
        ("a", false),
        ("\\", false),
        ("{", false),
        ("", true),
        ("a*", true),
        (r"\b", true),
        (r"(.)(.)?", true),
        (r"\\N|$", true),
        (r"[^{}]*", true),
    ];
    static MATCHERS: std::sync::OnceLock<Vec<TextMatcher>> = std::sync::OnceLock::new();
    let matchers = MATCHERS.get_or_init(|| {
        (patterns.iter())
            .filter_map(|&(pat, is_regex)| TextMatcher::new(pat, is_regex, false, Field::Text).ok())
            .collect()
    });
    for matcher in matchers {
        for (strip_tags, only_tags) in [(false, false), (true, false), (false, true)] {
            let mut matcher = matcher.clone();
            matcher.strip_tags = strip_tags;
            matcher.only_tags = only_tags;
            for case in [CaseRule::Written, CaseRule::Follow, CaseRule::Upper] {
                let r = Replace {
                    matcher: matcher.clone(),
                    replacement: r"<$2\1${1}$9\\\N\1c>ż".into(),
                    first_only: false,
                    case,
                };
                for &p in &some {
                    if let Some(at) = r.matcher.find_at(text, p) {
                        assert!(text.is_char_boundary(at.start) && text.is_char_boundary(at.end));
                        assert!(at.start <= at.end && at.end <= text.len());
                        let _ = r.replace_one(text, at);
                    }
                }
            }
        }
    }

    let syls: Vec<(i64, String)> = tags::karaoke_syllables(text)
        .into_iter()
        .map(|s| (s.duration_ms, s.text))
        .collect();
    let names: Vec<&str> = syls.iter().map(|s| s.1.as_str()).collect();
    let _ = fs_core::kanji_timer::auto_match(&names, text);
    let mut m = fs_core::kanji_timer::KanjiMatcher::new(syls, &tags::strip_tags(text));
    for step in 0..64 {
        match step % 7 {
            0 => m.auto_match(),
            1 => drop(m.increase_source()),
            2 => drop(m.increase_destination()),
            3 => drop(m.decrease_source()),
            4 => drop(m.accept()),
            5 => drop(m.decrease_destination()),
            _ => drop(m.accept()),
        }
        let _ = (m.output(), m.groups(), m.remaining());
        let _ = (m.source_selection(), m.unmatched_source());
        let _ = (m.destination_selection(), m.unmatched_destination());
    }
    while m.undo() {}

    let _ = fs_core::Time::parse_ass(text);
    let _ = fs_core::Time::parse_srt(text);
    let _ = fs_core::Time::parse_offset(text);
    let _ = fs_core::Time::from_ass_field(text).to_ass();
    let _ = fs_core::Color::parse_style(text);
    let _ = fs_core::Color::parse_tag(text);
    let _ = fs_core::Color::parse_alpha(text);
    let _ = fs_core::Color::parse_html(text);
}

/// The document-wide tools: shifting, post-processing, resampling, replacing, filtering, the
/// translation grid, undo and the exporters.
fn exercise_doc_tools(doc: &Document) {
    use fs_core::Time;
    use fs_core::ops::{
        ArMode, CaseRule, Field, Filter, MoveTargets, PostProcess, Replace, Resample, ShiftAmount,
        ShiftWhich, TextMatcher,
    };
    use fs_formats::export;

    // the first lines are enough: every tool below walks the whole document several times
    let mut doc = doc.clone();
    let extra: Vec<usize> = (60..doc.event_count()).collect();
    doc.remove_events(&extra);
    let doc = &doc;
    let n = doc.event_count();
    let fr = Framerate::from_rational(24000, 1001, true).unwrap();
    let vfr = Framerate::from_timecodes(vec![0, 40, 90, 91, 500, 5000]).unwrap();
    let mut history = fs_core::history::Editor::new(doc.clone());

    for (amount, which) in [
        (ShiftAmount::Time(Time(-123_456)), ShiftWhich::Both),
        (ShiftAmount::Time(Time(i64::MAX / 4)), ShiftWhich::End),
        (ShiftAmount::Frames(-7, &fr), ShiftWhich::Start),
        (ShiftAmount::Frames(1_000_000, &vfr), ShiftWhich::Both),
    ] {
        history.edit("shift", |d| {
            ops::shift_times(d, &Rows::All, amount, which, None);
            ops::shift_times(
                d,
                &Rows::From(n / 2),
                amount,
                which,
                Some(&["Default".into()]),
            );
        });
    }

    let keyframes = [0, 3, 10, 11, 250, 100_000];
    for (i, f) in [&fr, &vfr].into_iter().enumerate() {
        let pp = PostProcess {
            lead_in: Some(Time(120)),
            lead_out: Some(Time(-50)),
            adjacent_threshold: Some(Time(300)),
            adjacent_overlap: Some(Time(100)),
            adjacent_bias: i as f64,
            keyframes: Some((&keyframes, f)),
            kf_before_start: Time(200),
            kf_after_start: Time(300),
            kf_before_end: Time(400),
            kf_after_end: Time(500),
            styles: Vec::new(),
        };
        history.edit("post-process", |d| {
            ops::post_process(d, &Rows::All, &pp);
            ops::post_process(d, &Rows::Selected(vec![0, n / 2, n + 5]), &pp);
        });
    }

    for (ar_mode, to) in [
        (ArMode::Stretch, (0, 0)),
        (ArMode::AddBorder, (1920, 1080)),
        (ArMode::RemoveBorder, (640, 1080)),
        (ArMode::Manual, (i32::MAX, 1)),
    ] {
        let r = Resample {
            from: (1280, 720),
            to,
            margin: [10, -2000, 3, i32::MIN / 2],
            ar_mode,
            matrix: Some(("TV.601".into(), "PC.709".into())),
        };
        history.edit("resample", |d| ops::resample(d, &r));
    }

    let targets = MoveTargets {
        drawing: true,
        ..MoveTargets::DEFAULT
    };
    history.edit("move", |d| {
        ops::translate_positions(d, &Rows::All, -1e6, 0.125, targets)
    });

    for (pat, is_regex) in [("a", false), (r"(\w)(\W)?", true), ("", true)] {
        for (field, strip_tags) in [
            (Field::Text, true),
            (Field::Text, false),
            (Field::Style, false),
            (Field::Actor, false),
            (Field::Effect, false),
        ] {
            let mut matcher = TextMatcher::new(pat, is_regex, false, field).unwrap();
            matcher.strip_tags = strip_tags;
            let filter = Filter {
                text: Some(matcher.clone()),
                only_overlapping: strip_tags,
                min_cps: Some(1.0),
                invert: is_regex,
                ..Filter::default()
            };
            let _ = filter.apply(&history.doc);
            let r = Replace {
                matcher,
                replacement: r"$2{\1}".into(),
                first_only: strip_tags,
                case: CaseRule::Follow,
            };
            history.edit("replace", |d| ops::replace(d, &Rows::All, &r));
        }
    }

    history.edit("rows", |d| {
        ops::new_end_times(d, 60);
        let copies = ops::duplicate(d, &[0, n / 2, n + 1]);
        ops::set_comment(d, &copies, None);
        ops::swap_rows(d, 0, n);
        ops::swap_rows(d, n / 2, n * 4);
        ops::strip_all_tags(d, &Rows::From(n / 2));
        ops::strip_comments(d, &Rows::All);
        ops::delete_rows(d, &copies);
    });

    history.edit("translation", |d| {
        tlmode::enable(d);
        let rows = tlmode::rows(d);
        if let Some(&row) = rows.get(rows.len() / 2) {
            let row = tlmode::set_translation(d, row, "tłumaczenie {\\i1}x");
            let row = tlmode::set_doubtful(d, row, true);
            let _ = tlmode::original_text(d, row);
            let _ = tlmode::translation_text(d, row);
        }
        let _ = tlmode::next_doubtful(d, 0);
        let _ = tlmode::next_untranslated(d, n);
        let _ = tlmode::paste_translation(d, n / 3, &["a", "", "b\\Nc"]);
        for op in [
            tlmode::Shift::InsertTranslation,
            tlmode::Shift::JoinTranslation,
            tlmode::Shift::DeleteTranslation,
            tlmode::Shift::InsertOriginal,
            tlmode::Shift::JoinOriginal,
            tlmode::Shift::DeleteOriginal,
        ] {
            tlmode::shift(d, n / 4, 3, op);
            tlmode::shift(d, tlmode::rows(d).len().saturating_sub(1), 9, op);
        }
        let _ = tlmode::translation_only(d);
    });

    while history.undo().is_some() {}
    assert_eq!(history.doc.to_ass_string(), doc.to_ass_string());
    while history.redo().is_some() {}
    let _ = (history.undo_memory(), history.is_modified());

    let _ = export::pieces(doc);
    for f in [&fr, &vfr] {
        let _ = export::write_encore(doc, f);
        let _ = export::write_transtation(doc, f);
    }
    let ttxt = export::write_ttxt(doc, Time(3000));
    assert!(export::parse_ttxt(&ttxt).is_some());
    for (tv_standard, line_wrapping, max_line_length) in
        [(0, 0, 0), (4, 1, 1), (2, 2, 42), (5, 3, 9)]
    {
        let s = export::EbuSettings {
            tv_standard,
            line_wrapping,
            max_line_length,
            text_encoding: tv_standard % 5,
            ..export::EbuSettings::default()
        };
        let _ = export::write_ebu_stl(doc, &s);
    }
    let _ = fs_formats::write_plain_text(doc);
    let _ = doc.attachments();
    let _ = doc.embedded_fonts();
}

/// `tools`: the slower editing tools too (once per input, in the format it is detected as).
fn exercise_doc(mut doc: Document, tools: bool) {
    let _ = doc.to_ass_string();
    if tools {
        exercise_doc_tools(&doc);
    }
    let n = doc.event_count();
    for i in 0..n.min(40) {
        let e = doc.event(i).unwrap().clone();
        exercise_text(&e.text);
        if tools && i < 3 {
            exercise_text_tools(&e.text);
        }
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
    let _ = fs_formats::export::parse_ttxt(&String::from_utf8_lossy(bytes));
    let _ = fs_formats::load_plain_text(bytes, &LoadOptions::default());
    let forced = [
        None,
        Some(Format::Ass),
        Some(Format::Srt),
        Some(Format::Mpl2),
        Some(Format::MicroDvd),
        Some(Format::Tmp),
        Some(Format::Ttxt),
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
            Format::Ttxt,
        ] {
            let _ = save_bytes(&l.doc, f, &SaveOptions::default());
        }
        exercise_doc(l.doc, format.is_none());
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
