//! Aegisub's other subtitle formats: MPEG-4 Timed Text (TTXT, read and written) and the
//! write-only Adobe Encore, TranStation and EBU Tech 3264 (STL).
//!
//! They share Aegisub's preparation of the lines ([`pieces`]): sorted by start, comments and
//! empty lines left out, overlapping lines cut into one line per span with the later line on
//! top, and identical neighbours joined again.

use std::collections::BTreeSet;
use std::fmt::Write as _;

use encoding_rs::{Encoding, ISO_8859_5, ISO_8859_6, ISO_8859_7, ISO_8859_8};
use fs_core::model::{EventKind, ssa_to_numpad};
use fs_core::tags::{self, Block, Line};
use fs_core::vfr::{Framerate, TimeType};
use fs_core::{Document, Time};

use crate::ebu_tables::{CP850_HIGH, DIACRITICS};
use crate::simple::Sub;

/// A line as the export formats write it.
#[derive(Debug, Clone, PartialEq)]
pub struct Piece {
    pub start: Time,
    pub end: Time,
    /// The text, tags included. Where lines overlap they are joined with `\N`, the line that
    /// started later first (Aegisub's RecombineOverlaps).
    pub text: String,
    /// The style of the line that started first.
    pub style: String,
}

/// Aegisub's Sort, StripComments, RecombineOverlaps and MergeIdentical as one sweep over the
/// line boundaries: O(n log n), where Aegisub's list splicing turns quadratic on stacked lines.
/// Lines that span no time (end at or before the start) are kept as they are.
pub fn pieces(doc: &Document) -> Vec<Piece> {
    let mut lines: Vec<&fs_core::Event> = (doc.events().iter())
        .map(|e| &e.event)
        .filter(|e| e.kind == EventKind::Dialogue && !e.text.is_empty())
        .collect();
    // stable: lines starting together keep the file's order, so the later one goes on top
    lines.sort_by_key(|e| e.start);
    // (time, 0 = an end, 1 = a start, line): ends first, so touching lines do not overlap
    let mut bounds: Vec<(Time, u8, usize)> = Vec::with_capacity(lines.len() * 2);
    let mut out = Vec::with_capacity(lines.len());
    let mut points = Vec::new();
    for (i, e) in lines.iter().enumerate() {
        if e.end > e.start {
            bounds.push((e.start, 1, i));
            bounds.push((e.end, 0, i));
        } else {
            points.push(Piece {
                start: e.start,
                end: e.end,
                text: e.text.clone(),
                style: e.style.clone(),
            });
        }
    }
    bounds.sort_unstable();
    // the lines showing, in start order (the index order of `lines`)
    let mut active = BTreeSet::new();
    let mut k = 0;
    while k < bounds.len() {
        let t = bounds[k].0;
        while k < bounds.len() && bounds[k].0 == t {
            let (_, start, i) = bounds[k];
            if start == 1 {
                active.insert(i);
            } else {
                active.remove(&i);
            }
            k += 1;
        }
        let (Some(&first), Some(&(next, ..))) = (active.first(), bounds.get(k)) else {
            continue;
        };
        let mut text = String::new();
        for &i in active.iter().rev() {
            if !text.is_empty() {
                text.push_str("\\N");
            }
            text.push_str(&lines[i].text);
        }
        push_merged(
            &mut out,
            Piece {
                start: t,
                end: next,
                text,
                style: lines[first].style.clone(),
            },
        );
    }
    if !points.is_empty() {
        out.extend(points);
        out.sort_by_key(|p| p.start);
    }
    out
}

/// Aegisub's MergeIdentical: a piece continuing the last one with the same text extends it.
fn push_merged(out: &mut Vec<Piece>, p: Piece) {
    match out.last_mut() {
        Some(last) if last.end == p.start && last.text == p.text => {
            last.start = last.start.min(p.start);
            last.end = last.end.max(p.end);
        }
        _ => out.push(p),
    }
}

/// Aegisub's StripTags and ConvertNewlines: the visible text, `\h` as a space and each run of
/// `\N`/`\n` as one `newline`.
pub fn plain_text(text: &str, newline: &str) -> String {
    let visible = tags::visible_text(text);
    let mut out = String::with_capacity(visible.len());
    let mut it = visible.chars().peekable();
    while let Some(c) = it.next() {
        if c == '\\' {
            match it.peek() {
                Some('h') => {
                    it.next();
                    out.push(' ');
                    continue;
                }
                Some('N' | 'n') => {
                    it.next();
                    if !out.ends_with(newline) {
                        out.push_str(newline);
                    }
                    continue;
                }
                _ => {}
            }
        }
        out.push(c);
    }
    out
}

/// `hh:mm:ss:ff` with `sep` between the fields.
fn smpte(fps: &Framerate, t: Time, sep: char) -> String {
    let (h, m, s, f) = fps.smpte_at_time(t.0);
    format!("{h:02}{sep}{m:02}{sep}{s:02}{sep}{f:02}")
}

// ---------------------------------------------------------------------------
// Adobe Encore and TranStation
// ---------------------------------------------------------------------------

/// Adobe Encore: `n start end text`, SMPTE times with `;` for drop-frame rates and `:` for the
/// others (the manual knows no other rates).
pub fn write_encore(doc: &Document, fps: &Framerate) -> String {
    let sep = if fps.needs_drop_frames() { ';' } else { ':' };
    let mut out = String::new();
    for (i, p) in pieces(doc).iter().enumerate() {
        let _ = write!(
            out,
            "{} {} {} {}\r\n",
            i + 1,
            smpte(fps, p.start, sep),
            smpte(fps, p.end, sep),
            plain_text(&p.text, "\r\n")
        );
    }
    out
}

