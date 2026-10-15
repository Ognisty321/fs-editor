//! Subtitle file formats: loading and saving ASS/SSA, SRT, MPL2, MicroDVD and
//! TMP with encoding detection.

pub mod encoding;
pub mod markup;
pub mod simple;

use std::path::Path;

use encoding_rs::{Encoding, UTF_8};
use fs_core::model::AssVersion;
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
        .map(|l| l.trim().trim_start_matches('\u{feff}'))
        .filter(|l| !l.is_empty())
        .take(20)
        .collect();
    // a comment may come before [Script Info]
    if sample
        .iter()
        .any(|l| l.eq_ignore_ascii_case("[script info]") || l.eq_ignore_ascii_case("[events]"))
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
    let is_tmp = |l: &&str| simple::tmp_line(l).is_some();
    let count = |f: &dyn Fn(&&str) -> bool| sample.iter().filter(|l| f(l)).count();
    let (m, p, t) = (count(&is_mdvd), count(&is_mpl), count(&is_tmp));
    let best = [(m, Format::MicroDvd), (p, Format::Mpl2), (t, Format::Tmp)]
        .into_iter()
        .max_by_key(|x| x.0)?;
    if best.0 > 0 {
        return Some(best.1);
    }
    // the name alone only for an empty file: text in none of the formats is not opened as one, to
    // be emptied by the next save
    if !sample.is_empty() {
        return None;
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
    /// How plain text is read (Aegisub's "Text import options").
    pub text: TextImport,
}

impl Default for LoadOptions {
    fn default() -> Self {
        LoadOptions {
            encoding: None,
            format: None,
            fps: Framerate::from_rational(24000, 1001, true).unwrap(),
            tmp_max_duration: Time(5000),
            text: TextImport::default(),
        }
    }
}

#[derive(Debug, Clone)]
pub struct TextImport {
    /// Text up to it names the actor; empty reads no actors.
    pub actor_separator: String,
    /// A line starting with it is a comment; empty makes none.
    pub comment_starter: String,
    /// Empty lines become empty comments instead of being left out.
    pub include_blank: bool,
}

