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
use crate::model::{Event, Style};

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
    let mut gone = Vec::new();
    for r in rows(doc) {
        if let Some(t) = r.translation {
            let ev = doc.event_mut(t).unwrap();
            ev.effect = strip_doubtful(&ev.effect).to_string();
            gone.push(r.original);
        }
    }
    gone.sort_unstable();
    doc.remove_events(&gone);
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

/// [`set_translation`] for many rows (ascending, distinct) in one pass: the new
/// original events go in together instead of moving the rest of the file each.
/// Returns the rows as they are afterwards.
pub fn set_translations(doc: &mut Document, rows: &[TlRow], texts: &[&str]) -> Vec<TlRow> {
    debug_assert!(rows.windows(2).all(|w| w[0].original < w[1].original));
    let style = tl_style(doc).unwrap_or_else(|| DEFAULT_TL_STYLE.into());
    let new_originals: Vec<(usize, Event)> = rows
        .iter()
        .filter(|r| r.translation.is_none())
        .map(|r| {
            let mut orig = doc.event(r.original).unwrap().clone();
            orig.style = style.clone();
            orig.effect = strip_doubtful(&orig.effect).to_string();
            (r.original, orig)
        })
        .collect();
    doc.insert_events(new_originals);
    // each inserted original moves the events after it down by one
    let mut moved = 0;
    let mut out = Vec::with_capacity(rows.len());
    for (r, text) in rows.iter().zip(texts) {
        let original = r.original + moved;
        let t = match r.translation {
            Some(t) => t + moved,
            None => {
                moved += 1;
                original + 1
            }
        };
        doc.event_mut(t).unwrap().text = text.to_string();
        out.push(TlRow {
            original,
            translation: Some(t),
        });
    }
    out
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
    let rs = rows(doc);
    let n = lines.len().min(rs.len().saturating_sub(first_row));
    if n > 0 {
        set_translations(doc, &rs[first_row..first_row + n], &lines[..n]);
        doc.set_info("TLMode Showtl", "Yes");
    }
    n
}

/// Kainote's translation matching window: line the pasted translation up with
/// the timed originals by moving one against the other.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Shift {
    /// Drop the translation of the rows; the translation below moves up.
    DeleteTranslation,
    /// Insert empty translations at the rows; the translation moves down.
    InsertTranslation,
    /// Join the following translation into the first row; the rest moves up.
    JoinTranslation,
    /// Delete the rows (original and timing); the translation stays in place.
    DeleteOriginal,
    /// Insert untimed empty rows before the first; the translation stays.
    InsertOriginal,
    /// Join the following original into the first row; the translation stays.
    JoinOriginal,
}

/// Apply `op` to the `count` rows from `first`; a join of one row takes the
/// next one with it. No translation text is lost: what moves past the end
/// gets new rows, and rows a move up leaves blank at the end are removed.
pub fn shift(doc: &mut Document, first: usize, count: usize, op: Shift) {
    let n = rows(doc).len();
    if !is_tl_mode(doc) || first >= n || count == 0 {
        return;
    }
    let count = count.min(n - first);
    let joined = (count.max(2) - 1).min(n - 1 - first);
    match op {
        Shift::DeleteTranslation => move_translations(doc, first, -(count as isize)),
        Shift::InsertTranslation => move_translations(doc, first, count as isize),
        Shift::DeleteOriginal => {
            move_translations(doc, first, count as isize);
            remove_rows(doc, first, count);
        }
        Shift::InsertOriginal => {
            let rs = rows(doc);
            let mut ev = doc.event(rs[first].main()).unwrap().clone();
            ev.text.clear();
            ev.effect = strip_doubtful(&ev.effect).to_string();
            doc.insert_events(vec![(rs[first].original, ev); count]);
            move_translations(doc, first, -(count as isize));
        }
        Shift::JoinTranslation | Shift::JoinOriginal if joined == 0 => {}
        Shift::JoinTranslation => {
            let rs = rows(doc);
            let text = join_texts((first..=first + joined).map(|r| translation_text(doc, rs[r])));
            set_translation(doc, rs[first], &text);
            move_translations(doc, first + 1, -(joined as isize));
        }
        Shift::JoinOriginal => {
            let rs = rows(doc);
            let text = join_texts((first..=first + joined).map(|r| original_text(doc, rs[r])));
            let end = doc.event(rs[first + joined].main()).unwrap().end;
            doc.event_mut(rs[first].original).unwrap().text = text;
            for e in [Some(rs[first].original), rs[first].translation]
                .into_iter()
                .flatten()
            {
                doc.event_mut(e).unwrap().end = end;
            }
            move_translations(doc, first + 1, joined as isize);
            remove_rows(doc, first + 1, joined);
        }
    }
}

