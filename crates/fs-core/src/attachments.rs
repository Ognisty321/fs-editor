//! Embedded fonts and graphics (`[Fonts]` / `[Graphics]`), uuencoded the way
//! Aegisub and VSFilter do: 6-bit groups + 33, 80 characters per line.

use std::ops::Range;
use std::sync::Arc;

use crate::ass::{Document, Section, SectionBody, SectionKind, fix_last_ending};
use crate::text::RawLine;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Attachment {
    pub font: bool,
    /// As stored (fonts carry SSA's `_0` before `.ttf`).
    pub name: String,
    /// Decoded size in bytes.
    pub size: usize,
}

pub fn uuencode(data: &[u8]) -> Vec<String> {
    let mut out = Vec::with_capacity(data.len() * 4 / 3 + 4);
    for chunk in data.chunks(3) {
        let s = [
            chunk[0],
            *chunk.get(1).unwrap_or(&0),
            *chunk.get(2).unwrap_or(&0),
        ];
        let d = [
            s[0] >> 2,
            ((s[0] & 0x3) << 4) | (s[1] >> 4),
            ((s[1] & 0xF) << 2) | (s[2] >> 6),
            s[2] & 0x3F,
        ];
        // a short last group keeps one character more than its bytes
        out.extend(d[..chunk.len() + 1].iter().map(|c| c + 33));
    }
    out.chunks(80)
        .map(|l| String::from_utf8_lossy(l).into_owned())
        .collect()
}

pub fn uudecode<'a>(lines: impl IntoIterator<Item = &'a str>) -> Vec<u8> {
    let chars: Vec<u8> = lines
        .into_iter()
        .flat_map(|l| l.trim().bytes())
        .map(|c| c.wrapping_sub(33) & 0x3F)
        .collect();
    let mut out = Vec::with_capacity(chars.len() * 3 / 4);
    for g in chars.chunks(4) {
        let s = [
            g[0],
            *g.get(1).unwrap_or(&0),
            *g.get(2).unwrap_or(&0),
            *g.get(3).unwrap_or(&0),
        ];
        let bytes = [
            (s[0] << 2) | (s[1] >> 4),
            (s[1] << 4) | (s[2] >> 2),
            (s[2] << 6) | s[3],
        ];
        out.extend_from_slice(&bytes[..g.len().saturating_sub(1)]);
    }
    out
}

fn kind(font: bool) -> SectionKind {
    if font {
        SectionKind::Fonts
    } else {
        SectionKind::Graphics
    }
}

fn header_name(line: &str, font: bool) -> Option<&str> {
    let key = if font { "fontname:" } else { "filename:" };
    let l = line.trim_start();
    l.get(..key.len())
        .filter(|k| k.eq_ignore_ascii_case(key))
        .map(|_| l[key.len()..].trim())
}

/// `(name, data line range)` of each entry in a section's raw lines.
fn entries(lines: &[RawLine], font: bool) -> Vec<(String, Range<usize>)> {
    let mut out: Vec<(String, Range<usize>)> = Vec::new();
    for (i, l) in lines.iter().enumerate() {
        if let Some(n) = header_name(&l.text, font) {
            out.push((n.to_string(), i + 1..i + 1));
        } else if let Some(last) = out.last_mut()
            && last.1.end == i
            && !l.text.trim().is_empty()
        {
            last.1.end = i + 1;
        }
    }
    out
}

fn raw(doc: &Document, font: bool) -> Option<&Vec<RawLine>> {
    let i = doc.find_section(kind(font))?;
    match &doc.sections[i].body {
        SectionBody::Raw(l) => Some(l),
        _ => None,
    }
}

fn decode(lines: &[RawLine], r: Range<usize>) -> Vec<u8> {
    uudecode(lines[r].iter().map(|l| l.text.as_str()))
}

impl Document {
    pub fn attachments(&self) -> Vec<Attachment> {
        let mut out = Vec::new();
        for font in [true, false] {
            if let Some(lines) = raw(self, font) {
                for (name, r) in entries(lines, font) {
                    let size = decode(lines, r).len();
                    out.push(Attachment { font, name, size });
                }
            }
        }
        out
    }

    pub fn attachment_data(&self, font: bool, name: &str) -> Option<Vec<u8>> {
        let lines = raw(self, font)?;
        let (_, r) = entries(lines, font).into_iter().find(|(n, _)| n == name)?;
        Some(decode(lines, r))
    }