/// TranStation: `SUB[<v><h><type> start>end]` and the text, a blank line between the lines and
/// `SUB[` at the end. Alignment and italics come from the style; `\i1` anywhere in a line makes
/// it italic (Aegisub tests for it after removing the tags, so there it never does).
pub fn write_transtation(doc: &Document, fps: &Framerate) -> String {
    let ps = pieces(doc);
    let mut out = String::new();
    for (i, p) in ps.iter().enumerate() {
        let style = doc.style(&p.style);
        let align = style.map_or(2, |s| s.alignment);
        let valign = match align {
            7.. => 9,
            4.. => 4,
            _ => 0,
        };
        let halign = match align {
            1 | 4 | 7 => 'L',
            3 | 6 | 9 => 'R',
            _ => ' ',
        };
        let italic = style.is_some_and(|s| s.italic) || p.text.contains("\\i1");
        // the out time is inclusive: a line ending where the next starts ends on its last frame
        // (Aegisub goes one more frame back, leaving a frame between the lines)
        let mut end = p.end;
        if ps.get(i + 1).is_some_and(|next| next.start == end) {
            let frame = fps.frame_at_time(end.0, TimeType::End);
            end = Time(fps.time_at_frame(frame, TimeType::End));
        }
        if i > 0 {
            out.push_str("\r\n");
        }
        let _ = write!(
            out,
            "SUB[{valign}{halign}{} {}>{}]\r\n{}\r\n",
            if italic { 'I' } else { 'N' },
            smpte(fps, p.start, ':'),
            smpte(fps, end, ':'),
            plain_text(&p.text, "\r\n")
        );
    }
    out.push_str("SUB[\r\n");
    out
}

// ---------------------------------------------------------------------------
// MPEG-4 Timed Text
// ---------------------------------------------------------------------------

fn xml_escape(s: &str, out: &mut String) {
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            _ => out.push(c),
        }
    }
}

fn ttxt_time(t: Time) -> String {
    let t = t.0.max(0);
    format!(
        "{:02}:{:02}:{:02}.{:03}",
        t / 3_600_000,
        (t / 60_000) % 60,
        (t / 1000) % 60,
        t % 1000
    )
}

/// TTXT 1.1 as Aegisub writes it: one sample per line, an empty sample where a gap starts, and
/// an empty sample `default_duration` after the last line.
pub fn write_ttxt(doc: &Document, default_duration: Time) -> String {
    let mut out = String::from(concat!(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n",
        "<TextStream version=\"1.1\">\r\n",
        "  <TextStreamHeader width=\"400\" height=\"60\" layer=\"0\" translation_x=\"0\" translation_y=\"0\">\r\n",
        "    <TextSampleDescription horizontalJustification=\"center\" verticalJustification=\"bottom\" backColor=\"0 0 0 0\" verticalText=\"no\" fillTextRegion=\"no\" continuousKaraoke=\"no\" scroll=\"None\">\r\n",
        "      <FontTable>\r\n",
        "        <FontTableEntry fontName=\"Sans\" fontID=\"1\"/>\r\n",
        "      </FontTable>\r\n",
        "      <TextBox top=\"0\" left=\"0\" bottom=\"60\" right=\"400\"/>\r\n",
        "      <Style styles=\"Normal\" fontID=\"1\" fontSize=\"18\" color=\"ff ff ff ff\"/>\r\n",
        "    </TextSampleDescription>\r\n",
        "  </TextStreamHeader>\r\n",
    ));
    let sample = |out: &mut String, at: Time, text: &str| {
        let _ = write!(
            out,
            "  <TextSample sampleTime=\"{}\" xml:space=\"preserve\">",
            ttxt_time(at)
        );
        xml_escape(text, out);
        out.push_str("</TextSample>\r\n");
    };
    let mut ps = pieces(doc);
    let last = ps.last().map_or(Time(0), |p| p.end);
    ps.push(Piece {
        start: last,
        end: last + default_duration,
        text: String::new(),
        style: String::new(),
    });
    let mut prev_end: Option<Time> = None;
    for p in &ps {
        if let Some(end) = prev_end
            && end != p.start
        {
            sample(&mut out, end, "");
        }
        sample(&mut out, p.start, &plain_text(&p.text, "\r\n"));
        prev_end = Some(p.end);
    }
    out.push_str("</TextStream>\r\n");
    out
}