fn join_texts<'a>(texts: impl Iterator<Item = &'a str>) -> String {
    texts
        .filter(|t| !t.is_empty())
        .collect::<Vec<_>>()
        .join("\\N")
}

fn remove_rows(doc: &mut Document, first: usize, count: usize) {
    let rs = rows(doc);
    let gone: Vec<usize> = rs[first..first + count]
        .iter()
        .flat_map(|r| std::iter::once(r.original).chain(r.translation))
        .collect();
    doc.remove_events(&gone);
}

/// Move the translations (text and doubtful mark) of rows `from..` by `delta`
/// rows, down if positive; each row keeps its original and timing.
fn move_translations(doc: &mut Document, from: usize, delta: isize) {
    let rs = rows(doc);
    let n = rs.len();
    let old: Vec<(String, bool)> = rs
        .iter()
        .map(|r| {
            r.translation
                .map(|t| {
                    let e = doc.event(t).unwrap();
                    (e.text.clone(), is_doubtful(&e.effect))
                })
                .unwrap_or_default()
        })
        .collect();
    let new = |r: usize| -> (String, bool) {
        let src = r as isize - delta;
        if src < from as isize {
            Default::default()
        } else {
            old.get(src as usize).cloned().unwrap_or_default()
        }
    };
    // overflow rows only while they carry text; rows a move up blanks at the end go
    let mut len = n + delta.max(0) as usize;
    let floor = if delta > 0 {
        n
    } else {
        from.max(n - delta.unsigned_abs().min(n))
    };
    while len > floor
        && new(len - 1).0.is_empty()
        && (len > n || original_text(doc, rs[len - 1]).is_empty())
    {
        len -= 1;
    }
    let base = doc.event(rs[n - 1].main()).unwrap().clone();
    if len < n {
        remove_rows(doc, len, n - len);
    }
    let todo: Vec<usize> = (from..len.min(n)).filter(|&r| new(r) != old[r]).collect();
    let values: Vec<(String, bool)> = todo.iter().map(|&r| new(r)).collect();
    let texts: Vec<&str> = values.iter().map(|v| v.0.as_str()).collect();
    let now = set_translations(
        doc,
        &todo.iter().map(|&r| rs[r]).collect::<Vec<_>>(),
        &texts,
    );
    for (row, (_, doubtful)) in now.into_iter().zip(&values) {
        set_doubtful(doc, row, *doubtful);
    }
    let style = tl_style(doc).unwrap_or_else(|| DEFAULT_TL_STYLE.into());
    for r in n..len {
        let (text, doubtful) = new(r);
        let mut orig = base.clone();
        orig.style = style.clone();
        orig.text.clear();
        orig.effect = strip_doubtful(&base.effect).to_string();
        let mut tl = base.clone();
        tl.text = text;
        tl.effect = orig.effect.clone() + if doubtful { DOUBTFUL_MARK } else { "" };
        doc.push_event(orig);
        doc.push_event(tl);
    }
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
        // an untranslated row before a translated one; lines past the end are left over
        let mut d = doc();
        enable(&mut d);
        assert_eq!(paste_translation(&mut d, 1, &["b"]), 1);
        assert_eq!(paste_translation(&mut d, 0, &["x", "y", "z"]), 2);
        assert_eq!(view(&d), pairs(&[("Hello", "x"), ("World", "y")]));
        assert_eq!(d.event_count(), 4);
    }

    fn view(d: &Document) -> Vec<(String, String)> {
        rows(d)
            .into_iter()
            .map(|r| (original_text(d, r).into(), translation_text(d, r).into()))
            .collect()
    }

    fn pairs(v: &[(&str, &str)]) -> Vec<(String, String)> {
        v.iter()
            .map(|(o, t)| (o.to_string(), t.to_string()))
            .collect()
    }

    fn shifted(op: Shift, first: usize, count: usize) -> Document {
        let mut s = doc().to_ass_string();
        s.push_str("Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,,C\n");
        s.push_str("Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,D\n");
        let mut d = Document::parse(&s.replace("Hello", "A").replace("World", "B"));
        enable(&mut d);
        paste_translation(&mut d, 0, &["a", "b", "c", "d"]);
        let b = rows(&d)[1];
        set_doubtful(&mut d, b, true);
        shift(&mut d, first, count, op);
        d
    }

    #[test]
    fn shift_translation() {
        let d = shifted(Shift::DeleteTranslation, 1, 1);
        assert_eq!(
            view(&d),
            pairs(&[("A", "a"), ("B", "c"), ("C", "d"), ("D", "")])
        );
        let mut d = shifted(Shift::InsertTranslation, 1, 1);
        assert_eq!(
            view(&d),
            pairs(&[("A", "a"), ("B", ""), ("C", "b"), ("D", "c"), ("", "d")])
        );
        let rs = rows(&d);
        assert!(is_doubtful(
            &d.event(rs[2].translation.unwrap()).unwrap().effect
        ));
        assert!(!is_doubtful(
            &d.event(rs[1].translation.unwrap()).unwrap().effect
        ));
        // and back: the blank row added at the end goes again
        shift(&mut d, 1, 1, Shift::DeleteTranslation);
        assert_eq!(
            view(&d),
            pairs(&[("A", "a"), ("B", "b"), ("C", "c"), ("D", "d")])
        );
        let d = shifted(Shift::JoinTranslation, 0, 1);
        assert_eq!(
            view(&d),
            pairs(&[("A", "a\\Nb"), ("B", "c"), ("C", "d"), ("D", "")])
        );
    }

    #[test]
    fn shift_original() {
        let d = shifted(Shift::DeleteOriginal, 1, 1);
        assert_eq!(
            view(&d),
            pairs(&[("A", "a"), ("C", "b"), ("D", "c"), ("", "d")])
        );
        let d = shifted(Shift::InsertOriginal, 1, 2);
        assert_eq!(
            view(&d),
            pairs(&[
                ("A", "a"),
                ("", "b"),
                ("", "c"),
                ("B", "d"),
                ("C", ""),
                ("D", "")
            ])
        );
        assert_eq!(d.event(rows(&d)[1].main()).unwrap().start, Time(1000));
        let d = shifted(Shift::JoinOriginal, 0, 2);
        assert_eq!(
            view(&d),
            pairs(&[("A\\NB", "a"), ("C", "b"), ("D", "c"), ("", "d")])
        );
        let r = rows(&d)[0];
        assert_eq!(d.event(r.original).unwrap().end, Time(2000));
        assert_eq!(d.event(r.main()).unwrap().end, Time(2000));
    }

    #[test]
    fn actor_prefixes() {
        let (p, a) = split_actor("[bookmark][hidden]Kaito");
        assert_eq!(p, vec!["[bookmark]", "[hidden]"]);
        assert_eq!(a, "Kaito");
        assert_eq!(join_actor(&p, a), "[bookmark][hidden]Kaito");
    }
}
