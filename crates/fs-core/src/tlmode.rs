//! Translation mode, stored the way Kainote does so files stay interchangeable:
//!
//! * `[Script Info]` has `TLMode: Yes` (or `Translated`) and
//!   `TLMode Style: <name>` (default `TLmode`).
//! * A translated line is two consecutive events: the original (with the TL
//!   style) followed by the translation (with the real style).
//! * An untranslated line is a single event whose text is the original.
//! * A doubtful ("not confirmed") translation has `\x0CD` appended to the
//!   Effect field of the translation event.
//!
//! The functions here present that as one logical row per pair.

use crate::ass::Document;
use crate::model::Style;

pub const DEFAULT_TL_STYLE: &str = "TLmode";
pub const DOUBTFUL_MARK: &str = "\u{0C}D";

/// One logical row of the translation grid.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TlRow {
    /// Event holding the original (foreign) text.
    pub original: usize,
    /// Event holding the translation, if the line has been translated.
    pub translation: Option<usize>,
}

impl TlRow {
    /// The event carrying timing, style and other fields for this row.
    pub fn main(&self) -> usize {
        self.translation.unwrap_or(self.original)
    }
}

/// Whether the document is in translation mode, and its TL style name.
pub fn tl_style(doc: &Document) -> Option<String> {
    let mode = doc.info("TLMode")?.trim();
    if !(mode.eq_ignore_ascii_case("yes") || mode.eq_ignore_ascii_case("translated")) {
        return None;
    }
    Some(
        doc.info("TLMode Style")
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| DEFAULT_TL_STYLE.into()),
    )
}

pub fn is_tl_mode(doc: &Document) -> bool {
    tl_style(doc).is_some()
}

/// Group events into translation rows.
pub fn rows(doc: &Document) -> Vec<TlRow> {
    let Some(style) = tl_style(doc) else {
        return (0..doc.event_count())
            .map(|i| TlRow {
                original: i,
                translation: None,
            })
            .collect();
    };
    let n = doc.event_count();
    let mut out = Vec::with_capacity(n);
    let mut i = 0;
    while i < n {
        let ev = doc.event(i).unwrap();
        if ev.style == style && i + 1 < n && doc.event(i + 1).unwrap().style != style {
            out.push(TlRow {
                original: i,
                translation: Some(i + 1),
            });
            i += 2;
        } else {
            out.push(TlRow {
                original: i,
                translation: None,
            });
            i += 1;
        }
    }
    out
}

/// Turn translation mode on: write the info keys and add the TL style as a
/// copy of the first style aligned to the top (`\an8`).
pub fn enable(doc: &mut Document) {
    if is_tl_mode(doc) {
        return;
    }
    doc.set_info("TLMode", "Yes");
    doc.set_info("TLMode Style", DEFAULT_TL_STYLE);
    if doc.style(DEFAULT_TL_STYLE).is_none() {
        let mut st = doc.styles().next().cloned().unwrap_or_else(Style::default);
        st.name = DEFAULT_TL_STYLE.into();
        st.alignment = 8;
        doc.add_style(st);
    }
}

/// Turn translation mode off. The translation replaces the original where it
/// exists; original-only lines keep their text. TL keys and style are removed.
pub fn disable(doc: &mut Document) {
    let Some(style) = tl_style(doc) else { return };
    let rs = rows(doc);
    for r in rs.iter().rev() {
        if let Some(t) = r.translation {
            let ev = doc.event_mut(t).unwrap();
            ev.effect = strip_doubtful(&ev.effect).to_string();
            doc.remove_event(r.original);
        }
    }
    doc.remove_info("TLMode");
    doc.remove_info("TLMode Style");
    doc.remove_info("TLMode Showtl");
    if let Some(i) = doc.style_index(&style) {
        doc.remove_style(i);
    }
}

/// Set the translation text of a row, creating the original/translation pair
/// if needed. Returns the updated row.
pub fn set_translation(doc: &mut Document, row: TlRow, text: &str) -> TlRow {
    let style = tl_style(doc).unwrap_or_else(|| DEFAULT_TL_STYLE.into());
    match row.translation {
        Some(t) => {
            doc.event_mut(t).unwrap().text = text.to_string();
            row
        }
        None => {
            // The single event becomes the translation; insert the original
            // (same fields, TL style) before it.
            let mut orig = doc.event(row.original).unwrap().clone();
            orig.style = style;
            orig.effect = strip_doubtful(&orig.effect).to_string();
            doc.insert_event(row.original, orig);
            let t = row.original + 1;
            doc.event_mut(t).unwrap().text = text.to_string();
            TlRow {
                original: row.original,
                translation: Some(t),
            }
        }
    }
}

/// Text of the original for a row.
pub fn original_text(doc: &Document, row: TlRow) -> &str {
    &doc.event(row.original).unwrap().text
}

/// Text of the translation (empty if untranslated).
pub fn translation_text(doc: &Document, row: TlRow) -> &str {
    row.translation
        .map(|t| doc.event(t).unwrap().text.as_str())
        .unwrap_or("")
}

pub fn is_doubtful(effect: &str) -> bool {
    effect.ends_with(DOUBTFUL_MARK)
}

pub fn strip_doubtful(effect: &str) -> &str {
    effect.strip_suffix(DOUBTFUL_MARK).unwrap_or(effect)
}

