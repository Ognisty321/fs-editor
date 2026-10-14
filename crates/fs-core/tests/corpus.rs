//! Tests on real subtitle files.
//!
//! `tests/corpus` (committed) holds redistributable files; `tests/corpus-local`
//! (git-ignored) and `$FS_CORPUS_DIR` may hold any additional real-world
//! scripts — every `.ass`/`.ssa` found there is tested too.

use std::path::{Path, PathBuf};

use fs_core::Document;

pub fn corpus_files() -> Vec<PathBuf> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests");
    let mut dirs = vec![root.join("corpus"), root.join("corpus-local")];
    if let Ok(d) = std::env::var("FS_CORPUS_DIR") {
        dirs.push(PathBuf::from(d));
    }
    let mut out = Vec::new();
    for d in dirs {
        collect(&d, &mut out);
    }
    out.sort();
    out
}

fn collect(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return;
    };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            collect(&p, out);
        } else if p
            .extension()
            .and_then(|e| e.to_str())
            .is_some_and(|e| e.eq_ignore_ascii_case("ass") || e.eq_ignore_ascii_case("ssa"))
        {
            out.push(p);
        }
    }
}

fn load(p: &Path) -> (Vec<u8>, Document) {
    let bytes = std::fs::read(p).unwrap();
    let text = String::from_utf8(bytes.clone()).expect("corpus files are UTF-8");
    (bytes, Document::parse(&text))
}

#[test]
fn roundtrip_is_byte_exact() {
    let files = corpus_files();
    assert!(!files.is_empty(), "no corpus files found");
    let mut failures = Vec::new();
    for p in &files {
        let (bytes, doc) = load(p);
        let out = doc.to_utf8_bytes();
        if out != bytes {
            let pos = out
                .iter()
                .zip(&bytes)
                .position(|(a, b)| a != b)
                .unwrap_or(out.len().min(bytes.len()));
            failures.push(format!(
                "{}: first difference at byte {pos} (len {} vs {})",
                p.display(),
                out.len(),
                bytes.len()
            ));
        }
    }
    println!("checked {} files", files.len());
    assert!(
        failures.is_empty(),
        "roundtrip failures:\n{}",
        failures.join("\n")
    );
}

#[test]
fn parsed_counts_match_line_prefixes() {
    // Every "Dialogue:"/"Comment:" line inside [Events] must become an event,
    // every "Style:" line a style.
    for p in corpus_files() {
        let (bytes, doc) = load(&p);
        let text = String::from_utf8(bytes).unwrap();
        let mut in_events = false;
        let mut in_styles = false;
        let (mut ev, mut st) = (0usize, 0usize);
        for line in text.lines() {
            let t = line.trim();
            if t.starts_with('[') {
                in_events = t.eq_ignore_ascii_case("[Events]");
                in_styles = t.to_ascii_lowercase().contains("styles]");
                continue;
            }
            if in_events && (t.starts_with("Dialogue:") || t.starts_with("Comment:")) {
                ev += 1;
            }
            if in_styles && t.starts_with("Style:") {
                st += 1;
            }
        }
        assert_eq!(doc.event_count(), ev, "{}", p.display());
        assert_eq!(doc.style_count(), st, "{}", p.display());
    }
}

#[test]
fn modified_lines_keep_untouched_fields() {
    for p in corpus_files() {
        let (bytes, mut doc) = load(&p);
        if doc.event_count() == 0 {
            continue;
        }
        let orig_line = doc.events()[0].orig.clone().unwrap().to_string();
        doc.event_mut(0).unwrap().text.push('X');
        let out = String::from_utf8(doc.to_utf8_bytes()).unwrap();
        let expected = format!("{orig_line}X");
        assert!(
            out.contains(&expected),
            "{}: expected modified line {expected:?}",
            p.display()
        );
        // all other bytes identical: length differs by exactly one
        assert_eq!(out.len(), bytes.len() + 1, "{}", p.display());
    }
}

