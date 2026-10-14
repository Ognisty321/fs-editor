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
