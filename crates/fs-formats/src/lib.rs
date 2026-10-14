//! Subtitle file formats: loading and saving ASS/SSA, SRT, MPL2, MicroDVD and
//! TMP with encoding detection.

pub mod encoding;
pub mod markup;
pub mod simple;

use std::path::Path;

use encoding_rs::{Encoding, UTF_8};
use fs_core::vfr::Framerate;
use fs_core::{Document, Event, EventKind, Time};

pub use simple::Sub;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Format {
    Ass,
    Ssa,
    Srt,
    Mpl2,
    MicroDvd,
    Tmp,
}

impl Format {
    pub const ALL: [Format; 6] = [
        Format::Ass,
        Format::Ssa,
        Format::Srt,
        Format::Mpl2,
        Format::MicroDvd,
        Format::Tmp,
    ];

    pub fn name(self) -> &'static str {
        match self {
            Format::Ass => "ASS",
            Format::Ssa => "SSA",
            Format::Srt => "SRT",
            Format::Mpl2 => "MPL2",
            Format::MicroDvd => "MicroDVD",
            Format::Tmp => "TMP",
        }
    }

    pub fn extension(self) -> &'static str {
        match self {
            Format::Ass => "ass",
            Format::Ssa => "ssa",
            Format::Srt => "srt",
            Format::Mpl2 | Format::Tmp => "txt",
            Format::MicroDvd => "sub",
        }
    }

    pub fn from_name(s: &str) -> Option<Format> {
        Some(match s.trim().to_ascii_lowercase().as_str() {
            "ass" => Format::Ass,
            "ssa" => Format::Ssa,
            "srt" | "subrip" => Format::Srt,
            "mpl2" | "mpl" => Format::Mpl2,
            "microdvd" | "mdvd" | "sub" => Format::MicroDvd,
            "tmp" | "tmplayer" => Format::Tmp,
            _ => return None,
        })
    }

    /// Whether the format can hold full ASS styling.
    pub fn is_ass(self) -> bool {
        matches!(self, Format::Ass | Format::Ssa)
    }
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("could not recognise the subtitle format")]
    UnknownFormat,
    #[error("the file contains no subtitles")]
    Empty,
}

/// Detect the format from content (and the file extension as a tiebreaker).
pub fn detect_format(text: &str, ext: Option<&str>) -> Option<Format> {
    let ext = ext.map(|e| e.to_ascii_lowercase());
    let sample: Vec<&str> = text
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .take(20)
        .collect();
    let first = sample
        .first()
        .copied()
        .unwrap_or("")
        .trim_start_matches('\u{feff}');
    if first.eq_ignore_ascii_case("[script info]")
        || sample.iter().any(|l| l.eq_ignore_ascii_case("[events]"))
    {
        let ssa = sample.iter().any(|l| l.eq_ignore_ascii_case("[v4 styles]"))
            || text
                .lines()
                .any(|l| l.trim().eq_ignore_ascii_case("ScriptType: v4.00"));
        return Some(if ssa { Format::Ssa } else { Format::Ass });
    }
    if sample.iter().any(|l| l.contains("-->")) {
        return Some(Format::Srt);
    }
    let is_mdvd = |l: &&str| {
        l.starts_with('{')
            && l[1..]
                .split_once('}')
                .is_some_and(|(n, r)| n.trim().parse::<i64>().is_ok() && r.starts_with('{'))
    };
    let is_mpl = |l: &&str| {
        l.starts_with('[')
            && l[1..]
                .split_once(']')
                .is_some_and(|(n, r)| n.trim().parse::<i64>().is_ok() && r.starts_with('['))
    };
    let is_tmp = |l: &&str| {
        let mut it = l.splitn(4, [':', '=']);
        let (a, b, c, d) = (it.next(), it.next(), it.next(), it.next());
        [a, b, c].iter().all(|x| {
            x.is_some_and(|x| !x.is_empty() && x.chars().all(|c| c.is_ascii_digit() || c == ','))
        }) && d.is_some()
    };
    let count = |f: &dyn Fn(&&str) -> bool| sample.iter().filter(|l| f(l)).count();
    let (m, p, t) = (count(&is_mdvd), count(&is_mpl), count(&is_tmp));
    let best = [(m, Format::MicroDvd), (p, Format::Mpl2), (t, Format::Tmp)]
        .into_iter()
        .max_by_key(|x| x.0)?;
    if best.0 > 0 {
        return Some(best.1);
    }
    match ext.as_deref() {
        Some("ass") => Some(Format::Ass),
        Some("ssa") => Some(Format::Ssa),
        Some("srt") => Some(Format::Srt),
        _ => None,
    }
}