/// Mark/unmark a row as doubtful (only meaningful once it has a translation
/// event; an untranslated row gets an empty translation first).
pub fn set_doubtful(doc: &mut Document, row: TlRow, doubtful: bool) -> TlRow {
    let row = if row.translation.is_none() && doubtful {
        set_translation(doc, row, "")
    } else {
        row
    };
    if let Some(t) = row.translation {
        let ev = doc.event_mut(t).unwrap();
        let base = strip_doubtful(&ev.effect).to_string();
        ev.effect = if doubtful { base + DOUBTFUL_MARK } else { base };
    }
    row
}

/// Next row (after `from`) that is doubtful.
pub fn next_doubtful(doc: &Document, from: usize) -> Option<usize> {
    let rs = rows(doc);
    (from + 1..rs.len()).find(|&i| {
        rs[i]
            .translation
            .is_some_and(|t| is_doubtful(&doc.event(t).unwrap().effect))
    })
}

/// Next row (after `from`) without a translation.
pub fn next_untranslated(doc: &Document, from: usize) -> Option<usize> {
    let rs = rows(doc);
    (from + 1..rs.len()).find(|&i| {
        rs[i]
            .translation
            .is_none_or(|t| doc.event(t).unwrap().text.trim().is_empty())
            && !doc.event(rs[i].original).unwrap().is_comment()
    })
}

/// A copy of the document containing only the translation ("Save
/// translation"): pairs collapse to their translation, untranslated lines
/// keep their original text.
pub fn translation_only(doc: &Document) -> Document {
    let mut d = doc.clone();
    disable(&mut d);
    d
}

/// Paste a list of translated lines into consecutive rows starting at
/// `first_row` (Kainote "Paste translation text"). Returns rows filled.
pub fn paste_translation(doc: &mut Document, first_row: usize, lines: &[&str]) -> usize {
    let mut filled = 0;
    for (r, l) in (first_row..).zip(lines) {
        // inserting a translation shifts event indices, so re-pair each time
        let Some(&row) = rows(doc).get(r) else { break };
        set_translation(doc, row, l);
        doc.set_info("TLMode Showtl", "Yes");
        filled += 1;
    }
    filled
}

// ---------------------------------------------------------------------------
// Kainote actor-field prefixes
// ---------------------------------------------------------------------------

pub const ACTOR_PREFIXES: &[&str] = &[
    "[bookmark]",
    "[hidden]",
    "[visible]",
    "[tree_description]",
    "[tree_opened]",
    "[tree_closed]",
];

/// Split Kainote's grid-feature prefixes from an Actor field. Returns the
/// prefixes found (in order) and the remaining real actor name.
pub fn split_actor(actor: &str) -> (Vec<&'static str>, &str) {
    let mut rest = actor;
    let mut found = Vec::new();
    'outer: loop {
        for p in ACTOR_PREFIXES {
            if let Some(r) = rest.strip_prefix(p) {
                found.push(*p);
                rest = r;
                continue 'outer;
            }
        }
        break;
    }
    (found, rest)
}

pub fn join_actor(prefixes: &[&str], actor: &str) -> String {
    let mut s: String = prefixes.concat();
    s.push_str(actor);
    s
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::time::Time;

    fn doc() -> Document {
        Document::parse(
            "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,Hello\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,World\n",
        )
    }

    #[test]
    fn enable_translate_save_reload() {
        let mut d = doc();
        enable(&mut d);
        assert_eq!(d.style(DEFAULT_TL_STYLE).unwrap().alignment, 8);
        let rs = rows(&d);
        assert_eq!(rs.len(), 2);
        let r = set_translation(&mut d, rs[1], "Świat");
        assert_eq!(
            r,
            TlRow {
                original: 1,
                translation: Some(2)
            }
        );
        set_doubtful(&mut d, r, true);
        // reload from text: pairs are recognised
        let d2 = Document::parse(&d.to_ass_string());
        let rs = rows(&d2);
        assert_eq!(rs.len(), 2);
        assert_eq!(original_text(&d2, rs[1]), "World");
        assert_eq!(translation_text(&d2, rs[1]), "Świat");
        assert!(is_doubtful(
            &d2.event(rs[1].translation.unwrap()).unwrap().effect
        ));
        assert_eq!(d2.event(1).unwrap().style, DEFAULT_TL_STYLE);
        assert_eq!(next_doubtful(&d2, 0), Some(1));
        assert_eq!(next_untranslated(&d2, 0), None);
        // save translation only
        let t = translation_only(&d2);
        assert!(!is_tl_mode(&t));
        assert_eq!(t.event_count(), 2);
        assert_eq!(t.event(1).unwrap().text, "Świat");
        assert_eq!(t.event(1).unwrap().effect, "");
        assert!(t.style(DEFAULT_TL_STYLE).is_none());
    }

    #[test]
    fn paste() {
        let mut d = doc();
        enable(&mut d);
        assert_eq!(paste_translation(&mut d, 0, &["Cześć", "Świecie"]), 2);
        let rs = rows(&d);
        assert_eq!(translation_text(&d, rs[0]), "Cześć");
        assert_eq!(translation_text(&d, rs[1]), "Świecie");
        assert_eq!(d.event(rs[1].main()).unwrap().start, Time(1000));
    }

    #[test]
    fn actor_prefixes() {
        let (p, a) = split_actor("[bookmark][hidden]Kaito");
        assert_eq!(p, vec!["[bookmark]", "[hidden]"]);
        assert_eq!(a, "Kaito");
        assert_eq!(join_actor(&p, a), "[bookmark][hidden]Kaito");
    }
}