#[test]
fn reserialised_documents_reparse_identically() {
    // Strip all original text so everything is re-serialised canonically,
    // then check that parsing the result yields the same model.
    for p in corpus_files() {
        let (_, mut doc) = load(&p);
        for e in doc.event_entries_mut() {
            std::sync::Arc::make_mut(e).orig = None;
        }
        let canon = doc.to_ass_string();
        let doc2 = Document::parse(&canon);
        assert_eq!(doc.event_count(), doc2.event_count());
        for (a, b) in doc.events().iter().zip(doc2.events()) {
            let (mut ea, eb) = (a.event.clone(), &b.event);
            ea.start = ea.start.round_cs();
            ea.end = ea.end.round_cs();
            assert_eq!(&ea, eb, "{}", p.display());
        }
    }
}

#[test]
fn override_tags_parse_losslessly() {
    let mut n = 0usize;
    for p in corpus_files() {
        let (_, doc) = load(&p);
        for e in doc.events() {
            let line = fs_core::tags::Line::parse(&e.event.text);
            assert_eq!(line.to_text(), e.event.text, "{}", p.display());
            n += 1;
        }
    }
    println!("{n} lines");
}

#[test]
fn later_format_line_applies_to_the_following_events() {
    let src = "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n\
               Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,first\n\
               Format: Layer, Start, End, Style, Text\n\
               Dialogue: 0,0:00:03.00,0:00:04.00,Top,second, with comma\n\
               [Events]\nFormat: Start, Text\n";
    let doc = Document::parse(src);
    let ev: Vec<_> = doc.events().iter().map(|e| (e.event.style.clone(), e.event.text.clone())).collect();
    assert_eq!(ev, [("Default".into(), "first".into()), ("Top".into(), "second, with comma".into())]);
    assert_eq!(doc.to_ass_string(), src);
}

/// The one-pass insert_events/remove_events give the same document as
/// insert_event/remove_event one at a time.
#[test]
fn batch_insert_and_remove_match_single_steps() {
    let crafted = "[Events]\r\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n\
                   Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,a\r\n; note b\r\n\
                   Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,,b\r\n\r\n; note c\r\n\
                   Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,c\r\n; note d\r\n\
                   Dialogue: 0,0:00:04.00,0:00:05.00,Default,,0,0,0,,d";
    let mut docs: Vec<Document> = corpus_files().iter().map(|p| load(p).1).collect();
    docs.push(Document::parse(crafted));
    for doc in docs {
        let n = doc.event_count();
        // a few deterministic subsets: every k-th line from an offset, plus the last line
        for (k, off) in [(1, 0), (2, 0), (2, 1), (3, 2), (5, 4)] {
            let mut rows: Vec<usize> = (off..n).step_by(k).collect();
            if n > 0 && rows.last() != Some(&(n - 1)) {
                rows.push(n - 1);
            }
            let (mut a, mut b) = (doc.clone(), doc.clone());
            for &r in rows.iter().rev() {
                a.remove_event(r);
            }
            assert_eq!(b.remove_events(&rows), rows.len());
            assert_eq!(a.to_ass_string(), b.to_ass_string());

            let items: Vec<(usize, fs_core::Event)> =
                rows.iter().map(|&r| (r + 1, doc.event(r).unwrap().clone())).collect();
            let (mut a, mut b) = (doc.clone(), doc.clone());
            for (j, (r, e)) in items.iter().enumerate() {
                a.insert_event(r + j, e.clone());
            }
            let at = b.insert_events(items.clone());
            assert_eq!(at, items.iter().enumerate().map(|(j, (r, _))| r + j).collect::<Vec<_>>());
            assert_eq!(a.to_ass_string(), b.to_ass_string());
            let ids = |d: &Document| d.events().iter().map(|e| e.id).collect::<Vec<_>>();
            assert_eq!(ids(&a), ids(&b));
        }
    }
}
