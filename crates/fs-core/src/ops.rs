//! Editing operations shared by the CLI, the GUI and automation.
//!
//! Every function operates on a [`Document`] in place; wrap calls in
//! [`crate::history::Editor::edit`] to make them undoable.

use std::cmp::Ordering;

use regex::{Regex, RegexBuilder};

use crate::ass::Document;
use crate::model::{Event, EventKind};
use crate::tags::{self, Block, DrawCmd, Line, Tag};
use crate::time::Time;
use crate::vfr::{Framerate, TimeType};

// ---------------------------------------------------------------------------
// Row selection helpers
// ---------------------------------------------------------------------------

/// Which rows an operation applies to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Rows {
    All,
    /// Explicit row indices.
    Selected(Vec<usize>),
    /// The given row and every row after it.
    From(usize),
    /// Every row whose start time is >= the given time.
    StartingAt(Time),
}

impl Rows {
    pub fn resolve(&self, doc: &Document) -> Vec<usize> {
        let n = doc.event_count();
        match self {
            Rows::All => (0..n).collect(),
            Rows::Selected(v) => {
                let mut v: Vec<usize> = v.iter().copied().filter(|&i| i < n).collect();
                v.sort_unstable();
                v.dedup();
                v
            }
            Rows::From(i) => (*i.min(&n)..n).collect(),
            Rows::StartingAt(t) => (0..n)
                .filter(|&i| doc.event(i).unwrap().start >= *t)
                .collect(),
        }
    }
}

// ---------------------------------------------------------------------------
// Shifting times
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShiftWhich {
    Both,
    Start,
    End,
}

#[derive(Debug, Clone, Copy)]
pub enum ShiftAmount<'a> {
    Time(Time),
    /// Shift by whole frames using frame-exact conversion.
    Frames(i64, &'a Framerate),
}

/// Shift the times of the given rows. Optionally restricted to a set of style
/// names. Returns the number of changed lines. Negative results are clamped
/// to zero, like Aegisub/Kainote.
pub fn shift_times(
    doc: &mut Document,
    rows: &Rows,
    amount: ShiftAmount<'_>,
    which: ShiftWhich,
    styles: Option<&[String]>,
) -> usize {
    let idx = rows.resolve(doc);
    let mut changed = 0;
    for i in idx {
        let ev = doc.event(i).unwrap();
        if let Some(st) = styles
            && !st.contains(&ev.style)
        {
            continue;
        }
        let (mut s, mut e) = (ev.start, ev.end);
        match amount {
            ShiftAmount::Time(d) => {
                if which != ShiftWhich::End {
                    s = (s + d).max(Time::ZERO);
                }
                if which != ShiftWhich::Start {
                    e = (e + d).max(Time::ZERO);
                }
            }
            ShiftAmount::Frames(n, fr) => {
                if which != ShiftWhich::End {
                    let f = fr.frame_at_time(s.ms(), TimeType::Start) + n;
                    s = Time(fr.time_at_frame(f, TimeType::Start).max(0));
                }
                if which != ShiftWhich::Start {
                    let f = fr.frame_at_time(e.ms(), TimeType::End) + n;
                    e = Time(fr.time_at_frame(f, TimeType::End).max(0));
                }
            }
        }
        if (s, e) != (ev.start, ev.end) {
            let ev = doc.event_mut(i).unwrap();
            ev.start = s;
            ev.end = e;
            changed += 1;
        }
    }
    changed
}

/// Retime a script from one frame rate (CFR or VFR) to another, keeping every
/// line on the same frames (Aegisub's "Transform Framerate" export filter):
/// times inside a frame are interpolated, and the relative times of `\t`,
/// `\move`, `\fad`, `\fade` and karaoke tags are converted too.
pub fn change_fps(doc: &mut Document, from: &Framerate, to: &Framerate) {
    let convert = |t: i64| -> i64 {
        let f = from.frame_at_time(t, TimeType::Exact);
        let (fs, fe) = (
            from.time_at_frame(f, TimeType::Exact),
            from.time_at_frame(f + 1, TimeType::Exact),
        );
        let dist = if fe > fs {
            (t - fs) as f64 / (fe - fs) as f64
        } else {
            0.0
        };
        let (ns, ne) = (
            to.time_at_frame(f, TimeType::Exact),
            to.time_at_frame(f + 1, TimeType::Exact),
        );
        ns + ((ne - ns) as f64 * dist) as i64
    };
    let cs = |t: i64| t.div_euclid(10) * 10;
    for i in 0..doc.event_count() {
        let ev = doc.event_mut(i).unwrap();
        let (old_start, old_end) = (cs(ev.start.ms()), cs(ev.end.ms()));
        let new_start = cs(convert(ev.start.ms()));
        let new_end = cs(convert(ev.end.ms()) + 9);
        let rel_start = |v: i64| {
            let n = convert(old_start + v) - new_start;
            // 0 means "line end" in some tags, so a nonzero time never becomes 0
            if n == 0 && v != 0 { 1 } else { n }
        };
        let (mut old_k, mut new_k) = (0i64, 0i64);
        let mut line = Line::parse(&ev.text);
        line.for_each_tag_mut(|t| {
            let int = |t: &Tag, k: usize| t.arg_f64(k).map(|v| v as i64);
            let starts: &[usize] = match t.name.as_str() {
                "move" if t.args.len() >= 6 => &[4, 5],
                "fade" if t.args.len() >= 7 => &[3, 4, 5, 6],
                "fad" if t.args.len() >= 2 => &[0],
                "t" if t.args.len() >= 3 => &[0, 1],
                "k" | "K" | "kf" | "ko" => {
                    if let Some(v) = int(t, 0) {
                        let start = old_start / 10 + old_k + v;
                        let n = (convert(start * 10) - new_start) / 10 - new_k;
                        old_k += v;
                        new_k += n;
                        t.set_args(vec![n.to_string()]);
                    }
                    &[]
                }
                _ => &[],
            };
            if starts.is_empty() && t.name != "fad" {
                return;
            }
            let mut a = t.args.clone();
            for &k in starts {
                if let Some(v) = int(t, k) {
                    a[k] = rel_start(v).to_string();
                }
            }
            if t.name == "fad"
                && let Some(v) = int(t, 1)
            {
                a[1] = (new_end - convert(old_end - v)).to_string();
            }
            if a != t.args {
                t.set_args(a);
            }
        });
        ev.text = line.to_text();
        ev.start = Time(new_start.max(0));
        ev.end = Time(new_end.max(0));
    }
}