/// TTXT 1.0 (text in a `text` attribute, rows in single quotes) or 1.1 (text as the content).
/// A sample ends the line before it; an empty one only ends it. `None` if it is not TTXT.
pub fn parse_ttxt(text: &str) -> Option<Vec<Sub>> {
    let xml = roxmltree::Document::parse(text).ok()?;
    let root = xml.root_element();
    if root.tag_name().name() != "TextStream" {
        return None;
    }
    let v10 = match root.attribute("version") {
        Some("1.0") => true,
        Some("1.1") => false,
        _ => return None,
    };
    let mut out: Vec<Sub> = Vec::new();
    // whether the last sample made a line its successor ends
    let mut open = false;
    for node in root.children().filter(|n| n.has_tag_name("TextSample")) {
        let at = Time::from_ass_field(node.attribute("sampleTime").unwrap_or("00:00:00.000"));
        if open && let Some(prev) = out.last_mut() {
            prev.end = at;
        }
        let text = if v10 {
            // rows in single quotes; what is outside them is left out
            let mut s = String::new();
            let (mut inside, mut first) = (false, true);
            for c in node.attribute("text").unwrap_or("").chars() {
                if c == '\'' {
                    if !inside && !first {
                        s.push_str("\\N");
                    }
                    first = false;
                    inside = !inside;
                } else if inside {
                    s.push(c);
                }
            }
            s
        } else {
            let content: String = (node.descendants())
                .filter(|n| n.is_text())
                .filter_map(|n| n.text())
                .collect();
            content.replace('\r', "").replace('\n', "\\N")
        };
        open = !text.is_empty();
        if open {
            out.push(Sub {
                start: at,
                // Aegisub's: until a sample ends it
                end: Time(36_000_000 - 10),
                text,
            });
        }
    }
    Some(out)
}

// ---------------------------------------------------------------------------
// EBU Tech 3264 (STL)
// ---------------------------------------------------------------------------

/// The settings of Aegisub's "Export to EBU STL format" dialog, numbered as there (and as
/// stored in its options).
#[derive(Debug, Clone, PartialEq)]
pub struct EbuSettings {
    /// 0 23.976 (STL24.01), 1 24, 2 25 (STL25.01), 3 29.97 non-drop, 4 29.97 drop-frame,
    /// 5 30 (STL30.01).
    pub tv_standard: u8,
    /// 0 ISO 6937-2 (Latin), 1 ISO 8859-5 (Cyrillic), 2 ISO 8859-6 (Arabic), 3 ISO 8859-7
    /// (Greek), 4 ISO 8859-8 (Hebrew), 5 UTF-8 (not in the standard).
    pub text_encoding: u8,
    /// Characters per row.
    pub max_line_length: usize,
    /// Rows longer than `max_line_length`: 0 wrapped as the script's WrapStyle says, 1 wrapped
    /// into balanced rows, 2 fail the export, 3 the line is left out.
    pub line_wrapping: u8,
    /// The lines' alignment sets the row and justification; otherwise all are centred at the
    /// bottom.
    pub translate_alignments: bool,
    /// The timecode of time 0: hours, minutes, seconds, frames.
    pub timecode_offset: [u8; 4],
    /// Out-times name the last frame shown instead of the first one gone.
    pub inclusive_end_times: bool,
    /// 0 open subtitles, 1 level-1 teletext, 2 level-2 teletext.
    pub display_standard: u8,
}

impl Default for EbuSettings {
    /// Aegisub's defaults.
    fn default() -> Self {
        EbuSettings {
            tv_standard: 0,
            text_encoding: 0,
            max_line_length: 42,
            line_wrapping: 1,
            translate_alignments: true,
            timecode_offset: [0; 4],
            inclusive_end_times: true,
            display_standard: 0,
        }
    }
}

impl EbuSettings {
    pub fn framerate(&self) -> Framerate {
        let (num, den, drop) = match self.tv_standard {
            0 => (24000, 1001, false),
            1 => (24, 1, false),
            3 => (30000, 1001, false),
            4 => (30000, 1001, true),
            5 => (30, 1, false),
            _ => (25, 1, false),
        };
        Framerate::from_rational(num, den, drop).expect("valid rate")
    }

    fn disk_format(&self) -> &'static [u8; 8] {
        match self.tv_standard {
            0 | 1 => b"STL24.01",
            3..=5 => b"STL30.01",
            _ => b"STL25.01",
        }
    }
}

/// A run of text with one formatting; `word_start`: a row may wrap before it.
#[derive(Debug, Clone, PartialEq)]
struct Part {
    text: String,
    underline: bool,
    italic: bool,
    word_start: bool,
}

type Row = Vec<Part>;

impl Part {
    fn new(underline: bool, italic: bool, word_start: bool) -> Part {
        Part {
            text: String::new(),
            underline,
            italic,
            word_start,
        }
    }
}

/// A subtitle of the file before it is cut into blocks.
struct EbuSub {
    time_in: i64,
    time_out: i64,
    /// 1 left, 2 centre, 3 right.
    justification: u8,
    /// 0 bottom, 1 middle, 2 top.
    vertical: u8,
    rows: Vec<Row>,
}

