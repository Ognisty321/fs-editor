//! Format tests on real files from tests/corpus and tests/corpus-local.

use std::path::{Path, PathBuf};

use fs_formats::{
    Format, LoadOptions, SaveOptions, encoding, load_bytes, load_file, save_bytes, to_subs,
};

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests")
}

fn ass_files() -> Vec<PathBuf> {
    let mut v = Vec::new();
    for d in ["corpus/aegisub", "corpus-local"] {
        if let Ok(rd) = std::fs::read_dir(root().join(d)) {
            v.extend(
                rd.flatten()
                    .map(|e| e.path())
                    .filter(|p| p.extension().is_some_and(|e| e == "ass")),
            );
        }
    }
    v.sort();
    v
}

#[test]
fn charset_samples() {
    let c = root().join("corpus/charset");
    let det = |f: &str| encoding::detect(&std::fs::read(c.join(f)).unwrap());
    assert_eq!(det("utf-8.txt").encoding, encoding_rs::UTF_8);
    assert_eq!(det("ascii.txt").encoding, encoding_rs::UTF_8);
    let b = det("utf-8-bom.txt");
    assert!(b.bom && b.encoding == encoding_rs::UTF_8);
    assert_eq!(det("utf-8-truncated.txt").encoding, encoding_rs::UTF_8);
    // Czech text: chardetng picks the Central European family
    let w = det("windows-1250.txt").encoding;
    assert!(
        w == encoding_rs::WINDOWS_1250 || w == encoding_rs::ISO_8859_2,
        "{}",
        w.name()
    );
}

#[test]
fn ass_load_save_is_byte_exact() {
    for p in ass_files() {
        let bytes = std::fs::read(&p).unwrap();
        let l = load_file(&p, &LoadOptions::default()).unwrap();
        assert_eq!(l.format, Format::Ass, "{}", p.display());
        let out = save_bytes(
            &l.doc,
            Format::Ass,
            &SaveOptions {
                encoding: l.encoding,
                bom: l.bom,
                ..Default::default()
            },
        );
        assert!(out == bytes, "{}", p.display());
    }
}

#[test]
fn convert_real_files_through_every_format() {
    for p in ass_files() {
        let l = load_file(&p, &LoadOptions::default()).unwrap();
        let expected = to_subs(&l.doc);
        for fmt in [
            Format::Srt,
            Format::Mpl2,
            Format::MicroDvd,
            Format::Tmp,
            Format::Ssa,
        ] {
            let bytes = save_bytes(&l.doc, fmt, &SaveOptions::default());
            let back = load_bytes(&bytes, Some(fmt.extension()), &LoadOptions::default())
                .unwrap_or_else(|e| panic!("{} via {}: {e}", p.display(), fmt.name()));
            assert_eq!(back.format, fmt, "{} via {}", p.display(), fmt.name());
            let got = to_subs(&back.doc);
            match fmt {
                Format::Ssa => assert_eq!(back.doc.event_count(), l.doc.event_count()),
                // TMP merges lines starting in the same second; others keep every line
                Format::Tmp => assert!(got.len() <= expected.len()),
                _ => assert_eq!(
                    got.len(),
                    expected.len(),
                    "{} via {}",
                    p.display(),
                    fmt.name()
                ),
            }
            if fmt == Format::Srt {
                for (a, b) in expected.iter().zip(&got) {
                    assert_eq!((a.start, a.end), (b.start, b.end));
                    // SRT cannot hold empty lines inside a cue
                    let norm = |t: &str| {
                        // visible text only (drawings are dropped by the conversion)
                        let (runs, _) = fs_formats::markup::ass_to_runs(t);
                        runs.iter()
                            .map(|r| r.text.as_str())
                            .collect::<String>()
                            .split('\n')
                            .filter(|l| !l.trim().is_empty())
                            .collect::<Vec<_>>()
                            .join(r"\N")
                    };
                    assert_eq!(norm(&a.text), norm(&b.text), "{}", p.display());
                }
            }
        }
    }
}