/// Snap start/end times to frame boundaries so every line starts and ends
/// exactly where the video frames change.
pub fn snap_to_frames(doc: &mut Document, rows: &Rows, fr: &Framerate) -> usize {
    let mut n = 0;
    for i in rows.resolve(doc) {
        let ev = doc.event(i).unwrap();
        let s = Time(
            fr.time_at_frame(
                fr.frame_at_time(ev.start.ms(), TimeType::Start),
                TimeType::Start,
            )
            .max(0),
        );
        let e = Time(
            fr.time_at_frame(fr.frame_at_time(ev.end.ms(), TimeType::End), TimeType::End)
                .max(0),
        );
        if (s, e) != (ev.start, ev.end) {
            let ev = doc.event_mut(i).unwrap();
            ev.start = s;
            ev.end = e;
            n += 1;
        }
    }
    n
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SortKey {
    Start,
    End,
    Style,
    Actor,
    Effect,
    Layer,
    Text,
}

/// Stable sort of all events (or only of a contiguous selection range when
/// `rows` is given — rows outside keep their positions).
pub fn sort_events(doc: &mut Document, key: SortKey, rows: Option<&[usize]>) {
    let cmp = |a: &Event, b: &Event| -> Ordering {
        match key {
            SortKey::Start => a.start.cmp(&b.start),
            SortKey::End => a.end.cmp(&b.end),
            SortKey::Style => a.style.cmp(&b.style),
            SortKey::Actor => a.actor.cmp(&b.actor),
            SortKey::Effect => a.effect.cmp(&b.effect),
            SortKey::Layer => a.layer.cmp(&b.layer),
            SortKey::Text => a.text.cmp(&b.text),
        }
    };
    let entries = doc.event_entries_mut();
    match rows {
        None => entries.sort_by(|a, b| cmp(&a.event, &b.event)),
        Some(rows) => {
            let mut rows: Vec<usize> = rows
                .iter()
                .copied()
                .filter(|&r| r < entries.len())
                .collect();
            rows.sort_unstable();
            rows.dedup();
            let mut items: Vec<_> = rows.iter().map(|&r| entries[r].clone()).collect();
            items.sort_by(|a, b| cmp(&a.event, &b.event));
            for (r, it) in rows.into_iter().zip(items) {
                entries[r] = it;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Filtering (a view, not a modification)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default)]
pub struct Filter {
    /// Show only these styles (empty = any).
    pub styles: Vec<String>,
    pub actors: Vec<String>,
    pub effects: Vec<String>,
    /// Text must match (plain substring or regex).
    pub text: Option<TextMatcher>,
    /// `Some(true)`: only comments, `Some(false)`: only dialogue.
    pub comments: Option<bool>,
    /// Restrict to these row indices (e.g. "filter by selection").
    pub rows: Option<Vec<usize>>,
    /// Only lines that overlap another line in time.
    pub only_overlapping: bool,
    /// Lines with CPS above this value.
    pub min_cps: Option<f64>,
    /// Invert the whole filter.
    pub invert: bool,
}

#[derive(Debug, Clone)]
pub struct TextMatcher {
    regex: Regex,
    /// Which field to search.
    pub field: Field,
    /// Match against text with tags stripped.
    pub strip_tags: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Field {
    Text,
    Style,
    Actor,
    Effect,
}

impl TextMatcher {
    pub fn new(
        pattern: &str,
        is_regex: bool,
        case_sensitive: bool,
        field: Field,
    ) -> Result<Self, regex::Error> {
        let pat = if is_regex {
            pattern.to_string()
        } else {
            regex::escape(pattern)
        };
        let regex = RegexBuilder::new(&pat)
            .case_insensitive(!case_sensitive)
            .build()?;
        Ok(TextMatcher {
            regex,
            field,
            strip_tags: false,
        })
    }

    fn haystack<'a>(&self, ev: &'a Event) -> std::borrow::Cow<'a, str> {
        match self.field {
            Field::Text if self.strip_tags => tags::strip_tags(&ev.text).into(),
            Field::Text => (&ev.text).into(),
            Field::Style => (&ev.style).into(),
            Field::Actor => (&ev.actor).into(),
            Field::Effect => (&ev.effect).into(),
        }
    }

    pub fn is_match(&self, ev: &Event) -> bool {
        self.regex.is_match(&self.haystack(ev))
    }
}

impl Filter {
    pub fn is_empty(&self) -> bool {
        self.styles.is_empty()
            && self.actors.is_empty()
            && self.effects.is_empty()
            && self.text.is_none()
            && self.comments.is_none()
            && self.rows.is_none()
            && !self.only_overlapping
            && self.min_cps.is_none()
            && !self.invert
    }

    /// Row indices that pass the filter.
    pub fn apply(&self, doc: &Document) -> Vec<usize> {
        let overlap: Vec<bool> = if self.only_overlapping {
            let mut v = vec![false; doc.event_count()];
            for (a, b) in overlapping_pairs(doc) {
                v[a] = true;
                v[b] = true;
            }
            v
        } else {
            Vec::new()
        };
        (0..doc.event_count())
            .filter(|&i| {
                let ev = doc.event(i).unwrap();
                let ok = (self.styles.is_empty() || self.styles.contains(&ev.style))
                    && (self.actors.is_empty() || self.actors.contains(&ev.actor))
                    && (self.effects.is_empty() || self.effects.contains(&ev.effect))
                    && self.text.as_ref().is_none_or(|m| m.is_match(ev))
                    && self.comments.is_none_or(|c| ev.is_comment() == c)
                    && self.rows.as_ref().is_none_or(|r| r.contains(&i))
                    && (!self.only_overlapping || overlap[i])
                    && self
                        .min_cps
                        .is_none_or(|c| tags::cps(&ev.text, ev.duration().ms()) > c);
                ok != self.invert
            })
            .collect()
    }
}

// ---------------------------------------------------------------------------
// Find & replace
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct Replace {
    pub matcher: TextMatcher,
    /// Replacement; `$1`/`${name}` references groups when using a regex.
    pub replacement: String,
    /// Replace only the first match per line.
    pub first_only: bool,
}

/// Replace in the given rows; returns number of replacements made. When
/// matching with tags stripped is requested, replacements are applied to
/// the plain-text blocks only, so override tags are never damaged.
pub fn replace(doc: &mut Document, rows: &Rows, r: &Replace) -> usize {
    let mut count = 0;
    let literal = !r.replacement.contains('$');
    for i in rows.resolve(doc) {
        let ev = doc.event(i).unwrap();
        let apply = |s: &str, count: &mut usize| -> Option<String> {
            let n = if r.first_only { 1 } else { 0 };
            let found = r.matcher.regex.find_iter(s).count();
            if found == 0 {
                return None;
            }
            *count += if r.first_only { 1 } else { found };
            Some(if literal {
                r.matcher
                    .regex
                    .replacen(s, n, regex::NoExpand(&r.replacement))
                    .into_owned()
            } else {
                r.matcher
                    .regex
                    .replacen(s, n, r.replacement.as_str())
                    .into_owned()
            })
        };
        let new: Option<String> = match r.matcher.field {
            Field::Text if r.matcher.strip_tags => {
                let mut line = Line::parse(&ev.text);
                let mut changed = false;
                for b in &mut line.blocks {
                    if let Block::Text(t) = b
                        && let Some(n) = apply(t, &mut count)
                    {
                        *t = n;
                        changed = true;
                        if r.first_only {
                            break;
                        }
                    }
                }
                changed.then(|| line.to_text())
            }
            _ => apply(&r.matcher.haystack(ev), &mut count),
        };
        if let Some(n) = new {
            let ev = doc.event_mut(i).unwrap();
            match r.matcher.field {
                Field::Text => ev.text = n,
                Field::Style => ev.style = n,
                Field::Actor => ev.actor = n,
                Field::Effect => ev.effect = n,
            }
        }
    }
    count
}

// ---------------------------------------------------------------------------
// Overlaps / collisions
// ---------------------------------------------------------------------------

/// Pairs of dialogue rows overlapping in time (comments ignored). Lines on
/// different layers still count — like Kainote's collision highlight.
pub fn overlapping_pairs(doc: &Document) -> Vec<(usize, usize)> {
    let mut idx: Vec<usize> = (0..doc.event_count())
        .filter(|&i| !doc.event(i).unwrap().is_comment())
        .collect();
    idx.sort_by_key(|&i| doc.event(i).unwrap().start);
    let mut out = Vec::new();
    for (k, &a) in idx.iter().enumerate() {
        let ea = doc.event(a).unwrap();
        for &b in &idx[k + 1..] {
            let eb = doc.event(b).unwrap();
            if eb.start >= ea.end {
                break;
            }
            if eb.end > ea.start && ea.end > ea.start && eb.end > eb.start {
                out.push((a.min(b), a.max(b)));
            }
        }
    }
    out.sort_unstable();
    out
}

/// Rows whose time range overlaps the given row.
pub fn overlapping_with(doc: &Document, row: usize) -> Vec<usize> {
    let Some(e) = doc.event(row) else {
        return Vec::new();
    };
    (0..doc.event_count())
        .filter(|&i| i != row)
        .filter(|&i| {
            let o = doc.event(i).unwrap();
            !o.is_comment() && o.start < e.end && o.end > e.start
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Line structure edits
// ---------------------------------------------------------------------------

/// Duplicate rows (each copy inserted after its original). Returns the new
/// row indices.
pub fn duplicate(doc: &mut Document, rows: &[usize]) -> Vec<usize> {
    let mut rows = rows.to_vec();
    rows.sort_unstable();
    rows.dedup();
    let items = rows
        .iter()
        .filter_map(|&r| Some((r + 1, doc.event(r)?.clone())))
        .collect();
    doc.insert_events(items)
}

pub fn delete_rows(doc: &mut Document, rows: &[usize]) -> usize {
    let mut rows = rows.to_vec();
    rows.sort_unstable();
    rows.dedup();
    doc.remove_events(&rows)
}

/// Remove styles no line uses (Kainote "Delete unused ASS styles"). Styles
/// named by a `\r` reset or the TL-mode style stay. Returns the removed names.
pub fn remove_unused_styles(doc: &mut Document) -> Vec<String> {
    let mut used: std::collections::HashSet<String> = std::collections::HashSet::new();
    for e in doc.events().iter().map(|e| &e.event) {
        used.insert(e.style.clone());
        for t in Line::parse(&e.text).tags() {
            if t.name == "r" && !t.args.is_empty() {
                used.insert(t.args[0].clone());
            }
        }
    }
    used.insert(doc.info("TLMode Style").unwrap_or("TLmode").to_string());
    let gone: Vec<String> = doc
        .styles()
        .filter(|s| !used.contains(&s.name))
        .map(|s| s.name.clone())
        .collect();
    for name in &gone {
        if let Some(i) = doc.style_index(name) {
            doc.remove_style(i);
        }
    }
    gone
}

/// Aegisub's "Fix Styles" export filter: lines whose style is not in the
/// file (compared without case) get `Default`. Returns how many changed.
pub fn fix_styles(doc: &mut Document) -> usize {
    let names: std::collections::HashSet<String> =
        doc.styles().map(|s| s.name.to_lowercase()).collect();
    let mut n = 0;
    for i in 0..doc.event_count() {
        let e = doc.event_mut(i).unwrap();
        if !names.contains(&e.style.to_lowercase()) {
            e.style = "Default".into();
            n += 1;
        }
    }
    n
}

/// Aegisub `trim_text`: drop spaces, tabs and `\N`/`\n`/`\h` at both ends.
fn trim_breaks(text: &str) -> String {
    let mut s = text;
    loop {
        let t = s.trim_matches([' ', '\t']);
        let t = ["\\N", "\\n", "\\h"]
            .iter()
            .fold(t, |t, b| t.strip_prefix(b).unwrap_or(t));
        let t = ["\\N", "\\n", "\\h"]
            .iter()
            .fold(t, |t, b| t.strip_suffix(b).unwrap_or(t));
        if t.len() == s.len() {
            return t.to_string();
        }
        s = t;
    }
}

/// Aegisub "Recombine Lines": undo a split-and-merge of overlapping lines
/// (1, 1+2, 2 → 1, 2 with widened times). Returns the rows left over from
/// `rows`, in document order after the deletions.
pub fn recombine(doc: &mut Document, rows: &[usize]) -> Vec<usize> {
    let mut sel: Vec<usize> = rows
        .iter()
        .copied()
        .filter(|&r| doc.event(r).is_some())
        .collect();
    sel.sort_unstable();
    sel.dedup();
    if sel.len() < 2 {
        return sel;
    }
    sel.sort_by_key(|&r| doc.event(r).unwrap().start);
    // working copies: (start, end, text)
    let mut l: Vec<(Time, Time, String)> = sel
        .iter()
        .map(|&r| {
            let e = doc.event(r).unwrap();
            (e.start, e.end, trim_breaks(&e.text))
        })
        .collect();
    let mut dead = vec![false; l.len()];
    let expand = |l: &mut Vec<(Time, Time, String)>, src: usize, dst: usize| {
        l[dst].0 = l[dst].0.min(l[src].0);
        l[dst].1 = l[dst].1.max(l[src].1);
    };
    // d1 starts (or ends) with d2's text: cut it off and widen d2
    let check = |l: &mut Vec<(Time, Time, String)>, d1: usize, d2: usize, start: bool| {
        let (a, b) = (l[d1].2.clone(), l[d2].2.clone());
        let rest = if start {
            a.strip_prefix(b.as_str())
        } else {
            a.strip_suffix(b.as_str())
        };
        match rest {
            Some(rest) => {
                l[d1].2 = trim_breaks(rest);
                expand(l, d1, d2);
                true
            }
            None => false,
        }
    };
    let end = l.len() - 1;
    for cur in 0..end {
        let mut d2 = cur + 1;
        if l[cur].2 == l[d2].2 {
            expand(&mut l, cur, d2);
            dead[cur] = true;
            continue;
        }
        if l[cur].2.is_empty() {
            dead[cur] = true;
            continue;
        }
        if d2 == end && l[d2].2.is_empty() {
            dead[d2] = true;
            continue;
        }
        while d2 <= end && check(&mut l, d2, cur, true) {
            d2 += 1;
        }
        while d2 <= end && check(&mut l, d2, cur, false) {
            d2 += 1;
        }
        while d2 <= end && check(&mut l, cur, d2, false) {
            d2 += 1;
        }
        while d2 <= end && check(&mut l, cur, d2, true) {
            d2 += 1;
        }
    }
    for (k, &r) in sel.iter().enumerate() {
        let e = doc.event_mut(r).unwrap();
        (e.start, e.end, e.text) = l[k].clone();
    }
    let gone: Vec<usize> = sel
        .iter()
        .zip(&dead)
        .filter(|(_, d)| **d)
        .map(|(r, _)| *r)
        .collect();
    delete_rows(doc, &gone);
    let mut left: Vec<usize> = sel
        .iter()
        .zip(&dead)
        .filter(|(_, d)| !**d)
        .map(|(r, _)| *r)
        .collect();
    left.sort_unstable();
    left.iter()
        .map(|&r| r - gone.iter().filter(|&&g| g < r).count())
        .collect()
}

/// Aegisub "Split Lines (by karaoke)": each `\k` syllable of the rows
/// becomes its own line with the syllable's times. Rows with fewer than
/// two syllables stay. Returns the new rows.
pub fn split_by_karaoke(doc: &mut Document, rows: &[usize]) -> Vec<usize> {
    let mut rows = rows.to_vec();
    rows.sort_unstable();
    rows.dedup();
    let mut out = Vec::new();
    let mut offset = 0;
    for r in rows {
        let r = r + offset;
        let Some(ev) = doc.event(r).cloned() else {
            continue;
        };
        let syls = crate::tags::karaoke_syllables(&ev.text);
        if syls.len() < 2 {
            continue;
        }
        doc.remove_event(r);
        for (k, s) in syls.iter().enumerate() {
            let mut e = ev.clone();
            e.start = ev.start + Time(s.start_ms);
            e.end = e.start + Time(s.duration_ms);
            e.text = s.text.clone();
            doc.insert_event(r + k, e);
            out.push(r + k);
        }
        offset += syls.len() - 1;
    }
    out
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JoinMode {
    /// Concatenate texts with a space (or `\N` if `newline`).
    Concatenate { newline: bool },
    /// Keep only the first line's text, extend timing.
    KeepFirst,
    /// Join as karaoke: each part becomes a `\k` syllable.
    AsKaraoke,
}

/// Join rows into the first of them. Returns the index of the joined row.
pub fn join(doc: &mut Document, rows: &[usize], mode: JoinMode) -> Option<usize> {
    let mut rows = rows.to_vec();
    rows.sort_unstable();
    rows.dedup();
    let first = *rows.first()?;
    if rows.len() < 2 {
        return Some(first);
    }
    let evs: Vec<Event> = rows
        .iter()
        .map(|&r| doc.event(r).unwrap().clone())
        .collect();
    let start = evs.iter().map(|e| e.start).min().unwrap();
    let end = evs.iter().map(|e| e.end).max().unwrap();
    let text = match mode {
        JoinMode::Concatenate { newline } => {
            let sep = if newline { "\\N" } else { " " };
            evs.iter()
                .map(|e| e.text.as_str())
                .filter(|t| !t.is_empty())
                .collect::<Vec<_>>()
                .join(sep)
        }
        JoinMode::KeepFirst => evs[0].text.clone(),
        JoinMode::AsKaraoke => {
            let mut s = String::new();
            let mut pos = start;
            for e in &evs {
                if e.start > pos {
                    s.push_str(&format!("{{\\k{}}}", ((e.start - pos).ms() + 5) / 10));
                }
                s.push_str(&format!(
                    "{{\\k{}}}{}",
                    ((e.end - e.start.max(pos)).ms().max(0) + 5) / 10,
                    tags::strip_tags(&e.text)
                ));
                pos = pos.max(e.end);
            }
            s
        }
    };
    for &r in rows[1..].iter().rev() {
        doc.remove_event(r);
    }
    let ev = doc.event_mut(first).unwrap();
    ev.start = start;
    ev.end = end;
    ev.text = text;
    Some(first)
}

/// Split a row at a byte position of its text. The split time is either
/// given or proportional to the text length (like Aegisub's "split at
/// cursor (estimate times)"). Returns the index of the second part.
pub fn split_at(
    doc: &mut Document,
    row: usize,
    byte_pos: usize,
    at: Option<Time>,
) -> Option<usize> {
    let ev = doc.event(row)?.clone();
    let pos = byte_pos.min(ev.text.len());
    if !ev.text.is_char_boundary(pos) {
        return None;
    }
    let (a, b) = ev.text.split_at(pos);
    let a = a.trim_end().trim_end_matches("\\N").trim_end().to_string();
    let b = b
        .trim_start()
        .trim_start_matches("\\N")
        .trim_start()
        .to_string();
    let mid = at.unwrap_or_else(|| {
        let la = tags::strip_tags(&a).chars().count() as i64;
        let lb = tags::strip_tags(&b).chars().count() as i64;
        let total = (la + lb).max(1);
        ev.start + Time(ev.duration().ms() * la / total)
    });
    let mid = mid.clamp(ev.start, ev.end);
    {
        let e1 = doc.event_mut(row).unwrap();
        e1.text = a;
        e1.end = mid;
    }
    let mut e2 = ev;
    e2.text = b;
    e2.start = mid;
    doc.insert_event(row + 1, e2);
    Some(row + 1)
}

/// Swap the text of two rows (Kainote "swap lines").
pub fn swap_rows(doc: &mut Document, a: usize, b: usize) {
    if a == b || a >= doc.event_count() || b >= doc.event_count() {
        return;
    }
    doc.event_entries_mut().swap(a, b);
}

/// Toggle comment state of rows.
pub fn set_comment(doc: &mut Document, rows: &[usize], comment: Option<bool>) {
    for &r in rows {
        if let Some(ev) = doc.event_mut(r) {
            let c = comment.unwrap_or(!ev.is_comment());
            ev.kind = if c {
                EventKind::Comment
            } else {
                EventKind::Dialogue
            };
        }
    }
}

/// Make each row's end equal the next row's start (continuous timing).
pub fn make_continuous(doc: &mut Document, rows: &[usize], use_next_start: bool) {
    let mut rows = rows.to_vec();
    rows.sort_unstable();
    for w in rows.windows(2) {
        let (a, b) = (w[0], w[1]);
        let (ea, eb) = (doc.event(a).unwrap().clone(), doc.event(b).unwrap().clone());
        if use_next_start {
            doc.event_mut(a).unwrap().end = eb.start;
        } else {
            doc.event_mut(b).unwrap().start = ea.end;
        }
    }
}

// ---------------------------------------------------------------------------
// Timing post-processor
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default)]
pub struct PostProcess<'a> {
    pub lead_in: Option<Time>,
    pub lead_out: Option<Time>,
    /// Link lines whose gap is below `adjacent_threshold`; `adjacent_bias`
    /// (0..1) chooses where in the gap they meet (0 = previous end, 1 = next start).
    pub adjacent_threshold: Option<Time>,
    pub adjacent_bias: f64,
    /// Snap to keyframes within these distances (ms).
    pub keyframes: Option<(&'a [i64], &'a Framerate)>,
    pub kf_before_start: Time,
    pub kf_after_start: Time,
    pub kf_before_end: Time,
    pub kf_after_end: Time,
    /// Only process these styles (empty = all).
    pub styles: Vec<String>,
}

/// Aegisub-style timing post-processor. Applies lead-in/out, then links
/// adjacent lines, then snaps to keyframes.
pub fn post_process(doc: &mut Document, rows: &Rows, pp: &PostProcess<'_>) {
    let mut idx: Vec<usize> = rows
        .resolve(doc)
        .into_iter()
        .filter(|&i| {
            let e = doc.event(i).unwrap();
            !e.is_comment() && (pp.styles.is_empty() || pp.styles.contains(&e.style))
        })
        .collect();
    let orig: Vec<(Time, Time)> = idx
        .iter()
        .map(|&i| (doc.event(i).unwrap().start, doc.event(i).unwrap().end))
        .collect();
    // lead in/out — but never past the neighbouring line's original time
    idx.sort_by_key(|&i| doc.event(i).unwrap().start);
    for (k, &i) in idx.iter().enumerate() {
        let ev = doc.event(i).unwrap().clone();
        let mut s = ev.start;
        let mut e = ev.end;
        if let Some(li) = pp.lead_in {
            let limit = if k > 0 {
                doc.event(idx[k - 1]).unwrap().end.min(s)
            } else {
                Time::ZERO
            };
            s = (s - li).max(limit).max(Time::ZERO);
        }
        if let Some(lo) = pp.lead_out {
            let limit = idx.get(k + 1).map(|&n| doc.event(n).unwrap().start.max(e));
            e += lo;
            if let Some(l) = limit {
                e = e.min(l);
            }
        }
        let evm = doc.event_mut(i).unwrap();
        evm.start = s;
        evm.end = e;
    }
    // adjacent linking
    if let Some(th) = pp.adjacent_threshold {
        for w in idx.windows(2) {
            let (a, b) = (w[0], w[1]);
            let (ae, bs) = (doc.event(a).unwrap().end, doc.event(b).unwrap().start);
            let gap = bs - ae;
            if gap.ms() >= 0 && gap <= th || (gap.ms() < 0 && -gap <= th) {
                let meet =
                    ae + Time((gap.ms() as f64 * pp.adjacent_bias.clamp(0.0, 1.0)).round() as i64);
                doc.event_mut(a).unwrap().end = meet;
                doc.event_mut(b).unwrap().start = meet;
            }
        }
    }
    // keyframe snapping
    if let Some((kfs, fr)) = pp.keyframes {
        let kf_times: Vec<i64> = kfs
            .iter()
            .map(|&f| fr.time_at_frame(f, TimeType::Start))
            .collect();
        let snap = |t: Time, before: Time, after: Time| -> Time {
            let mut best: Option<i64> = None;
            for &k in &kf_times {
                let d = k - t.ms();
                let ok = (d <= 0 && -d <= before.ms()) || (d >= 0 && d <= after.ms());
                if ok && best.is_none_or(|b| (b - t.ms()).abs() > d.abs()) {
                    best = Some(k);
                }
            }
            best.map(Time).unwrap_or(t)
        };
        for &i in &idx {
            let ev = doc.event(i).unwrap().clone();
            let s = snap(ev.start, pp.kf_before_start, pp.kf_after_start);
            let mut e = snap(ev.end, pp.kf_before_end, pp.kf_after_end);
            if e <= s {
                e = ev.end.max(s);
            }
            let evm = doc.event_mut(i).unwrap();
            evm.start = s;
            evm.end = e;
        }
    }
    let _ = orig;
}

// ---------------------------------------------------------------------------
// Tag cleanup
// ---------------------------------------------------------------------------

/// Remove all override tags (keeping `\N`), optionally keeping comments.
pub fn strip_all_tags(doc: &mut Document, rows: &Rows) {
    for i in rows.resolve(doc) {
        let t = tags::strip_tags(&doc.event(i).unwrap().text);
        doc.event_mut(i).unwrap().text = t;
    }
}

/// Remove `{comment}` blocks only.
pub fn strip_comments(doc: &mut Document, rows: &Rows) {
    for i in rows.resolve(doc) {
        let mut line = Line::parse(&doc.event(i).unwrap().text);
        let n = line.blocks.len();
        line.blocks.retain(|b| !matches!(b, Block::Comment(_)));
        if line.blocks.len() != n {
            doc.event_mut(i).unwrap().text = line.to_text();
        }
    }
}

// ---------------------------------------------------------------------------
// Resampling
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct Resample {
    pub from: (i32, i32),
    pub to: (i32, i32),
    /// When the aspect ratio changes: stretch (scale X and Y independently),
    /// or keep proportions (scale by Y and add horizontal offset).
    pub stretch: bool,
    /// Extra margin added on each side of the destination (letterbox).
    pub margin: (i32, i32, i32, i32),
}

/// Change the script resolution, scaling styles, margins, positions, clips,
/// drawings and size-related tags, like Aegisub's "Resample Resolution".
pub fn resample(doc: &mut Document, r: &Resample) {
    let (fw, fh) = (r.from.0.max(1) as f64, r.from.1.max(1) as f64);
    let (ml, mr, mt, mb) = r.margin;
    let (tw, th) = (
        (r.to.0 - ml - mr).max(1) as f64,
        (r.to.1 - mt - mb).max(1) as f64,
    );
    let ry = th / fh;
    let rx = if r.stretch { tw / fw } else { ry };
    let ox = if r.stretch {
        ml as f64
    } else {
        ml as f64 + (tw - fw * rx) / 2.0
    };
    let oy = mt as f64;
    // horizontal stretch relative to vertical, applied to \fscx
    let ar = rx / ry;
    let px = |x: f64| x * rx + ox;
    let py = |y: f64| y * ry + oy;

    // styles
    let n = doc.style_count();
    for i in 0..n {
        let st = doc.style_mut(i).unwrap();
        st.size *= ry;
        st.outline *= ry;
        st.shadow *= ry;
        st.spacing *= rx;
        st.scale_x *= ar;
        st.margin_l = (st.margin_l as f64 * rx).round() as i32 + ml;
        st.margin_r = (st.margin_r as f64 * rx).round() as i32 + mr;
        st.margin_v = (st.margin_v as f64 * ry).round() as i32 + mt;
        st.margin_b = (st.margin_b as f64 * ry).round() as i32 + mb;
    }

    // events
    for i in 0..doc.event_count() {
        let ev = doc.event(i).unwrap();
        let mut line = Line::parse(&ev.text);
        let scale_tag = |t: &mut Tag| {
            let num = |t: &Tag, k: usize| t.arg_f64(k).unwrap_or(0.0);
            let s = |v: f64| crate::model::fmt_f64((v * 1000.0).round() / 1000.0);
            match t.name.as_str() {
                "pos" | "org" if t.args.len() >= 2 => {
                    let a = vec![s(px(num(t, 0))), s(py(num(t, 1)))];
                    t.set_args(a);
                }
                "move" if t.args.len() >= 4 => {
                    let mut a = vec![
                        s(px(num(t, 0))),
                        s(py(num(t, 1))),
                        s(px(num(t, 2))),
                        s(py(num(t, 3))),
                    ];
                    a.extend(t.args[4..].iter().cloned());
                    t.set_args(a);
                }
                "clip" | "iclip" if t.args.len() == 4 => {
                    let a = vec![
                        s(px(num(t, 0))),
                        s(py(num(t, 1))),
                        s(px(num(t, 2))),
                        s(py(num(t, 3))),
                    ];
                    t.set_args(a);
                }
                "clip" | "iclip" => {
                    let (scale, d) = if t.args.len() == 2 {
                        (t.arg_f64(0).unwrap_or(1.0).max(1.0), t.args[1].clone())
                    } else {
                        (1.0, t.args.first().cloned().unwrap_or_default())
                    };
                    let f = 2f64.powf(scale - 1.0);
                    let mut cmds = tags::parse_drawing(&d);
                    tags::transform_drawing(&mut cmds, |x, y| (px(x / f) * f, py(y / f) * f));
                    let mut a = t.args.clone();
                    *a.last_mut().unwrap() = tags::write_drawing(&cmds);
                    t.set_args(a);
                }
                "fs" | "bord" | "shad" | "be" | "blur" if !t.args.is_empty() => {
                    if let Some(v) = t.arg_f64(0) {
                        let f = if matches!(t.name.as_str(), "be") {
                            1.0
                        } else {
                            ry
                        };
                        t.set_args(vec![s(v * f)]);
                    }
                }
                "xbord" | "xshad" | "fsp" => {
                    if let Some(v) = t.arg_f64(0) {
                        t.set_args(vec![s(v * rx)]);
                    }
                }
                "ybord" | "yshad" => {
                    if let Some(v) = t.arg_f64(0) {
                        t.set_args(vec![s(v * ry)]);
                    }
                }
                "fscx" if ar != 1.0 => {
                    if let Some(v) = t.arg_f64(0) {
                        t.set_args(vec![s(v * ar)]);
                    }
                }
                _ => {}
            }
        };
        let mut changed = false;
        for b in &mut line.blocks {
            match b {
                Block::Override { tags, raw } => {
                    for t in tags.iter_mut() {
                        let before = t.clone();
                        if t.name == "t" {
                            if let Some(mut inner) = t.transform_tags() {
                                inner.iter_mut().for_each(&scale_tag);
                                let mut a = t.args.clone();
                                *a.last_mut().unwrap() =
                                    inner.iter().map(|x| x.to_text()).collect();
                                t.set_args(a);
                            }
                        } else {
                            scale_tag(t);
                        }
                        if *t != before {
                            *raw = None;
                            changed = true;
                        }
                    }
                }
                Block::Drawing { commands, .. } => {
                    // drawings are positioned via \pos; scale their coordinates
                    let mut cmds = tags::parse_drawing(commands);
                    if !cmds.is_empty() {
                        tags::transform_drawing(&mut cmds, |x, y| (x * rx, y * ry));
                        *commands = tags::write_drawing(&cmds);
                        changed = true;
                    }
                }
                _ => {}
            }
        }
        let new_text = changed.then(|| line.to_text());
        let ev = doc.event_mut(i).unwrap();
        if let Some(t) = new_text {
            ev.text = t;
        }
        if ev.margin_l != 0 {
            ev.margin_l = (ev.margin_l as f64 * rx).round() as i32;
        }
        if ev.margin_r != 0 {
            ev.margin_r = (ev.margin_r as f64 * rx).round() as i32;
        }
        if ev.margin_v != 0 {
            ev.margin_v = (ev.margin_v as f64 * ry).round() as i32;
        }
        if ev.margin_b != 0 {
            ev.margin_b = (ev.margin_b as f64 * ry).round() as i32;
        }
    }
    doc.set_info("PlayResX", &r.to.0.to_string());
    doc.set_info("PlayResY", &r.to.1.to_string());
}

// ---------------------------------------------------------------------------
// Positions (for visual tools / "move all")
// ---------------------------------------------------------------------------

/// What the position shifter moves (Kainote's toggles under the tool).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MoveTargets {
    pub pos: bool,
    pub move_start: bool,
    pub move_end: bool,
    pub clip: bool,
    /// Points of `\p` drawings (relative to the line position).
    pub drawing: bool,
    pub org: bool,
}

impl MoveTargets {
    /// Everything except drawing points, which already follow \pos.
    pub const DEFAULT: MoveTargets = MoveTargets {
        pos: true,
        move_start: true,
        move_end: true,
        clip: true,
        drawing: false,
        org: true,
    };
}

/// Translate \pos, \move, \org, \clip and drawings in the given rows.
pub fn translate_positions(doc: &mut Document, rows: &Rows, dx: f64, dy: f64, what: MoveTargets) {
    for i in rows.resolve(doc) {
        let mut line = Line::parse(&doc.event(i).unwrap().text);
        let s = |v: f64| crate::model::fmt_f64((v * 1000.0).round() / 1000.0);
        line.for_each_tag_mut(|t| {
            let n = |k: usize| t.arg_f64(k).unwrap_or(0.0);
            match t.name.as_str() {
                "pos" if what.pos && t.args.len() >= 2 => {
                    let a = vec![s(n(0) + dx), s(n(1) + dy)];
                    t.set_args(a)
                }
                "org" if what.org && t.args.len() >= 2 => {
                    let a = vec![s(n(0) + dx), s(n(1) + dy)];
                    t.set_args(a)
                }
                "move" if (what.move_start || what.move_end) && t.args.len() >= 4 => {
                    let (sx, sy) = if what.move_start {
                        (dx, dy)
                    } else {
                        (0.0, 0.0)
                    };
                    let (ex, ey) = if what.move_end { (dx, dy) } else { (0.0, 0.0) };
                    let mut a = vec![s(n(0) + sx), s(n(1) + sy), s(n(2) + ex), s(n(3) + ey)];
                    a.extend(t.args[4..].iter().cloned());
                    t.set_args(a)
                }
                _ if !what.clip => {}
                "clip" | "iclip" if t.args.len() == 4 => {
                    let a = vec![s(n(0) + dx), s(n(1) + dy), s(n(2) + dx), s(n(3) + dy)];
                    t.set_args(a)
                }
                "clip" | "iclip" => {
                    let scale = if t.args.len() == 2 {
                        t.arg_f64(0).unwrap_or(1.0).max(1.0)
                    } else {
                        1.0
                    };
                    let f = 2f64.powf(scale - 1.0);
                    let mut cmds =
                        tags::parse_drawing(t.args.last().map(String::as_str).unwrap_or(""));
                    tags::transform_drawing(&mut cmds, |x, y| (x + dx * f, y + dy * f));
                    let mut a = t.args.clone();
                    if let Some(l) = a.last_mut() {
                        *l = tags::write_drawing(&cmds);
                    }
                    t.set_args(a)
                }
                _ => {}
            }
        });
        if what.drawing {
            for b in &mut line.blocks {
                if let Block::Drawing { scale, commands } = b {
                    let f = 2f64.powi(scale.saturating_sub(1) as i32);
                    let mut cmds = tags::parse_drawing(commands);
                    tags::transform_drawing(&mut cmds, |x, y| (x + dx * f, y + dy * f));
                    *commands = tags::write_drawing(&cmds);
                }
            }
        }
        let t = line.to_text();
        if t != doc.event(i).unwrap().text {
            doc.event_mut(i).unwrap().text = t;
        }
    }
}

/// Used by the drawing tool: drawing commands of the first drawing block.
pub fn drawing_of(text: &str) -> Option<Vec<DrawCmd>> {
    Line::parse(text).blocks.iter().find_map(|b| match b {
        Block::Drawing { commands, .. } => Some(tags::parse_drawing(commands)),
        _ => None,
    })
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Default, PartialEq)]
pub struct Stats {
    pub dialogue: usize,
    pub comments: usize,
    pub styles: usize,
    pub duration: Time,
    pub max_cps: f64,
    pub lines_over_cps: usize,
    /// Lines colliding with another line of the same style and layer
    /// (typesetting overlaps on purpose, so other collisions don't count).
    pub overlaps: usize,
    pub characters: usize,
    pub used_styles: Vec<String>,
    pub missing_styles: Vec<String>,
}

pub fn stats(doc: &Document, cps_limit: f64) -> Stats {
    let mut s = Stats {
        styles: doc.style_count(),
        ..Default::default()
    };
    let mut used = std::collections::BTreeSet::new();
    for e in doc.events() {
        let ev = &e.event;
        if ev.is_comment() {
            s.comments += 1;
            continue;
        }
        s.dialogue += 1;
        s.duration = s.duration.max(ev.end);
        let c = tags::cps(&ev.text, ev.duration().ms());
        s.max_cps = s.max_cps.max(c);
        if c > cps_limit {
            s.lines_over_cps += 1;
        }
        s.characters += tags::char_count(&ev.text, false, false);
        used.insert(ev.style.clone());
    }
    let mut colliding = std::collections::BTreeSet::new();
    for (a, b) in overlapping_pairs(doc) {
        let (ea, eb) = (doc.event(a).unwrap(), doc.event(b).unwrap());
        if ea.style == eb.style && ea.layer == eb.layer {
            colliding.insert(a);
            colliding.insert(b);
        }
    }
    s.overlaps = colliding.len();
    s.missing_styles = used
        .iter()
        .filter(|n| doc.style(n).is_none())
        .cloned()
        .collect();
    s.used_styles = used.into_iter().collect();
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    fn doc(lines: &[(&str, &str, &str, &str)]) -> Document {
        let mut s = String::from(
            "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
        );
        for (st, en, style, text) in lines {
            s.push_str(&format!("Dialogue: 0,{st},{en},{style},,0,0,0,,{text}\n"));
        }
        Document::parse(&s)
    }

    #[test]
    fn shift_ms_and_frames() {
        let mut d = doc(&[
            ("0:00:01.00", "0:00:02.00", "Default", "a"),
            ("0:00:00.10", "0:00:00.50", "Default", "b"),
        ]);
        assert_eq!(
            shift_times(
                &mut d,
                &Rows::All,
                ShiftAmount::Time(Time(-500)),
                ShiftWhich::Both,
                None
            ),
            2
        );
        assert_eq!(d.event(0).unwrap().start, Time(500));
        assert_eq!(d.event(1).unwrap().start, Time(0)); // clamped
        let fr = Framerate::from_rational(24000, 1001, true).unwrap();
        let mut d = doc(&[("0:00:01.00", "0:00:02.00", "Default", "a")]);
        shift_times(
            &mut d,
            &Rows::All,
            ShiftAmount::Frames(24, &fr),
            ShiftWhich::Both,
            None,
        );
        let e = d.event(0).unwrap();
        let sf = fr.frame_at_time(e.start.round_cs().ms(), TimeType::Start);
        let ef = fr.frame_at_time(e.end.round_cs().ms(), TimeType::End);
        assert_eq!(sf, fr.frame_at_time(1000, TimeType::Start) + 24);
        assert_eq!(ef, fr.frame_at_time(2000, TimeType::End) + 24);
    }

    #[test]
    fn shift_restricted() {
        let mut d = doc(&[
            ("0:00:01.00", "0:00:02.00", "Default", "a"),
            ("0:00:01.00", "0:00:02.00", "Sign", "b"),
        ]);
        let st = vec!["Sign".to_string()];
        shift_times(
            &mut d,
            &Rows::All,
            ShiftAmount::Time(Time(1000)),
            ShiftWhich::Start,
            Some(&st),
        );
        assert_eq!(d.event(0).unwrap().start, Time(1000));
        assert_eq!(d.event(1).unwrap().start, Time(2000));
        assert_eq!(d.event(1).unwrap().end, Time(2000));
    }

    #[test]
    fn sort_and_filter() {
        let mut d = doc(&[
            ("0:00:03.00", "0:00:04.00", "Default", "c"),
            ("0:00:01.00", "0:00:02.50", "Sign", "a"),
            ("0:00:02.00", "0:00:03.00", "Default", "b"),
        ]);
        sort_events(&mut d, SortKey::Start, None);
        assert_eq!(
            d.events()
                .iter()
                .map(|e| e.event.text.as_str())
                .collect::<String>(),
            "abc"
        );
        let f = Filter {
            styles: vec!["Default".into()],
            ..Default::default()
        };
        assert_eq!(f.apply(&d), vec![1, 2]);
        let f = Filter {
            only_overlapping: true,
            ..Default::default()
        };
        assert_eq!(f.apply(&d), vec![0, 1]);
        let f = Filter {
            text: Some(TextMatcher::new("^[ab]$", true, true, Field::Text).unwrap()),
            invert: true,
            ..Default::default()
        };
        assert_eq!(f.apply(&d), vec![2]);
    }

    #[test]
    fn find_replace_preserves_tags() {
        let mut d = doc(&[(
            "0:00:00.00",
            "0:00:01.00",
            "Default",
            r"{\i1}bob is\N{\i0}bob",
        )]);
        let mut m = TextMatcher::new("b", false, true, Field::Text).unwrap();
        m.strip_tags = true;
        let n = replace(
            &mut d,
            &Rows::All,
            &Replace {
                matcher: m,
                replacement: "$".into(),
                first_only: false,
            },
        );
        assert_eq!(n, 4);
        assert_eq!(d.event(0).unwrap().text, r"{\i1}$o$ is\N{\i0}$o$");
        let m = TextMatcher::new(r"(\w+) is", true, true, Field::Text).unwrap();
        replace(
            &mut d,
            &Rows::All,
            &Replace {
                matcher: m,
                replacement: "${1}!".into(),
                first_only: true,
            },
        );
        assert_eq!(d.event(0).unwrap().text, r"{\i1}$o$ is\N{\i0}$o$");
    }

    #[test]
    fn join_split() {
        let mut d = doc(&[
            ("0:00:00.00", "0:00:01.00", "Default", "Hello"),
            ("0:00:01.00", "0:00:03.00", "Default", "world"),
        ]);
        join(&mut d, &[0, 1], JoinMode::Concatenate { newline: false });
        assert_eq!(d.event_count(), 1);
        assert_eq!(d.event(0).unwrap().text, "Hello world");
        assert_eq!(d.event(0).unwrap().end, Time(3000));
        split_at(&mut d, 0, 5, None);
        assert_eq!(d.event_count(), 2);
        assert_eq!(d.event(0).unwrap().text, "Hello");
        assert_eq!(d.event(1).unwrap().text, "world");
        assert_eq!(d.event(0).unwrap().end, Time(1500));
        assert_eq!(d.event(1).unwrap().start, Time(1500));
    }

    #[test]
    fn karaoke_join() {
        let mut d = doc(&[
            ("0:00:00.00", "0:00:00.50", "Default", "ka"),
            ("0:00:00.70", "0:00:01.00", "Default", "ra"),
        ]);
        join(&mut d, &[0, 1], JoinMode::AsKaraoke);
        assert_eq!(d.event(0).unwrap().text, r"{\k50}ka{\k20}{\k30}ra");
    }

    #[test]
    fn post_processor() {
        let mut d = doc(&[
            ("0:00:01.00", "0:00:02.00", "Default", "a"),
            ("0:00:02.20", "0:00:03.00", "Default", "b"),
        ]);
        let pp = PostProcess {
            lead_in: Some(Time(100)),
            lead_out: Some(Time(50)),
            adjacent_threshold: Some(Time(300)),
            adjacent_bias: 0.5,
            ..Default::default()
        };
        post_process(&mut d, &Rows::All, &pp);
        assert_eq!(d.event(0).unwrap().start, Time(900));
        // a ends 2050, b starts 2100 → meet in the middle
        assert_eq!(d.event(0).unwrap().end, Time(2075));
        assert_eq!(d.event(1).unwrap().start, Time(2075));
    }

    #[test]
    fn keyframe_snap() {
        let fr = Framerate::from_fps(10.0).unwrap();
        let kfs = [0i64, 10, 25];
        let mut d = doc(&[("0:00:00.95", "0:00:02.40", "Default", "a")]);
        let pp = PostProcess {
            keyframes: Some((&kfs, &fr)),
            kf_before_start: Time(100),
            kf_after_start: Time(100),
            kf_before_end: Time(200),
            kf_after_end: Time(200),
            ..Default::default()
        };
        post_process(&mut d, &Rows::All, &pp);
        assert_eq!(
            d.event(0).unwrap().start,
            Time(fr.time_at_frame(10, TimeType::Start))
        );
        assert_eq!(
            d.event(0).unwrap().end,
            Time(fr.time_at_frame(25, TimeType::Start))
        );
    }

    #[test]
    fn resample_doubles() {
        let mut d = doc(&[(
            "0:00:00.00",
            "0:00:01.00",
            "Default",
            r"{\pos(100,50)\fs30\clip(0,0,10,10)\p1}m 0 0 l 10 10",
        )]);
        resample(
            &mut d,
            &Resample {
                from: (640, 360),
                to: (1280, 720),
                stretch: false,
                margin: (0, 0, 0, 0),
            },
        );
        assert_eq!(
            d.event(0).unwrap().text,
            r"{\pos(200,100)\fs60\clip(0,0,20,20)\p1}m 0 0 l 20 20"
        );
        let st = d.style("Default").unwrap();
        assert_eq!(st.size, 40.0);
        assert_eq!(st.margin_l, 20);
        assert_eq!(d.play_res(), (1280, 720));
    }

    #[test]
    fn translate() {
        let mut d = doc(&[(
            "0:00:00.00",
            "0:00:01.00",
            "Default",
            r"{\move(1,2,3,4,0,100)\clip(m 0 0 l 1 1)}x",
        )]);
        translate_positions(&mut d, &Rows::All, 10.0, -1.0, MoveTargets::DEFAULT);
        assert_eq!(
            d.event(0).unwrap().text,
            r"{\move(11,1,13,3,0,100)\clip(m 10 -1 l 11 0)}x"
        );
        let only_end = MoveTargets {
            pos: false,
            move_start: false,
            clip: false,
            ..MoveTargets::DEFAULT
        };
        translate_positions(&mut d, &Rows::All, 1.0, 1.0, only_end);
        assert_eq!(
            d.event(0).unwrap().text,
            r"{\move(11,1,14,4,0,100)\clip(m 10 -1 l 11 0)}x"
        );
        let mut d = doc(&[("0:00:00.00", "0:00:01.00", "Default", r"{\p2}m 0 0 l 4 4")]);
        let drawing = MoveTargets {
            drawing: true,
            ..MoveTargets::DEFAULT
        };
        translate_positions(&mut d, &Rows::All, 1.0, 2.0, drawing);
        assert_eq!(d.event(0).unwrap().text, r"{\p2}m 2 4 l 6 8");
    }

    #[test]
    fn change_fps_converts_tag_times() {
        let mut d = doc(&[(
            "0:00:01.00",
            "0:00:02.00",
            "Default",
            r"{\move(0,0,1,1,200,400)\fad(200,400)\k50}a{\k50}b",
        )]);
        let fr = |n| Framerate::from_rational(n, 1, false).unwrap();
        change_fps(&mut d, &fr(25), &fr(50));
        let e = d.event(0).unwrap();
        assert_eq!((e.start.ms(), e.end.ms()), (500, 1000));
        assert_eq!(e.text, r"{\move(0,0,1,1,100,200)\fad(100,200)\k25}a{\k25}b");
    }

    #[test]
    fn fix_styles_defaults_missing() {
        let mut d = doc(&[
            ("0:00:00.00", "0:00:01.00", "default", "kept, case differs"),
            ("0:00:00.00", "0:00:01.00", "Gone", "fixed"),
        ]);
        assert_eq!(fix_styles(&mut d), 1);
        assert_eq!(d.event(0).unwrap().style, "default");
        assert_eq!(d.event(1).unwrap().style, "Default");
    }

    #[test]
    fn unused_styles_go() {
        let mut d = doc(&[("0:00:00.00", "0:00:01.00", "Default", r"{\rAlt}x")]);
        for name in ["Alt", "Unused"] {
            let mut st = d.style("Default").unwrap().clone();
            st.name = name.into();
            d.add_style(st);
        }
        assert_eq!(remove_unused_styles(&mut d), ["Unused"]);
        assert_eq!(d.style_count(), 2);
    }

    #[test]
    fn recombine_split_merge() {
        // 1, 1+2, 2 (the merged middle line spans both)
        let mut d = doc(&[
            ("0:00:01.00", "0:00:02.00", "Default", "one"),
            ("0:00:02.00", "0:00:03.00", "Default", r"one\Ntwo"),
            ("0:00:03.00", "0:00:04.00", "Default", "two"),
        ]);
        let left = recombine(&mut d, &[0, 1, 2]);
        let t = |i: usize| {
            let e = d.event(i).unwrap();
            (e.start.ms(), e.end.ms(), e.text.clone())
        };
        assert_eq!(left, [0, 1]);
        assert_eq!(d.events().len(), 2);
        assert_eq!(t(0), (1000, 3000, "one".into()));
        assert_eq!(t(1), (2000, 4000, "two".into()));
    }

    #[test]
    fn split_karaoke_lines() {
        let mut d = doc(&[
            (
                "0:00:01.00",
                "0:00:02.00",
                "Default",
                r"{\k20}ka{\k30\i1}ra",
            ),
            ("0:00:05.00", "0:00:06.00", "Default", "plain"),
        ]);
        assert_eq!(split_by_karaoke(&mut d, &[0, 1]), [0, 1]);
        let e = |i: usize| d.event(i).unwrap().clone();
        assert_eq!(
            (e(0).start.ms(), e(0).end.ms(), e(0).text),
            (1000, 1200, "ka".into())
        );
        assert_eq!((e(1).start.ms(), e(1).end.ms()), (1200, 1500));
        assert_eq!(e(2).text, "plain");
    }

    #[test]
    fn statistics() {
        let d = doc(&[
            (
                "0:00:00.00",
                "0:00:01.00",
                "Default",
                "abcdefghijklmnopqrstuvwxyz",
            ),
            ("0:00:00.50", "0:00:02.00", "Missing", "x"),
            ("0:00:00.80", "0:00:03.00", "Default", "y"),
        ]);
        let s = stats(&d, 20.0);
        assert_eq!(s.dialogue, 3);
        assert_eq!(s.lines_over_cps, 1);
        assert_eq!(s.overlaps, 2);
        assert_eq!(s.missing_styles, vec!["Missing".to_string()]);
    }
}