/// The rows of an ASS line with their underline and italics, and its alignment: Aegisub's
/// `SetTextFromAss`, without its slips (`\n` breaks a row in WrapStyle 2, not 1; `\h` becomes a
/// space in every part, not only the last; `\r` goes back to the style).
fn ebu_rows(
    text: &str,
    style_u: bool,
    style_i: bool,
    align: u8,
    wrap_style: i32,
) -> (Vec<Row>, u8) {
    let (mut u, mut i, mut align) = (style_u, style_i, align);
    let mut rows: Vec<Row> = vec![vec![Part::new(u, i, true)]];
    for block in Line::parse(text).blocks {
        match block {
            Block::Text(t) => {
                let t = t.replace("\\t", " ");
                let mut chars = t.chars().peekable();
                while let Some(c) = chars.next() {
                    let hard = c == '\\'
                        && chars
                            .next_if(|&n| n == 'N' || (n == 'n' && wrap_style == 2))
                            .is_some();
                    if hard {
                        rows.push(vec![Part::new(u, i, true)]);
                    } else if c == ' ' || (c == '\\' && chars.next_if_eq(&'n').is_some()) {
                        last_part(&mut rows).text.push(' ');
                        rows.last_mut().expect("a row").push(Part::new(u, i, true));
                    } else if c == '\\' && chars.next_if_eq(&'h').is_some() {
                        // a hard space joins the words around it
                        last_part(&mut rows).text.push(' ');
                    } else {
                        last_part(&mut rows).text.push(c);
                    }
                }
            }
            Block::Override { tags, .. } => {
                for tag in &tags {
                    let flag = |def: bool| tag.arg_f64(0).map_or(def, |v| v != 0.0);
                    match tag.name.as_str() {
                        "u" => u = flag(style_u),
                        "i" => i = flag(style_i),
                        "r" => (u, i) = (style_u, style_i),
                        "an" => {
                            if let Some(v) = tag.arg_f64(0) {
                                align = v as u8;
                            }
                        }
                        "a" => {
                            if let Some(v) = tag.arg_f64(0) {
                                align = ssa_to_numpad(v as i32);
                            }
                        }
                        _ => {}
                    }
                }
                let last = last_part(&mut rows);
                if (last.underline, last.italic) != (u, i) {
                    if last.text.is_empty() {
                        (last.underline, last.italic) = (u, i);
                    } else {
                        rows.last_mut().expect("a row").push(Part::new(u, i, false));
                    }
                }
            }
            Block::Drawing { .. } | Block::Comment(_) => {}
        }
    }
    (rows, align)
}

fn last_part(rows: &mut [Row]) -> &mut Part {
    rows.last_mut()
        .and_then(|r| r.last_mut())
        .expect("every row has a part")
}

fn row_len(row: &Row) -> usize {
    row.iter().map(|p| p.text.chars().count()).sum()
}

/// Aegisub's `agi::get_wrap_points`: the words new rows start at. `mode` is an ASS WrapStyle:
/// 0 balanced with the first row the longer, 1 greedy, 2 no wrapping, 3 balanced with the last
/// row the longer, 4 balanced. Unlike Aegisub's, moving words to make the last row longer
/// measures the word that moves, and never empties a row.
pub fn wrap_points(widths: &[usize], max: usize, mode: u8) -> Vec<usize> {
    let mut ret = Vec::new();
    if mode == 2 || widths.len() < 2 || widths.iter().sum::<usize>() <= max {
        return ret;
    }
    if mode == 1 {
        let mut cur = 0;
        for (i, &w) in widths.iter().enumerate() {
            if cur > 0 && cur + w > max {
                ret.push(i);
                cur = 0;
            }
            cur += w;
        }
        return ret;
    }
    let n = widths.len();
    // the least waste of the words up to i, and where its last row starts
    let mut cost = vec![u64::MAX; n];
    let mut starts = vec![0usize; n];
    for end in 0..n {
        let mut width = 0;
        for start in (0..=end).rev() {
            width += widths[start];
            // a row over the limit only when it is one word
            if width > max && start != end {
                break;
            }
            let waste = (width as i64 - max as i64).unsigned_abs().pow(2);
            let c = waste.saturating_add(if start > 0 { cost[start - 1] } else { 0 });
            if c < cost[end] {
                cost[end] = c;
                starts[end] = start;
            }
        }
    }
    let mut last = n;
    while last > 0 && starts[last - 1] > 0 {
        last = starts[last - 1];
        ret.push(last);
    }
    ret.reverse();
    if mode != 4 {
        unbalance(&mut ret, widths, max, mode == 3);
    }
    ret
}

/// Moves words across the breaks until the first (or last) of each pair of rows is the longer.
fn unbalance(breaks: &mut [usize], widths: &[usize], max: usize, last_longer: bool) {
    let bounds = |b: &[usize]| {
        let mut v = Vec::with_capacity(b.len() + 2);
        v.push(0);
        v.extend_from_slice(b);
        v.push(widths.len());
        v
    };
    let mut lw: Vec<usize> = bounds(breaks)
        .windows(2)
        .map(|w| widths[w[0]..w[1]].iter().sum())
        .collect();
    for i in 0..breaks.len() {
        let lo = if i == 0 { 0 } else { breaks[i - 1] };
        let hi = breaks.get(i + 1).copied().unwrap_or(widths.len());
        if last_longer {
            // the last word of row i moves down while row i+1 is the shorter
            while lw[i + 1] < lw[i] && breaks[i] - 1 > lo {
                let w = widths[breaks[i] - 1];
                if lw[i + 1] + w > max {
                    break;
                }
                lw[i + 1] += w;
                lw[i] -= w;
                breaks[i] -= 1;
            }
        } else {
            // the first word of row i+1 moves up while row i is the shorter
            while lw[i] < lw[i + 1] && breaks[i] + 1 < hi {
                let w = widths[breaks[i]];
                if lw[i] + w > max {
                    break;
                }
                lw[i] += w;
                lw[i + 1] -= w;
                breaks[i] += 1;
            }
        }
    }
}

/// Rows longer than `max` wrapped at word starts (Aegisub's `SplitLines`).
fn split_rows(rows: Vec<Row>, max: usize, mode: i32) -> Vec<Row> {
    if !(0..=4).contains(&mode) || mode == 2 {
        return rows;
    }
    let mut out = Vec::with_capacity(rows.len());
    for row in rows {
        let mut words: Vec<usize> = Vec::new();
        for p in &row {
            if p.word_start || words.is_empty() {
                words.push(0);
            }
            *words.last_mut().expect("a word") += p.text.chars().count();
        }
        let points = wrap_points(&words, max, mode as u8);
        if points.is_empty() {
            out.push(row);
            continue;
        }
        out.push(Vec::new());
        let (mut word, mut next) = (0usize, 0usize);
        for (k, p) in row.into_iter().enumerate() {
            if p.word_start || k == 0 {
                if k > 0 {
                    word += 1;
                }
                if points.get(next) == Some(&word) {
                    out.push(Vec::new());
                    next += 1;
                }
            }
            out.last_mut().expect("a row").push(p);
        }
    }
    out
}