#[derive(Debug, Clone)]
pub struct LoadOptions {
    /// Force a specific encoding instead of detecting.
    pub encoding: Option<&'static Encoding>,
    /// Force a format instead of detecting.
    pub format: Option<Format>,
    /// Frame rate for MicroDVD files without an FPS header.
    pub fps: Framerate,
    /// Maximum line duration for TMP (which has no end times).
    pub tmp_max_duration: Time,
}

impl Default for LoadOptions {
    fn default() -> Self {
        LoadOptions {
            encoding: None,
            format: None,
            fps: Framerate::from_rational(24000, 1001, true).unwrap(),
            tmp_max_duration: Time(5000),
        }
    }
}

#[derive(Debug, Clone)]
pub struct Loaded {
    pub doc: Document,
    pub format: Format,
    pub encoding: &'static Encoding,
    pub bom: bool,
    /// FPS declared inside a MicroDVD file.
    pub declared_fps: Option<f64>,
    pub warnings: Vec<fs_core::ass::ParseWarning>,
}

pub fn load_file(path: &Path, opts: &LoadOptions) -> Result<Loaded, Error> {
    let bytes = std::fs::read(path)?;
    load_bytes(&bytes, path.extension().and_then(|e| e.to_str()), opts)
}

pub fn load_bytes(bytes: &[u8], ext: Option<&str>, opts: &LoadOptions) -> Result<Loaded, Error> {
    let (text, dec) = encoding::decode(bytes, opts.encoding);
    let format = opts
        .format
        .or_else(|| detect_format(&text, ext))
        .ok_or(Error::UnknownFormat)?;
    let mut declared_fps = None;
    let (doc, warnings) = match format {
        Format::Ass | Format::Ssa => {
            let (mut d, w) = Document::parse_with_warnings(&text);
            d.bom = dec.bom;
            (d, w)
        }
        Format::Srt => (from_subs(simple::parse_srt(&text)), Vec::new()),
        Format::Mpl2 => (from_subs(simple::parse_mpl2(&text)), Vec::new()),
        Format::MicroDvd => {
            let (subs, fps) = simple::parse_microdvd(&text, &opts.fps);
            declared_fps = fps;
            (from_subs(subs), Vec::new())
        }
        Format::Tmp => (
            from_subs(simple::parse_tmp(&text, opts.tmp_max_duration)),
            Vec::new(),
        ),
    };
    Ok(Loaded {
        doc,
        format,
        encoding: dec.encoding,
        bom: dec.bom,
        declared_fps,
        warnings,
    })
}

/// Build an ASS document (default style, 1920×1080) from simple subtitles.
pub fn from_subs(subs: Vec<Sub>) -> Document {
    let mut doc = Document::new();
    doc.remove_event(0);
    for s in subs {
        doc.push_event(Event::dialogue(s.start, s.end, "Default", &s.text));
    }
    doc
}

/// Dialogue lines of a document as simple subtitles, sorted by start time
/// (comments and empty lines are skipped).
pub fn to_subs(doc: &Document) -> Vec<Sub> {
    let mut v: Vec<Sub> = doc
        .events()
        .iter()
        .map(|e| &e.event)
        .filter(|e| e.kind == EventKind::Dialogue && has_visible_text(&e.text))
        .map(|e| Sub {
            start: e.start,
            end: e.end,
            text: e.text.clone(),
        })
        .collect();
    v.sort_by_key(|s| s.start);
    v
}

/// Whether a line has any text that survives conversion to a simple format
/// (drawings and tag-only lines do not).
fn has_visible_text(text: &str) -> bool {
    markup::ass_to_runs(text)
        .0
        .iter()
        .any(|r| !r.text.trim().is_empty())
}

#[derive(Debug, Clone)]
pub struct SaveOptions {
    pub encoding: &'static Encoding,
    pub bom: bool,
    /// Frame rate for MicroDVD output.
    pub fps: Framerate,
    /// Write the `{1}{1}fps` header in MicroDVD.
    pub microdvd_fps_header: bool,
}

impl Default for SaveOptions {
    fn default() -> Self {
        SaveOptions {
            encoding: UTF_8,
            bom: true,
            fps: Framerate::from_rational(24000, 1001, true).unwrap(),
            microdvd_fps_header: true,
        }
    }
}