    /// Embed a file; fonts get SSA's `_0` suffix like Aegisub. Returns the stored name.
    pub fn attach(&mut self, font: bool, file_name: &str, data: &[u8]) -> String {
        let mut name = file_name.to_string();
        if font && name.to_ascii_lowercase().ends_with(".ttf") {
            name.insert_str(name.len() - 4, "_0");
        }
        let le = self.default_ending;
        let i = match self.find_section(kind(font)) {
            Some(i) => i,
            None => {
                // Aegisub writes attachments between the styles and the events
                let pos = self
                    .find_section(SectionKind::Events)
                    .unwrap_or(self.sections.len());
                let header = if font { "[Fonts]" } else { "[Graphics]" };
                self.insert_section_with_spacing(
                    pos,
                    Section {
                        kind: kind(font),
                        header: RawLine::new(header, le),
                        body: SectionBody::Raw(Vec::new()),
                    },
                );
                pos
            }
        };
        if let SectionBody::Raw(lines) = &mut Arc::make_mut(&mut self.sections[i]).body {
            // before the blank lines that separate it from the next section
            let at = lines
                .iter()
                .rposition(|l| !l.text.trim().is_empty())
                .map_or(0, |p| p + 1);
            let key = if font { "fontname" } else { "filename" };
            let new = std::iter::once(format!("{key}: {name}"))
                .chain(uuencode(data))
                .map(|t| RawLine::new(t, le));
            lines.splice(at..at, new);
        }
        name
    }

    /// `other`'s attached fonts and graphics as they are, for those this has none of (a
    /// conversion between SSA and ASS keeps them).
    pub fn copy_attachments(&mut self, other: &Document) {
        for font in [true, false] {
            let (Some(i), None) = (
                other.find_section(kind(font)),
                self.find_section(kind(font)),
            ) else {
                continue;
            };
            let mut sec = (*other.sections[i]).clone();
            if let SectionBody::Raw(lines) = &mut sec.body {
                // the spacing is the new place's
                while lines.last().is_some_and(|l| l.text.trim().is_empty()) {
                    lines.pop();
                }
                fix_last_ending(lines, self.default_ending);
            }
            let pos = self
                .find_section(SectionKind::Events)
                .unwrap_or(self.sections.len());
            self.insert_section_with_spacing(pos, sec);
        }
    }

    pub fn remove_attachment(&mut self, font: bool, name: &str) -> bool {
        let Some(i) = self.find_section(kind(font)) else {
            return false;
        };
        let SectionBody::Raw(lines) = &mut Arc::make_mut(&mut self.sections[i]).body else {
            return false;
        };
        let Some((_, r)) = entries(lines, font).into_iter().find(|(n, _)| n == name) else {
            return false;
        };
        lines.drain(r.start - 1..r.end);
        if entries(lines, font).is_empty() {
            self.sections.remove(i);
        }
        true
    }
}

/// The file name to extract a stored attachment to (drops SSA's `_0`).
pub fn file_name(name: &str) -> String {
    let lower = name.to_ascii_lowercase();
    match lower.rfind('_') {
        Some(u) if lower.ends_with(".ttf") && u + 6 == name.len() => {
            format!("{}{}", &name[..u], &name[u + 2..])
        }
        _ => name.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uu_roundtrip_all_lengths() {
        for n in 0..300usize {
            let data: Vec<u8> = (0..n).map(|i| (i * 37 + 11) as u8).collect();
            let lines = uuencode(&data);
            assert!(lines.iter().all(|l| l.len() <= 80));
            assert_eq!(uudecode(lines.iter().map(String::as_str)), data, "len {n}");
        }
        // "Man" -> 6-bit groups 19 22 5 46, each + 33
        assert_eq!(uuencode(b"Man"), ["47&O"]);
    }

    #[test]
    fn attach_list_extract_remove() {
        let mut d = Document::parse(
            "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
        );
        let font: Vec<u8> = (0..500).map(|i| i as u8).collect();
        assert_eq!(d.attach(true, "A.ttf", &font), "A_0.ttf");
        d.attach(false, "logo.png", b"png!");
        let text = d.to_ass_string();
        assert!(text.find("[Fonts]").unwrap() < text.find("[Events]").unwrap());
        let mut d = Document::parse(&text);
        assert_eq!(
            d.attachments(),
            [
                Attachment {
                    font: true,
                    name: "A_0.ttf".into(),
                    size: 500
                },
                Attachment {
                    font: false,
                    name: "logo.png".into(),
                    size: 4
                },
            ]
        );
        assert_eq!(d.attachment_data(true, "A_0.ttf").unwrap(), font);
        assert_eq!(file_name("A_0.ttf"), "A.ttf");
        assert!(d.remove_attachment(true, "A_0.ttf"));
        assert!(!d.to_ass_string().contains("[Fonts]"));
        assert_eq!(d.attachments().len(), 1);
    }
}