/// One character of ISO 6937 (EBU's code table 00): ASCII but `$`, the letters with a
/// diacritic as two bytes, and `?` for what it lacks.
fn push_iso6937(c: char, out: &mut Vec<u8>) {
    let b = match c {
        '$' => 0xA4,
        ' '..='~' => c as u8,
        '\u{A0}' => 0xA0,
        '¡' => 0xA1,
        '¢' => 0xA2,
        '£' => 0xA3,
        '¥' => 0xA5,
        '§' => 0xA7,
        '¤' => 0xA8,
        '‘' => 0xA9,
        '“' => 0xAA,
        '«' => 0xAB,
        '←' => 0xAC,
        '↑' => 0xAD,
        '→' => 0xAE,
        '↓' => 0xAF,
        '°' => 0xB0,
        '±' => 0xB1,
        '²' => 0xB2,
        '³' => 0xB3,
        '×' => 0xB4,
        'µ' => 0xB5,
        '¶' => 0xB6,
        '·' => 0xB7,
        '÷' => 0xB8,
        '’' => 0xB9,
        '”' => 0xBA,
        '»' => 0xBB,
        '¼' => 0xBC,
        '½' => 0xBD,
        '¾' => 0xBE,
        '¿' => 0xBF,
        '―' => 0xD0,
        '¹' => 0xD1,
        '®' => 0xD2,
        '©' => 0xD3,
        '™' => 0xD4,
        '♪' => 0xD5,
        '¬' => 0xD6,
        '¦' => 0xD7,
        '⅛' => 0xDC,
        '⅜' => 0xDD,
        '⅝' => 0xDE,
        '⅞' => 0xDF,
        'Ω' => 0xE0,
        'Æ' => 0xE1,
        'Đ' => 0xE2,
        'ª' => 0xE3,
        'Ħ' => 0xE4,
        'Ĳ' => 0xE6,
        'Ŀ' => 0xE7,
        'Ł' => 0xE8,
        'Ø' => 0xE9,
        'Œ' => 0xEA,
        'º' => 0xEB,
        'Þ' => 0xEC,
        'Ŧ' => 0xED,
        'Ŋ' => 0xEE,
        'ŉ' => 0xEF,
        'ĸ' => 0xF0,
        'æ' => 0xF1,
        'đ' => 0xF2,
        'ð' => 0xF3,
        'ħ' => 0xF4,
        'ı' => 0xF5,
        'ĳ' => 0xF6,
        'ŀ' => 0xF7,
        'ł' => 0xF8,
        'ø' => 0xF9,
        'œ' => 0xFA,
        'ß' => 0xFB,
        'þ' => 0xFC,
        'ŧ' => 0xFD,
        'ŋ' => 0xFE,
        '\u{AD}' => 0xFF,
        _ => match DIACRITICS.binary_search_by_key(&c, |d| d.0) {
            Ok(k) => {
                out.push(DIACRITICS[k].1);
                DIACRITICS[k].2
            }
            Err(_) => b'?',
        },
    };
    out.push(b);
}

fn push_8bit(c: char, enc: &'static Encoding, out: &mut Vec<u8>) {
    let mut buf = [0u8; 4];
    let (bytes, _, unmappable) = enc.encode(c.encode_utf8(&mut buf));
    if unmappable {
        out.push(b'?');
    } else {
        out.extend_from_slice(&bytes);
    }
}

fn encode_ebu(text: &str, encoding: u8, out: &mut Vec<u8>) {
    let eight = match encoding {
        1 => Some(ISO_8859_5),
        2 => Some(ISO_8859_6),
        3 => Some(ISO_8859_7),
        4 => Some(ISO_8859_8),
        _ => None,
    };
    for c in text.chars() {
        match (encoding, eight) {
            (5, _) => out.extend_from_slice(c.encode_utf8(&mut [0u8; 4]).as_bytes()),
            (_, Some(enc)) => push_8bit(c, enc, out),
            _ => push_iso6937(c, out),
        }
    }
}

/// Code page 850 for the header's text fields, cut to the field.
fn put_cp850(field: &mut [u8], text: &str) {
    let bytes = text.chars().map(|c| match c {
        ' '..='~' => c as u8,
        _ => CP850_HIGH
            .iter()
            .position(|&h| h == c)
            .map_or(b'?', |k| 0x80 + k as u8),
    });
    for (dst, b) in field.iter_mut().zip(bytes) {
        *dst = b;
    }
}

/// Today's date and time (UTC): `yymmddHHMMSS`.
fn stamp_now() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_secs() as i64);
    let (days, rem) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    // Howard Hinnant's civil_from_days
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!(
        "{:02}{m:02}{d:02}{:02}{:02}{:02}",
        y % 100,
        rem / 3600,
        rem / 60 % 60,
        rem % 60
    )
}