impl Default for TextImport {
    fn default() -> Self {
        TextImport {
            actor_separator: ":".into(),
            comment_starter: "#".into(),
            include_blank: false,
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
    // lines that looked like the format but none read: an empty grid would empty the file on save
    if !format.is_ass()
        && doc.event_count() == 0
        && !text.trim_start_matches('\u{feff}').trim().is_empty()
    {
        return Err(Error::Empty);
    }
    Ok(Loaded {
        doc,
        format,
        encoding: dec.encoding,
        bom: dec.bom,
        declared_fps,
        warnings,
    })
}

/// Aegisub's plain text import: an event per line, timed 0; "Actor: text" names the actor (kept
/// for the indented lines after it), a line starting with the comment starter or left empty is a
/// comment. The separators come from `opts.text`.
pub fn load_plain_text(bytes: &[u8], opts: &LoadOptions) -> Result<Loaded, Error> {
    let (text, dec) = encoding::decode(bytes, opts.encoding);
    if text.starts_with("# timecode") {
        return Err(Error::UnknownFormat); // a timecodes file, not subtitles
    }
    let TextImport {
        actor_separator: sep,
        comment_starter: rem,
        include_blank,
    } = &opts.text;
    let mut doc = Document::new();
    doc.remove_event(0);
    let mut actor = "";
    for line in text.lines().filter(|l| *include_blank || !l.is_empty()) {
        let (mut value, comment) = match line.strip_prefix(rem.as_str()) {
            Some(v) if !rem.is_empty() => (v, true),
            _ => (line, false),
        };
        // Aegisub drops one byte of a longer separator; the whole one goes here
        if !comment
            && !sep.is_empty()
            && !value.starts_with([' ', '\t'])
            && let Some((a, rest)) = value.split_once(sep.as_str())
        {
            actor = a.trim();
            value = rest;
        }
        let value = value.trim_start();
        let mut ev = Event::dialogue(Time(0), Time(0), "Default", value);
        if comment || value.is_empty() {
            ev.kind = EventKind::Comment;
        } else {
            ev.actor = actor.to_string();
        }
        doc.push_event(ev);
    }
    Ok(Loaded {
        doc,
        format: Format::Ass,
        encoding: dec.encoding,
        bom: dec.bom,
        declared_fps: None,
        warnings: Vec::new(),
    })
}

/// Aegisub's plain text export: "Actor: text" when most dialogue lines name one, comments as
/// "# text", without tags and drawings; lines left with no text are skipped.
pub fn write_plain_text(doc: &Document) -> String {
    let events: Vec<&Event> = (0..doc.event_count())
        .filter_map(|i| doc.event(i))
        .collect();
    let dialogue = events.iter().filter(|e| e.kind != EventKind::Comment);
    let named = dialogue.clone().filter(|e| !e.actor.is_empty()).count();
    let actors = named > dialogue.count() / 2;
    let mut out = String::new();
    for e in events {
        let text = fs_core::tags::visible_text(&e.text);
        if text.is_empty() {
            continue;
        }
        if e.kind == EventKind::Comment {
            out += "# ";
        }
        if actors {
            out += &e.actor;
            out += ": ";
        }
        out += &text;
        out += "\r\n";
    }
    out
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
/// (comments and lines of only tags or drawings are skipped; empty ones, timed before they are
/// typed, are kept as Aegisub keeps them).
pub fn to_subs(doc: &Document) -> Vec<Sub> {
    let mut v: Vec<Sub> = doc
        .events()
        .iter()
        .map(|e| &e.event)
        .filter(|e| {
            e.kind == EventKind::Dialogue && (e.text.is_empty() || has_visible_text(&e.text))
        })
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
    if !text.contains('{') {
        // no blocks to parse: anything but spaces and the \N, \n, \h escapes shows
        let mut it = text.chars().peekable();
        while let Some(c) = it.next() {
            if c == '\\' && it.next_if(|n| matches!(n, 'N' | 'n' | 'h')).is_some() {
                continue;
            }
            if !c.is_whitespace() {
                return true;
            }
        }
        return false;
    }
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
    encoding::encode(&save_text(doc, format, opts), opts.encoding, opts.bom)
}

fn save_text(doc: &Document, format: Format, opts: &SaveOptions) -> String {
    match format {
        // an SSA script saved as ASS becomes one, and the other way (Aegisub)
        Format::Ass if doc.version() == AssVersion::Ssa => {
            convert(doc, AssVersion::Ass).to_ass_string()
        }
        Format::Ssa if doc.version() != AssVersion::Ssa => {
            convert(doc, AssVersion::Ssa).to_ass_string()
        }
        Format::Ass | Format::Ssa => doc.to_ass_string(),
        Format::Srt => simple::write_srt(&to_subs(doc)),
        Format::Mpl2 => simple::write_mpl2(&to_subs(doc)),
        Format::MicroDvd => {
            simple::write_microdvd(&to_subs(doc), &opts.fps, opts.microdvd_fps_header)
        }
        Format::Tmp => simple::write_tmp(&to_subs(doc)),
    }
}

/// Whether `format` holds all of `doc` (Aegisub's CanSave): lines of the one style with no
/// actor, effect, margins or layer, tags the format can say, no attachments, and the text it
/// writes reads back the same. Save goes to an ASS name otherwise.
pub fn lossless(doc: &Document, format: Format, opts: &SaveOptions) -> bool {
    use fs_core::tags::{Block, Line};
    if matches!(format, Format::Ass | Format::Ssa) {
        return true;
    }
    // the ones `markup` converts
    const KNOWN: [&str; 10] = ["i", "b", "u", "s", "c", "1c", "an", "a", "fn", "fs"];
    let plain = |e: &Event| {
        e.kind == EventKind::Dialogue
            && e.style == "Default"
            && e.actor.is_empty()
            && e.effect.is_empty()
            && (e.layer, e.margin_l, e.margin_r, e.margin_v) == (0, 0, 0, 0)
            && (!e.text.contains('{')
                || Line::parse(&e.text).blocks.iter().all(|b| match b {
                    Block::Override { tags, .. } => {
                        tags.iter().all(|t| KNOWN.contains(&t.name.as_str()))
                    }
                    Block::Text(_) => true,
                    Block::Drawing { .. } | Block::Comment(_) => false,
                }))
    };
    let mut evs: Vec<&Event> = doc.events().iter().map(|e| &e.event).collect();
    if !doc.attachments().is_empty() || !evs.iter().all(|e| plain(e)) {
        return false;
    }
    // what the format cannot say (italics in part of an MPL2 line …) does not come back
    let back = read_back(doc, format, opts);
    let canon = |t: &str| {
        let (runs, an) = markup::ass_to_runs(t);
        markup::runs_to_ass(&runs, an)
    };
    evs.sort_by_key(|e| e.start);
    evs.len() == back.len()
        && evs
            .iter()
            .zip(&back)
            .all(|(e, b)| e.text == b.text || canon(&e.text) == canon(&b.text))
}

/// The lines a save of `doc` in the simple `format` writes, read back.
fn read_back(doc: &Document, format: Format, opts: &SaveOptions) -> Vec<Sub> {
    let text = save_text(doc, format, opts);
    match format {
        Format::Srt => simple::parse_srt(&text),
        Format::Mpl2 => simple::parse_mpl2(&text),
        Format::MicroDvd => simple::parse_microdvd(&text, &opts.fps).0,
        _ => simple::parse_tmp(&text, LoadOptions::default().tmp_max_duration),
    }
}

/// `doc` cut down to what `format` holds (Kainote's conversion): the lines a save writes, read
/// back, in place of its events, and no attachments. ASS and SSA hold it all.
pub fn reduce(doc: &mut Document, format: Format, opts: &SaveOptions) {
    if format.is_ass() {
        return;
    }
    let subs = read_back(doc, format, opts);
    doc.retain_events(|_| false);
    for s in subs {
        doc.push_event(Event::dialogue(s.start, s.end, "Default", &s.text));
    }
    for a in doc.attachments() {
        doc.remove_attachment(a.font, &a.name);
    }
}

/// The encoding and BOM it was written with: UTF-8 with a BOM, the one Aegisub always saves in,
/// when `opts.encoding` lacks a character of the text.
pub fn save_file(
    doc: &Document,
    path: &Path,
    format: Format,
    opts: &SaveOptions,
) -> Result<(&'static Encoding, bool), Error> {
    let text = save_text(doc, format, opts);
    let (enc, bom) = if encoding::holds(&text, opts.encoding) {
        (opts.encoding, opts.bom)
    } else {
        (UTF_8, true)
    };
    let bytes = encoding::encode(&text, enc, bom);
    // write atomically: temp file + rename
    let tmp = path.with_extension(format!(
        "{}.fs-tmp",
        path.extension().and_then(|e| e.to_str()).unwrap_or("")
    ));
    std::fs::write(&tmp, &bytes)?;
    std::fs::rename(&tmp, path)?;
    Ok((enc, bom))
}

/// `doc` in the sections of `version`: SSA's `[V4 Styles]` and Format lines or ASS's (legacy
/// alignment is the writer's, from the version), the attachments as they are.
pub fn convert(doc: &Document, version: AssVersion) -> Document {
    use fs_core::model::{EventField, StyleField};
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
    out.set_info("ScriptType", version.script_type());
    // creating the styles section uses the document version
    {
        let st = out.ensure_styles();
        st.version = version;
        st.format.as_mut().unwrap().fields = StyleField::default_format(version);
    }
    let le = out.default_ending;
    if let Some(sec) = out
        .sections
        .iter_mut()
        .find(|s| s.kind == fs_core::SectionKind::Styles)
    {
        std::sync::Arc::make_mut(sec).header =
            fs_core::text::RawLine::new(version.styles_header(), le);
    }
    for s in doc.styles() {
        out.add_style(s.clone());
    }
    out.ensure_events().format.as_mut().unwrap().fields = EventField::default_format(version);
    for e in doc.events() {
        out.push_event(e.event.clone());
    }
    out.copy_attachments(doc);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_text_import() {
        let text = "Anna: Hi there\n  and more\n\n# a note\nBob:   Yes: sure\n:\n";
        let l = load_plain_text(text.as_bytes(), &LoadOptions::default()).unwrap();
        let d = &l.doc;
        let evs: Vec<_> = (0..d.event_count())
            .map(|i| d.event(i).unwrap())
            .map(|e| {
                (
                    e.kind == EventKind::Comment,
                    e.actor.as_str(),
                    e.text.as_str(),
                )
            })
            .collect();
        assert_eq!(
            evs,
            [
                (false, "Anna", "Hi there"),
                (false, "Anna", "and more"), // indented: the actor goes on
                (true, "", "a note"),
                (false, "Bob", "Yes: sure"),
                (true, "", ""),
            ]
        );
        assert!(load_plain_text(b"# timecode format v2\n0\n", &LoadOptions::default()).is_err());
        // other separators, blank lines kept as empty comments
        let opts = LoadOptions {
            text: TextImport {
                actor_separator: " - ".into(),
                comment_starter: "//".into(),
                include_blank: true,
            },
            ..LoadOptions::default()
        };
        let o = load_plain_text("Ann - Hi: you\n\n//note\n#not\n".as_bytes(), &opts).unwrap();
        let rows: Vec<_> = (0..o.doc.event_count())
            .map(|i| o.doc.event(i).unwrap())
            .map(|e| {
                (
                    e.kind == EventKind::Comment,
                    e.actor.as_str(),
                    e.text.as_str(),
                )
            })
            .collect();
        assert_eq!(
            rows,
            [
                (false, "Ann", "Hi: you"),
                (true, "", ""),
                (true, "", "note"),
                (false, "Ann", "#not"),
            ]
        );
        // no separators: neither actors nor comments
        let none = LoadOptions {
            text: TextImport {
                actor_separator: String::new(),
                comment_starter: String::new(),
                include_blank: false,
            },
            ..LoadOptions::default()
        };
        let n = load_plain_text(b"A: b\n# c\n", &none).unwrap();
        assert_eq!(n.doc.event(0).unwrap().text, "A: b");
        assert_eq!(n.doc.event(1).unwrap().kind, EventKind::Dialogue);
        // and back, as Aegisub writes it (an actor column on every line, empty lines left out)
        assert_eq!(
            write_plain_text(d),
            "Anna: Hi there\r\nAnna: and more\r\n# : a note\r\nBob: Yes: sure\r\n"
        );
    }

    #[test]
    fn save_switches_to_utf8_for_what_the_encoding_lacks() {
        let dir = std::env::temp_dir().join(format!("fsed-enc-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("a.srt");
        let cp1250 = encoding::by_name("cp1250").unwrap();
        let opts = SaveOptions {
            encoding: cp1250,
            bom: false,
            ..Default::default()
        };
        let doc = |t: &str| {
            from_subs(vec![Sub {
                start: Time(0),
                end: Time(1000),
                text: t.into(),
            }])
        };
        assert_eq!(
            save_file(&doc("zażółć"), &p, Format::Srt, &opts).unwrap(),
            (cp1250, false)
        );
        assert_eq!(
            std::fs::read(&p).unwrap()[34..],
            *b"za\xbf\xf3\xb3\xe6\r\n\r\n"
        );
        assert_eq!(
            save_file(&doc("zażółć ★"), &p, Format::Srt, &opts).unwrap(),
            (UTF_8, true)
        );
        let l = load_file(&p, &LoadOptions::default()).unwrap();
        assert_eq!(l.doc.event(0).unwrap().text, "zażółć ★");
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn lossless_formats() {
        let opts = SaveOptions::default();
        let doc = |lines: &[&str]| {
            from_subs(
                lines
                    .iter()
                    .enumerate()
                    .map(|(k, t)| Sub {
                        start: Time(k as i64 * 1000),
                        end: Time(k as i64 * 1000 + 500),
                        text: t.to_string(),
                    })
                    .collect(),
            )
        };
        let srt = |lines: &[&str]| lossless(&doc(lines), Format::Srt, &opts);
        assert!(srt(&[r"{\an8}{\i1}a{\i0}\Nb", "", r"{\c&H0000FF&}red"]));
        assert!(!srt(&[r"{\pos(1,2)}a"]));
        assert!(!srt(&[r"a{note}"]));
        assert!(!srt(&[r"{\p1}m 0 0 l 1 1{\p0}"]));
        let mut d = doc(&["a", "b"]);
        d.event_mut(1).unwrap().actor = "Ann".into();
        assert!(!lossless(&d, Format::Srt, &opts));
        d.event_mut(1).unwrap().actor.clear();
        d.event_mut(1).unwrap().kind = EventKind::Comment;
        assert!(!lossless(&d, Format::Srt, &opts));
        assert!(lossless(&d, Format::Ass, &opts));
        // italics: of whole lines in MPL2, anywhere in SRT
        let part = doc(&[r"a {\i1}b"]);
        assert!(!lossless(&part, Format::Mpl2, &opts));
        assert!(lossless(&part, Format::Srt, &opts));
        assert!(lossless(&doc(&[r"{\i1}a\N{\i0}b"]), Format::Mpl2, &opts));

        // a conversion leaves what the format holds
        let mut d = doc(&[r"{\pos(1,2)\i1}a", "b", "c"]);
        d.event_mut(1).unwrap().kind = EventKind::Comment;
        d.event_mut(2).unwrap().style = "Signs".into();
        d.attach(true, "f.ttf", b"font");
        reduce(&mut d, Format::Srt, &opts);
        assert!(lossless(&d, Format::Srt, &opts) && d.attachments().is_empty());
        let texts: Vec<_> = d.events().iter().map(|e| e.event.text.as_str()).collect();
        assert_eq!(texts, [r"{\i1}a", "c"]);
    }

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
        assert_eq!(
            detect_format("; made by hand\n\u{feff}[Script Info]\n", Some("txt")),
            Some(Format::Ass)
        );
        // the name only for an empty file
        assert_eq!(detect_format("hello", Some("srt")), None);
        assert_eq!(detect_format("\u{feff}\n", Some("srt")), Some(Format::Srt));
        let opts = LoadOptions::default();
        assert!(matches!(
            load_bytes(b"1\n00:00:01 --> later\nx\n", Some("srt"), &opts),
            Err(Error::Empty)
        ));
        assert!(matches!(
            load_bytes(b"hello", Some("ass"), &opts),
            Err(Error::UnknownFormat)
        ));
        assert_eq!(
            load_bytes(b"", Some("srt"), &opts)
                .unwrap()
                .doc
                .event_count(),
            0
        );
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

    #[test]
    fn ssa_and_ass_keep_attachments_and_upgrade() {
        let mut doc = Document::new();
        doc.attach(true, "a.ttf", b"font bytes");
        doc.attach(false, "b.png", b"png");
        let opts = SaveOptions {
            bom: false,
            ..Default::default()
        };
        let save = |d: &Document, f| String::from_utf8(save_bytes(d, f, &opts)).unwrap();
        let ssa = save(&doc, Format::Ssa);
        let ssa_doc = load_bytes(ssa.as_bytes(), None, &LoadOptions::default())
            .unwrap()
            .doc;
        assert_eq!(ssa_doc.attachments(), doc.attachments());
        assert_eq!(
            ssa_doc.attachment_data(true, "a_0.ttf").unwrap(),
            b"font bytes"
        );
        // as it is when it stays SSA
        assert_eq!(save(&ssa_doc, Format::Ssa), ssa);
        // and ASS again when saved as ASS
        let ass = save(&ssa_doc, Format::Ass);
        assert!(ass.contains("[V4+ Styles]") && ass.contains("ScriptType: v4.00+"));
        let back = load_bytes(ass.as_bytes(), None, &LoadOptions::default()).unwrap();
        assert_eq!(back.format, Format::Ass);
        assert_eq!(back.doc.style("Default"), doc.style("Default"));
        assert_eq!(back.doc.event(0), doc.event(0));
        assert_eq!(back.doc.attachments(), doc.attachments());
        assert!(
            !ass.contains("\n\n\n") && !ass.contains("\r\n\r\n\r\n"),
            "{ass}"
        );
    }
}
