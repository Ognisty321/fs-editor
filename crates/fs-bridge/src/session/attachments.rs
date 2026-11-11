//! Attachments of the script and the fonts collector.

use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::path::Path;
use std::sync::OnceLock;

use super::Session;

/// What [`collect_fonts`] looks for: the script's faces and the characters drawn with each.
pub struct FontUses(BTreeMap<fs_core::fonts::FontUse, BTreeSet<char>>);

impl FontUses {
    /// Another script's faces too (Kainote collects all open tabs at once).
    pub fn add(&mut self, other: &FontUses) {
        for (face, chars) in &other.0 {
            self.0.entry(face.clone()).or_default().extend(chars);
        }
    }
}

impl Session {
    pub fn font_uses(&self) -> Box<FontUses> {
        Box::new(FontUses(fs_core::fonts::used_fonts(&self.ed.doc)))
    }

    pub fn attachments(&self) -> Vec<crate::ffi::AttachmentRow> {
        self.ed
            .doc
            .attachments()
            .into_iter()
            .map(|a| crate::ffi::AttachmentRow {
                font: a.font,
                name: a.name,
                size: a.size,
            })
            .collect()
    }

    pub fn attach_file(&mut self, font: bool, path: &str) -> Result<String, String> {
        let data = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
        let file = Path::new(path)
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let mut stored = String::new();
        self.edit("Attach file", |d| stored = d.attach(font, &file, &data));
        Ok(stored)
    }

    /// Aegisub's "Attach fonts to current subtitles": the files go into [Fonts] in one step,
    /// except those attached already. Returns how many went in.
    pub fn attach_fonts(&mut self, paths: &[String]) -> Result<usize, String> {
        let mut have: HashSet<String> = self
            .ed
            .doc
            .attachments()
            .into_iter()
            .filter(|a| a.font)
            .map(|a| fs_core::attachments::file_name(&a.name).to_lowercase())
            .collect();
        let mut files = Vec::new();
        for p in paths {
            let name = Path::new(p)
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            if !name.is_empty() && have.insert(name.to_lowercase()) {
                files.push((name, std::fs::read(p).map_err(|e| format!("{p}: {e}"))?));
            }
        }
        if !files.is_empty() {
            self.edit("Attach fonts", |d| {
                for (name, data) in &files {
                    d.attach(true, name, data);
                }
            });
        }
        Ok(files.len())
    }

    pub fn extract_attachment(&self, font: bool, name: &str, path: &str) -> Result<(), String> {
        let data = self
            .ed
            .doc
            .attachment_data(font, name)
            .ok_or_else(|| format!("no attachment {name}"))?;
        std::fs::write(path, data).map_err(|e| format!("{path}: {e}"))
    }

    /// An attachment's decoded bytes; empty when there is none.
    pub fn attachment_data(&self, font: bool, name: &str) -> Vec<u8> {
        self.ed.doc.attachment_data(font, name).unwrap_or_default()
    }

    pub fn remove_attachment(&mut self, font: bool, name: &str) {
        let name = name.to_string();
        self.edit("Delete attachment", move |d| {
            d.remove_attachment(font, &name);
        });
    }
}

/// The system's fonts, scanned once: a scan takes seconds and they rarely change while editing.
fn system_fonts() -> fontdb::Database {
    static DB: OnceLock<fontdb::Database> = OnceLock::new();
    DB.get_or_init(|| {
        let mut db = fontdb::Database::new();
        db.load_system_fonts();
        db
    })
    .clone()
}

pub fn collect_fonts(
    uses: &FontUses,
    dirs: Vec<String>,
    media: &str,
    dest: &str,
) -> Result<String, String> {
    use std::sync::atomic::{AtomicUsize, Ordering};
    let mut db = system_fonts();
    for d in &dirs {
        db.load_fonts_dir(d);
    }
    if media.is_empty() {
        return fonts_report(&db, uses, dest, None);
    }
    // a folder per run: two collectors may run at once
    static RUN: AtomicUsize = AtomicUsize::new(0);
    let tmp = std::env::temp_dir().join(format!(
        "fs-editor-fonts-{}-{}",
        std::process::id(),
        RUN.fetch_add(1, Ordering::Relaxed)
    ));
    let unpack = || -> Result<(), String> {
        std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
        for (name, data) in fs_media::attached_fonts(Path::new(media)).unwrap_or_default() {
            // attachment names come from the file: keep only the last path component
            let name = Path::new(&name)
                .file_name()
                .map(|n| n.to_owned())
                .unwrap_or_default();
            if !name.is_empty() {
                std::fs::write(tmp.join(name), data).map_err(|e| e.to_string())?;
            }
        }
        Ok(())
    };
    let report = unpack().and_then(|()| {
        db.load_fonts_dir(&tmp);
        fonts_report(&db, uses, dest, Some(&tmp))
    });
    let _ = std::fs::remove_dir_all(&tmp);
    report
}

/// One line per face; fonts found in `attached` (the unpacked video attachments) are named
/// by file, as that folder is gone afterwards.
fn fonts_report(
    db: &fontdb::Database,
    uses: &FontUses,
    dest: &str,
    attached: Option<&Path>,
) -> Result<String, String> {
    use std::fmt::Write;
    if !dest.is_empty() {
        std::fs::create_dir_all(dest).map_err(|e| format!("{dest}: {e}"))?;
    }
    let mut out = String::new();
    for (f, found) in fs_core::fonts::locate(db, &uses.0) {
        let face = format!(
            "{} ({}{})",
            f.family,
            f.weight,
            if f.italic { ", italic" } else { "" }
        );
        let Some(loc) = found else {
            let _ = writeln!(out, "MISSING  {face}");
            continue;
        };
        let tag = if loc.synthetic { "FAUX   " } else { "OK     " };
        match (attached, loc.path.file_name()) {
            (Some(a), Some(name)) if loc.path.starts_with(a) => {
                let _ = write!(out, "{tag}  {face}: {} (attached)", name.to_string_lossy());
            }
            _ => {
                let _ = write!(out, "{tag}  {face}: {}", loc.path.display());
            }
        }
        if !loc.missing_glyphs.is_empty() {
            let _ = write!(
                out,
                "  missing glyphs: {}",
                loc.missing_glyphs.iter().collect::<String>()
            );
        }
        out.push('\n');
        if let (false, Some(name)) = (dest.is_empty(), loc.path.file_name()) {
            std::fs::copy(&loc.path, std::path::Path::new(dest).join(name))
                .map_err(|e| format!("{}: {e}", loc.path.display()))?;
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn fonts_attach_once_in_one_step() {
        let dir = std::env::temp_dir().join(format!("fsb-fonts-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let paths: Vec<String> = ["A.ttf", "b.otf"]
            .iter()
            .map(|n| {
                let p = dir.join(n);
                std::fs::write(&p, b"not really a font").unwrap();
                p.to_string_lossy().into_owned()
            })
            .collect();
        let mut s = session(ASS);
        assert_eq!(s.attach_fonts(&paths), Ok(2));
        // the same files again (in another case too) add nothing
        assert_eq!(s.attach_fonts(&paths), Ok(0));
        let names: Vec<String> = s.attachments().into_iter().map(|a| a.name).collect();
        assert_eq!(names, ["A_0.ttf", "b.otf"]);
        assert!(s.undo());
        assert!(s.attachments().is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }
}