/// Serialise a document in the given format.
pub fn save_bytes(doc: &Document, format: Format, opts: &SaveOptions) -> Vec<u8> {
    let text = match format {
        Format::Ass => doc.to_ass_string(),
        Format::Ssa => to_ssa(doc).to_ass_string(),
        Format::Srt => simple::write_srt(&to_subs(doc)),
        Format::Mpl2 => simple::write_mpl2(&to_subs(doc)),
        Format::MicroDvd => {
            simple::write_microdvd(&to_subs(doc), &opts.fps, opts.microdvd_fps_header)
        }
        Format::Tmp => simple::write_tmp(&to_subs(doc)),
    };
    encoding::encode(&text, opts.encoding, opts.bom)
}

pub fn save_file(
    doc: &Document,
    path: &Path,
    format: Format,
    opts: &SaveOptions,
) -> Result<(), Error> {
    let bytes = save_bytes(doc, format, opts);
    // write atomically: temp file + rename
    let tmp = path.with_extension(format!(
        "{}.fs-tmp",
        path.extension().and_then(|e| e.to_str()).unwrap_or("")
    ));
    std::fs::write(&tmp, &bytes)?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

/// Convert an ASS document to SSA v4 (styles section header, Format lines
/// and legacy alignment are handled by the writer from the version).
pub fn to_ssa(doc: &Document) -> Document {
    use fs_core::model::{AssVersion, EventField, StyleField};
    let mut out = Document::empty();
    out.default_ending = doc.default_ending;
    out.bom = doc.bom;
    for e in doc
        .info_section()
        .map(|s| s.entries.as_slice())
        .unwrap_or(&[])
    {
        out.set_info(&e.key, &e.value);
    }
    out.set_info("ScriptType", "v4.00");
    // creating the styles section uses the document version → SSA
    {
        let st = out.ensure_styles();
        st.version = AssVersion::Ssa;
        st.format.as_mut().unwrap().fields = StyleField::default_format(AssVersion::Ssa);
    }
    let le = out.default_ending;
    if let Some(sec) = out
        .sections
        .iter_mut()
        .find(|s| s.kind == fs_core::SectionKind::Styles)
    {
        std::sync::Arc::make_mut(sec).header = fs_core::text::RawLine::new("[V4 Styles]", le);
    }
    for s in doc.styles() {
        out.add_style(s.clone());
    }
    out.ensure_events().format.as_mut().unwrap().fields =
        EventField::default_format(AssVersion::Ssa);
    for e in doc.events() {
        out.push_event(e.event.clone());
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detection() {
        assert_eq!(
            detect_format("[Script Info]\nTitle: x\n", None),
            Some(Format::Ass)
        );
        assert_eq!(
            detect_format("[Script Info]\nScriptType: v4.00\n[V4 Styles]\n", None),
            Some(Format::Ssa)
        );
        assert_eq!(
            detect_format("1\n00:00:01,000 --> 00:00:02,000\nx\n", None),
            Some(Format::Srt)
        );
        assert_eq!(
            detect_format("{0}{25}x\n{30}{50}y\n", None),
            Some(Format::MicroDvd)
        );
        assert_eq!(detect_format("[0][25]x\n", None), Some(Format::Mpl2));
        assert_eq!(
            detect_format("0:00:01:x\n00:00:05:y\n", None),
            Some(Format::Tmp)
        );
        assert_eq!(detect_format("hello", None), None);
    }

    #[test]
    fn convert_srt_to_ass_and_back() {
        let srt = "1\r\n00:00:01,000 --> 00:00:02,000\r\n<i>Zażółć</i>\r\n\r\n";
        let l = load_bytes(srt.as_bytes(), Some("srt"), &LoadOptions::default()).unwrap();
        assert_eq!(l.format, Format::Srt);
        assert_eq!(l.doc.event_count(), 1);
        assert_eq!(l.doc.event(0).unwrap().text, r"{\i1}Zażółć");
        let out = save_bytes(
            &l.doc,
            Format::Srt,
            &SaveOptions {
                bom: false,
                ..Default::default()
            },
        );
        assert_eq!(String::from_utf8(out).unwrap(), srt);
    }

    #[test]
    fn ass_to_ssa_and_back() {
        let doc = Document::new();
        let ssa = save_bytes(
            &doc,
            Format::Ssa,
            &SaveOptions {
                bom: false,
                ..Default::default()
            },
        );
        let text = String::from_utf8(ssa).unwrap();
        assert!(text.contains("[V4 Styles]"));
        assert!(text.contains("TertiaryColour"));
        assert!(text.contains("Dialogue: Marked=0,"));
        let back = load_bytes(text.as_bytes(), None, &LoadOptions::default()).unwrap();
        assert_eq!(back.format, Format::Ssa);
        assert_eq!(back.doc.style("Default"), doc.style("Default"));
        assert_eq!(back.doc.event(0), doc.event(0));
    }
}