/// The file Aegisub's EBU STL export writes, with its slips fixed: frames of the timecode
/// offset (Aegisub uses its seconds twice), middle-aligned lines (Aegisub puts them at the
/// top), alignment rows (it reads 3, 6 and 9 a row too high) and row lengths in characters
/// (it counts UTF-8 bytes). `Err` names a line over the maximum length when the settings say
/// to fail.
pub fn write_ebu_stl(doc: &Document, s: &EbuSettings) -> Result<Vec<u8>, String> {
    write_ebu_stl_at(doc, s, &stamp_now())
}

fn write_ebu_stl_at(doc: &Document, s: &EbuSettings, stamp: &str) -> Result<Vec<u8>, String> {
    const ITALIC: [u8; 2] = [0x81, 0x80];
    const UNDERLINE: [u8; 2] = [0x83, 0x82];
    const LINE_BREAK: u8 = 0x8A;
    const UNUSED: u8 = 0x8F;
    const TF: usize = 112;

    let fps = s.framerate();
    let [oh, om, os, of] = s.timecode_offset.map(i64::from);
    let bias = fps.frame_at_smpte(oh, om, os, of);
    let wrap_style: i32 = doc
        .info("WrapStyle")
        .and_then(|v| v.trim().parse().ok())
        .unwrap_or(0);
    let max = s.max_line_length.max(1);
    let default_style = fs_core::Style::default();

    let mut subs = Vec::new();
    for p in pieces(doc) {
        let style = doc.style(&p.style).unwrap_or(&default_style);
        let (rows, align) = ebu_rows(
            &p.text,
            style.underline,
            style.italic,
            style.alignment,
            wrap_style,
        );
        let rows = match s.line_wrapping {
            0 => split_rows(rows, max, wrap_style),
            1 => split_rows(rows, max, 4),
            _ if rows.iter().all(|r| row_len(r) <= max) => rows,
            2 => return Err(format!("Line over maximum length: {}", p.text)),
            _ => continue,
        };
        let align = if (1..=9).contains(&align) { align } else { 2 };
        let mut time_out = fps.frame_at_time(p.end.0, TimeType::Exact) + bias;
        if s.inclusive_end_times {
            time_out -= 1;
        }
        subs.push(EbuSub {
            time_in: fps.frame_at_time(p.start.0, TimeType::Exact) + bias,
            time_out,
            justification: (align - 1) % 3 + 1,
            vertical: (align - 1) / 3,
            rows,
        });
    }
    if subs.is_empty() {
        // an empty file still needs a subtitle, and a blank one is ignored
        let mut part = Part::new(false, false, true);
        part.text.push(' ');
        subs.push(EbuSub {
            time_in: bias,
            time_out: bias,
            justification: 2,
            vertical: 0,
            rows: vec![vec![part]],
        });
    }

    // teletext rows are 1-23, open subtitles 0-99
    let (min_row, max_row): (u8, u8) = if s.display_standard == 0 {
        (0, 100)
    } else {
        (1, 24)
    };
    let tc = |frame: i64| {
        let (h, m, sec, f) = fps.smpte_at_frame(frame);
        [h, m, sec, f].map(|v| v.clamp(0, 255) as u8)
    };
    let mut tti: Vec<[u8; 128]> = Vec::with_capacity(subs.len());
    for (number, sub) in subs.iter().enumerate() {
        let mut text = Vec::new();
        for row in &sub.rows {
            if !text.is_empty() {
                text.push(LINE_BREAK);
            }
            // formatting starts over on every row; teletext has none
            let (mut u, mut i) = (false, false);
            for part in row {
                if s.display_standard == 0 {
                    if u != part.underline {
                        text.push(UNDERLINE[usize::from(part.underline)]);
                    }
                    if i != part.italic {
                        text.push(ITALIC[usize::from(part.italic)]);
                    }
                    (u, i) = (part.underline, part.italic);
                }
                encode_ebu(&part.text, s.text_encoding, &mut text);
            }
        }
        let mut base = [0u8; 128];
        base[1..3].copy_from_slice(&(number as u16).to_le_bytes());
        base[3] = 0xFF;
        base[5..9].copy_from_slice(&tc(sub.time_in));
        base[9..13].copy_from_slice(&tc(sub.time_out));
        (base[13], base[14]) = if s.translate_alignments {
            let rows = sub.rows.len().min(255) as u8;
            let vp = match sub.vertical {
                2 => min_row,
                1 => (max_row / 2)
                    .saturating_sub((max_row / 5).saturating_mul(rows))
                    .max(min_row),
                _ => max_row - 1,
            };
            (vp, sub.justification)
        } else {
            (max_row - 1, 2)
        };
        base[16..].fill(UNUSED);
        let mut extension = 0u8;
        let mut pos = 0;
        loop {
            let rest = text.len() - pos;
            let mut block = base;
            let n = rest.min(TF);
            block[16..16 + n].copy_from_slice(&text[pos..pos + n]);
            // a block the text goes on after numbers itself; the last is 0xFF
            if rest >= TF {
                block[3] = extension;
                extension = extension.wrapping_add(1);
            }
            tti.push(block);
            pos += n;
            if rest < TF {
                break;
            }
            // text filling the last block exactly is ended by an empty one
            if pos == text.len() {
                tti.push(base);
                break;
            }
        }
    }

    let mut gsi = [b' '; 1024];
    gsi[0..3].copy_from_slice(b"850");
    gsi[3..11].copy_from_slice(s.disk_format());
    gsi[11] = b'0' + s.display_standard.min(2);
    let cct = match s.text_encoding {
        5 => *b"U8",
        e => [b'0', b'0' + e.min(4)],
    };
    gsi[12..14].copy_from_slice(&cct);
    gsi[14..16].copy_from_slice(b"00");
    put_cp850(&mut gsi[16..48], doc.info("Title").unwrap_or(""));
    put_cp850(
        &mut gsi[144..176],
        doc.info("Original Translation").unwrap_or(""),
    );
    gsi[208..224].copy_from_slice(format!("FSE-{stamp}").as_bytes());
    gsi[224..230].copy_from_slice(&stamp.as_bytes()[..6]);
    gsi[230..236].copy_from_slice(&stamp.as_bytes()[..6]);
    gsi[236..238].copy_from_slice(b"00");
    gsi[238..243].copy_from_slice(format!("{:5}", tti.len().min(99_999)).as_bytes());
    gsi[243..248].copy_from_slice(format!("{:5}", subs.len().min(99_999)).as_bytes());
    gsi[248..251].copy_from_slice(b"001");
    gsi[251..253].copy_from_slice(format!("{:02}", s.max_line_length.min(99)).as_bytes());
    gsi[253..255].copy_from_slice(b"99");
    gsi[255] = b'1';
    let [h, m, sec, f] = s.timecode_offset;
    gsi[256..264].copy_from_slice(format!("{h:02}{m:02}{sec:02}{f:02}").as_bytes());
    let first = &tti[0][5..9];
    gsi[264..272].copy_from_slice(
        format!(
            "{:02}{:02}{:02}{:02}",
            first[0], first[1], first[2], first[3]
        )
        .as_bytes(),
    );
    gsi[272] = b'1';
    gsi[273] = b'1';
    gsi[274..277].copy_from_slice(b"XXX");
    put_cp850(
        &mut gsi[309..341],
        doc.info("Original Editing").unwrap_or(""),
    );
    if s.text_encoding == 5 {
        let uda = &mut gsi[448..1024];
        uda.fill(0);
        let note = b"This file was exported by FS Editor using non-standard UTF-8 encoding for the subtitle blocks. The TTI.TF field contains UTF-8-encoded text interspersed with the standard formatting codes, which are not encoded. GSI.CCT is set to 'U8' to signify this.";
        uda[..note.len()].copy_from_slice(note);
    }

    let mut out = Vec::with_capacity(1024 + tti.len() * 128);
    out.extend_from_slice(&gsi);
    for block in &tti {
        out.extend_from_slice(block);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use fs_core::Event;

    fn doc_of(lines: &[(i64, i64, &str)]) -> Document {
        let mut d = Document::new();
        d.remove_event(0);
        for &(s, e, t) in lines {
            d.push_event(Event::dialogue(Time(s), Time(e), "Default", t));
        }
        d
    }

    fn spans(ps: &[Piece]) -> Vec<(i64, i64, &str)> {
        ps.iter()
            .map(|p| (p.start.0, p.end.0, p.text.as_str()))
            .collect()
    }

    #[test]
    fn overlaps_recombine_like_aegisub() {
        // Aegisub's split-merge: A, B over A, C over B over A, C over B, B
        let d = doc_of(&[(0, 10, "A"), (5, 15, "B"), (7, 12, "C")]);
        assert_eq!(
            spans(&pieces(&d)),
            [
                (0, 5, "A"),
                (5, 7, "B\\NA"),
                (7, 10, "C\\NB\\NA"),
                (10, 12, "C\\NB"),
                (12, 15, "B")
            ]
        );
    }

    #[test]
    fn identical_neighbours_merge_and_comments_go() {
        let mut d = doc_of(&[(0, 5, "x"), (5, 10, "x"), (10, 20, ""), (20, 30, "y")]);
        d.event_mut(3).unwrap().kind = EventKind::Comment;
        d.push_event(Event::dialogue(Time(40), Time(40), "Default", "point"));
        assert_eq!(spans(&pieces(&d)), [(0, 10, "x"), (40, 40, "point")]);
    }

    #[test]
    fn plain_text_strips_tags_and_merges_breaks() {
        assert_eq!(
            plain_text("{\\i1}a\\N\\Nb\\hc{\\p1}m 0 0{\\p0}", "\r\n"),
            "a\r\nb c"
        );
    }

    #[test]
    fn encore_uses_semicolons_for_drop_frame() {
        let d = doc_of(&[(1000, 2000, "Hi\\Nthere")]);
        let pal = Framerate::from_rational(25, 1, false).unwrap();
        assert_eq!(
            write_encore(&d, &pal),
            "1 00:00:01:00 00:00:02:00 Hi\r\nthere\r\n"
        );
        let ntsc = Framerate::from_rational(30000, 1001, true).unwrap();
        assert!(write_encore(&d, &ntsc).starts_with("1 00;00;00;"));
    }

    #[test]
    fn transtation_ends_touching_lines_a_frame_early() {
        let mut d = doc_of(&[(0, 1000, "a"), (1000, 2000, "{\\i1}b")]);
        d.style_mut(0).unwrap().alignment = 7;
        let fps = Framerate::from_rational(25, 1, false).unwrap();
        assert_eq!(
            write_transtation(&d, &fps),
            "SUB[9LN 00:00:00:00>00:00:00:24]\r\na\r\n\r\nSUB[9LI 00:00:01:00>00:00:02:00]\r\nb\r\nSUB[\r\n"
        );
    }

    #[test]
    fn ttxt_round_trip() {
        let d = doc_of(&[(1000, 2000, "one\\Ntwo & <three>"), (3000, 4000, "four")]);
        let x = write_ttxt(&d, Time(3000));
        let subs = parse_ttxt(&x).unwrap();
        let got: Vec<_> = subs
            .iter()
            .map(|s| (s.start.0, s.end.0, s.text.as_str()))
            .collect();
        assert_eq!(
            got,
            [(1000, 2000, "one\\Ntwo & <three>"), (3000, 4000, "four")]
        );
    }

    #[test]
    fn ttxt_10_reads_quoted_rows() {
        let x = r#"<TextStream version="1.0"><TextSample sampleTime="00:00:01.000" text="'a b''c'"/><TextSample sampleTime="00:00:02.500" text=""/></TextStream>"#;
        let subs = parse_ttxt(x).unwrap();
        assert_eq!(subs.len(), 1);
        assert_eq!((subs[0].start.0, subs[0].end.0), (1000, 2500));
        assert_eq!(subs[0].text, "a b\\Nc");
        assert!(parse_ttxt("<html/>").is_none());
    }

    #[test]
    fn wrap_points_balanced_and_greedy() {
        let w = [5, 5, 5, 5, 5];
        assert_eq!(wrap_points(&w, 15, 1), [3]);
        assert_eq!(wrap_points(&w, 15, 4), [3]);
        assert_eq!(wrap_points(&w, 100, 4), Vec::<usize>::new());
        // first longer and last longer differ when the words split unevenly
        let w = [4, 4, 4, 4, 4, 4, 4];
        let first = wrap_points(&w, 20, 0);
        let last = wrap_points(&w, 20, 3);
        let rows = |b: &[usize]| {
            let mut v = vec![0];
            v.extend_from_slice(b);
            v.push(w.len());
            v.windows(2).map(|x| x[1] - x[0]).collect::<Vec<_>>()
        };
        assert!(rows(&first)[0] >= rows(&first)[1], "{first:?}");
        assert!(rows(&last)[1] >= rows(&last)[0], "{last:?}");
        assert!(rows(&first).iter().chain(&rows(&last)).all(|&r| r > 0));
    }

    #[test]
    fn iso6937_letters_with_diacritics() {
        let mut out = Vec::new();
        encode_ebu("Zażółć $é€", 0, &mut out);
        assert_eq!(
            out,
            [
                b'Z', b'a', 0xC7, b'z', 0xC2, b'o', 0xF8, 0xC2, b'c', b' ', 0xA4, 0xC2, b'e', b'?'
            ]
        );
        out.clear();
        encode_ebu("Привет", 1, &mut out);
        assert_eq!(out, [0xBF, 0xE0, 0xD8, 0xD2, 0xD5, 0xE2]);
    }

    #[test]
    fn ebu_file_layout() {
        let mut d = doc_of(&[
            (1000, 2000, "{\\i1}Hello{\\i0} world"),
            (3000, 4000, "{\\an8}top"),
        ]);
        d.set_info("Title", "Pokémon");
        let s = EbuSettings {
            tv_standard: 2,
            timecode_offset: [10, 0, 0, 5],
            ..EbuSettings::default()
        };
        let f = write_ebu_stl_at(&d, &s, "261015120000").unwrap();
        assert_eq!(f.len(), 1024 + 2 * 128);
        assert_eq!(&f[0..11], b"850STL25.01");
        assert_eq!(&f[16..23], b"Pok\x82mon");
        assert_eq!(&f[238..248], b"    2    2");
        assert_eq!(&f[256..272], b"1000000510000105");
        let b0 = &f[1024..1152];
        assert_eq!(b0[3], 0xFF);
        // 10:00:00:05 plus one second, out-time inclusive
        assert_eq!(&b0[5..13], &[10, 0, 1, 5, 10, 0, 2, 4]);
        assert_eq!((b0[13], b0[14]), (99, 2));
        assert_eq!(&b0[16..29], b"\x80Hello\x81 world");
        assert_eq!(b0[29], 0x8F);
        let b1 = &f[1152..1280];
        assert_eq!(u16::from_le_bytes([b1[1], b1[2]]), 1);
        assert_eq!((b1[13], b1[14]), (0, 2));
    }

    #[test]
    fn ebu_long_text_continues_in_extension_blocks() {
        let long = "word ".repeat(40);
        let d = doc_of(&[(0, 1000, long.trim_end())]);
        let s = EbuSettings {
            line_wrapping: 1,
            max_line_length: 40,
            ..EbuSettings::default()
        };
        let f = write_ebu_stl_at(&d, &s, "261015120000").unwrap();
        let blocks: Vec<&[u8]> = f[1024..].chunks(128).collect();
        assert_eq!(blocks.len(), 2);
        assert_eq!((blocks[0][3], blocks[1][3]), (0, 0xFF));
        assert!(blocks[0][16..].contains(&0x8A));
        let over = EbuSettings {
            line_wrapping: 2,
            max_line_length: 40,
            ..EbuSettings::default()
        };
        assert!(write_ebu_stl_at(&d, &over, "261015120000").is_err());
        let skip = EbuSettings {
            line_wrapping: 3,
            max_line_length: 40,
            ..EbuSettings::default()
        };
        // nothing left: one blank subtitle
        let f = write_ebu_stl_at(&d, &skip, "261015120000").unwrap();
        assert_eq!(f.len(), 1024 + 128);
        assert_eq!(f[1024 + 16], b' ');
    }
}
