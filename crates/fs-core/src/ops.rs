//! Editing operations shared by the CLI, the GUI and automation.
//!
//! Every function operates on a [`Document`] in place; wrap calls in
//! [`crate::history::Editor::edit`] to make them undoable.

use std::cmp::Ordering;
use std::ops::Range;

use regex::{Captures, Regex, RegexBuilder};

use crate::ass::Document;
use crate::color::{Color, MatrixConversion};
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
    /// Match only inside the `{...}` blocks (Kainote's "Skip text"); wins over `strip_tags`.
    pub only_tags: bool,
    /// The user wrote a regex: `$1` in a replacement refers to its groups. Otherwise
    /// replacements are literal.
    pub is_regex: bool,
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
            only_tags: false,
            is_regex,
        })
    }

    /// The value of the field searched.
    pub fn field_value<'a>(&self, ev: &'a Event) -> &'a str {
        match self.field {
            Field::Text => &ev.text,
            Field::Style => &ev.style,
            Field::Actor => &ev.actor,
            Field::Effect => &ev.effect,
        }
    }

    pub fn set_field(&self, ev: &mut Event, v: String) {
        *match self.field {
            Field::Text => &mut ev.text,
            Field::Style => &mut ev.style,
            Field::Actor => &mut ev.actor,
            Field::Effect => &mut ev.effect,
        } = v;
    }

    fn haystack<'a>(&self, ev: &'a Event) -> std::borrow::Cow<'a, str> {
        match self.field {
            Field::Text if self.strip_tags => tags::strip_tags(&ev.text).into(),
            _ => self.field_value(ev).into(),
        }
    }

    /// `f` for each match in `text`, a value of the field, from byte `from` on, as a range of
    /// `text`, with the text matched and its groups when `groups`, until `f` says stop. With
    /// `strip_tags` the text is searched without its `{...}` blocks (Aegisub's skip tags: a match
    /// may span a block, `^` and `$` are the ends of what shows); with `only_tags`, inside each
    /// block.
    fn each_match(
        &self,
        text: &str,
        from: usize,
        groups: bool,
        mut f: impl FnMut(Range<usize>, &str, Option<&Captures>) -> bool,
    ) {
        if self.field != Field::Text || !(self.strip_tags || self.only_tags) {
            scan(&self.regex, text, from, groups, |m, c| {
                f(m.clone(), &text[m], c)
            });
        } else if self.only_tags {
            for b in tags::block_spans(text) {
                let at = from.saturating_sub(b.start);
                let block = &text[b.clone()];
                if b.end >= from
                    && !scan(&self.regex, block, at, groups, |m, c| {
                        f(b.start + m.start..b.start + m.end, &block[m], c)
                    })
                {
                    return;
                }
            }
        } else {
            let (hay, runs) = shown(text);
            let at = runs
                .iter()
                .map(|&(_, t, n)| from.saturating_sub(t).min(n))
                .sum();
            // a match's start is in the run it starts, its end in the run it ends: tags at
            // either edge stay outside
            let back = |p: usize, end: bool| {
                runs.iter()
                    .find(|&&(h, _, n)| p < h + n || (end && p == h + n))
                    .map_or_else(
                        || runs.last().map_or(text.len(), |&(_, t, n)| t + n),
                        |&(h, t, _)| t + p - h,
                    )
            };
            scan(&self.regex, &hay, at, groups, |m, c| {
                let s = back(m.start, false);
                f(s..back(m.end, true).max(s), &hay[m], c)
            });
        }
    }

    /// The first match in `text`, a value of the field, from byte `from` on (Find next).
    pub fn find_at(&self, text: &str, from: usize) -> Option<Range<usize>> {
        let mut hit = None;
        self.each_match(text, from, false, |m, _, _| {
            hit = Some(m);
            false
        });
        hit
    }

    pub fn is_match(&self, ev: &Event) -> bool {
        if self.field == Field::Text && self.only_tags {
            return tags::block_spans(&ev.text)
                .into_iter()
                .any(|b| self.regex.is_match(&ev.text[b]));
        }
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
        // sorted once, so a long row list is a binary search per line
        let rows = self.rows.as_ref().map(|r| {
            let mut r = r.clone();
            r.sort_unstable();
            r
        });
        (0..doc.event_count())
            .filter(|&i| {
                let ev = doc.event(i).unwrap();
                let ok = (self.styles.is_empty() || self.styles.contains(&ev.style))
                    && (self.actors.is_empty() || self.actors.contains(&ev.actor))
                    && (self.effects.is_empty() || self.effects.contains(&ev.effect))
                    && self.text.as_ref().is_none_or(|m| m.is_match(ev))
                    && self.comments.is_none_or(|c| ev.is_comment() == c)
                    && rows.as_ref().is_none_or(|r| r.binary_search(&i).is_ok())
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

/// `pattern` as a regex matching only at the start and/or the end of the field (Kainote's
/// "Start of text" and "End of text").
pub fn anchored(pattern: &str, is_regex: bool, start: bool, end: bool) -> String {
    let p = if is_regex {
        format!("(?:{pattern})")
    } else {
        regex::escape(pattern)
    };
    format!(
        "{}{p}{}",
        if start { "^" } else { "" },
        if end { "$" } else { "" }
    )
}

/// `f` for each match of `re` in `hay` from byte `at` on, as `captures_iter` has them (no empty
/// match where the one before ended), with its groups when `groups`; false when `f` stopped it.
fn scan(
    re: &Regex,
    hay: &str,
    mut at: usize,
    groups: bool,
    mut f: impl FnMut(Range<usize>, Option<&Captures>) -> bool,
) -> bool {
    let mut last = None;
    while at <= hay.len() {
        let Some(m) = re.find_at(hay, at) else {
            break;
        };
        at = m.end();
        if m.is_empty() {
            at += hay[at..].chars().next().map_or(1, char::len_utf8);
            if last == Some(m.start()) {
                continue;
            }
        }
        last = Some(m.end());
        // the groups cost a second search, only where a replacement uses them
        let c = groups.then(|| re.captures_at(hay, m.start())).flatten();
        if !f(m.range(), c.as_ref()) {
            return false;
        }
    }
    true
}

/// `text` without its `{...}` blocks, as [`tags::strip_tags`] has it, and its runs: where each
/// starts in the result and in `text`, and its length.
fn shown(text: &str) -> (String, Vec<(usize, usize, usize)>) {
    let (mut hay, mut runs, mut i) = (String::with_capacity(text.len()), Vec::new(), 0);
    while i < text.len() {
        let open = text[i..].find('{').map_or(text.len(), |k| i + k);
        if open > i {
            runs.push((hay.len(), i, open - i));
            hay.push_str(&text[i..open]);
        }
        // an unclosed block hides the rest
        i = text[open..].find('}').map_or(text.len(), |k| open + k + 1);
    }
    (hay, runs)
}

/// `rep` read as Aegisub's replacement (boost's Perl format): `$1a` is group 1, then "a" (the
/// regex crate would look for a group named "1a"); `\1` to `\9` are groups too, unless they open
/// a colour or alpha tag (`\1c`, `\3a`, which Aegisub would read as a group and a letter); `\\`
/// is one backslash and `\$` a dollar. Any other backslash stays, so `\N` is still a line break
/// (Aegisub drops it and leaves "N").
fn perl_groups(rep: &str) -> String {
    let (mut out, mut rest) = (String::with_capacity(rep.len()), rep);
    while let Some(i) = rest.find(['$', '\\']) {
        out.push_str(&rest[..i]);
        let after = &rest[i + 1..];
        if rest.as_bytes()[i] == b'\\' {
            let group = after.starts_with(|c: char| ('1'..='9').contains(&c))
                && !after[1..].starts_with(['c', 'a']);
            if group {
                out.push_str(&format!("${{{}}}", &after[..1]));
                rest = &after[1..];
            } else if let Some(r) = after.strip_prefix('\\') {
                out.push('\\');
                rest = r;
            } else if let Some(r) = after.strip_prefix('$') {
                out.push_str("$$");
                rest = r;
            } else {
                out.push('\\');
                rest = after;
            }
            continue;
        }
        let digits = after.bytes().take_while(u8::is_ascii_digit).count();
        if let Some(r) = after.strip_prefix('$') {
            out.push_str("$$");
            rest = r;
        } else if digits > 0
            && after[digits..].starts_with(|c: char| c.is_ascii_alphanumeric() || c == '_')
        {
            out.push_str(&format!("${{{}}}", &after[..digits]));
            rest = &after[digits..];
        } else {
            out.push('$');
            rest = after;
        }
    }
    out + rest
}

#[derive(Debug, Clone)]
pub struct Replace {
    pub matcher: TextMatcher,
    /// Replacement; `$1`/`${name}` references groups when the matcher is a user's regex.
    pub replacement: String,
    /// Replace only the first match per line.
    pub first_only: bool,
    /// Write each replacement in the case of its match ([`follow_case`]).
    pub keep_case: bool,
}

/// `rep` in the case of `matched`, so a case-insensitive "sie" -> "się" turns "Sie" into
/// "Się" and "SIE" into "SIĘ" (Kainote's error corrector): all capitals stay capitals, a
/// leading capital stays a leading capital, anything else is left as written.
pub fn follow_case(matched: &str, rep: String) -> String {
    let letters: Vec<char> = matched.chars().filter(|c| c.is_alphabetic()).collect();
    if !letters.first().is_some_and(|c| c.is_uppercase()) {
        return rep;
    }
    if letters.len() > 1 && letters.iter().all(|c| c.is_uppercase()) {
        return rep.to_uppercase();
    }
    let Some((i, c)) = rep.char_indices().find(|(_, c)| c.is_alphabetic()) else {
        return rep;
    };
    format!(
        "{}{}{}",
        &rep[..i],
        c.to_uppercase(),
        &rep[i + c.len_utf8()..]
    )
}

impl Replace {
    /// The replacement as written for the regex crate.
    fn rep(&self) -> String {
        if self.matcher.is_regex {
            perl_groups(&self.replacement)
        } else {
            self.replacement.clone()
        }
    }

    /// What replaces the match `matched`, with groups `c` when the matcher is a user's regex.
    fn expand(&self, rep: &str, matched: &str, c: Option<&Captures>) -> String {
        let mut out = String::new();
        match c {
            Some(c) => c.expand(rep, &mut out),
            None => out.push_str(rep),
        }
        if self.keep_case {
            follow_case(matched, out)
        } else {
            out
        }
    }

    /// `text` with the matches from byte `from` on replaced (the first one alone when `first`),
    /// and how many; "" when none. Tags a skip-tags match spans stay, after its replacement.
    fn apply(&self, rep: &str, text: &str, from: usize, first: bool) -> (String, usize) {
        let m = &self.matcher;
        let keep_tags = m.field == Field::Text && m.strip_tags && !m.only_tags;
        let (mut out, mut last, mut n) = (String::new(), 0, 0);
        m.each_match(text, from, m.is_regex, |at, matched, c| {
            out.push_str(&text[last..at.start]);
            out.push_str(&self.expand(rep, matched, c));
            if keep_tags {
                for b in tags::block_spans(&text[at.clone()]) {
                    out.push('{');
                    out.push_str(&text[at.start + b.start..at.start + b.end]);
                    out.push('}');
                }
            }
            (last, n) = (at.end, n + 1);
            !first
        });
        if n > 0 {
            out.push_str(&text[last..]);
        }
        (out, n)
    }

    /// Replace next: the match at `at` of `text`, a value of the field, if one is still exactly
    /// there; the new text and where its replacement ends.
    pub fn replace_one(&self, text: &str, at: Range<usize>) -> Option<(String, usize)> {
        if self.matcher.find_at(text, at.start)? != at {
            return None;
        }
        let (new, _) = self.apply(&self.rep(), text, at.start, true);
        let end = new.len() - (text.len() - at.end);
        Some((new, end))
    }
}

/// Replace in the given rows; returns number of replacements made.
pub fn replace(doc: &mut Document, rows: &Rows, r: &Replace) -> usize {
    let rep = r.rep();
    let mut count = 0;
    for i in rows.resolve(doc) {
        let (new, n) = r.apply(
            &rep,
            r.matcher.field_value(doc.event(i).unwrap()),
            0,
            r.first_only,
        );
        if n > 0 {
            count += n;
            r.matcher.set_field(doc.event_mut(i).unwrap(), new);
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

/// Duplicate rows: the copies of each block of adjacent rows go after the block, in order
/// (Aegisub, Kainote). Returns the new row indices.
/// Kainote's new end times of a conversion: each dialogue lasts `ms_per_char` per character of
/// its text without tags, at least a second, and the one before it ends by its start.
pub fn new_end_times(doc: &mut Document, ms_per_char: i64) {
    let mut prev: Option<usize> = None;
    for i in 0..doc.event_count() {
        let Some(e) = doc.event(i).filter(|e| e.kind == EventKind::Dialogue) else {
            continue;
        };
        let (start, chars) = (e.start, tags::strip_tags(&e.text).chars().count() as i64);
        if let Some(p) = prev.and_then(|p| doc.event_mut(p))
            && p.end > start
        {
            p.end = start;
        }
        if let Some(e) = doc.event_mut(i) {
            e.end = Time(start.0 + (ms_per_char * chars).max(1000));
        }
        prev = Some(i);
    }
}

pub fn duplicate(doc: &mut Document, rows: &[usize]) -> Vec<usize> {
    let mut rows = rows.to_vec();
    rows.sort_unstable();
    rows.dedup();
    let items = rows
        .chunk_by(|a, b| b - a == 1)
        .flat_map(|block| {
            let after = block[block.len() - 1] + 1;
            block.iter().map(move |&r| (after, r))
        })
        .filter_map(|(at, r)| Some((at, doc.event(r)?.clone())))
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
    let mut gone: Vec<usize> = sel
        .iter()
        .zip(&dead)
        .filter(|(_, d)| **d)
        .map(|(r, _)| *r)
        .collect();
    gone.sort_unstable();
    delete_rows(doc, &gone);
    let mut left: Vec<usize> = sel
        .iter()
        .zip(&dead)
        .filter(|(_, d)| !**d)
        .map(|(r, _)| *r)
        .collect();
    left.sort_unstable();
    left.iter()
        .map(|&r| r - gone.partition_point(|&g| g < r))
        .collect()
}

/// Aegisub "Split Lines (by karaoke)": each `\k` syllable of the rows
/// becomes its own line with the syllable's times. Rows with fewer than
/// two syllables stay. Returns the new rows.
pub fn split_by_karaoke(doc: &mut Document, rows: &[usize]) -> Vec<usize> {
    let mut rows = rows.to_vec();
    rows.sort_unstable();
    rows.dedup();
    let mut gone = Vec::new();
    let mut items = Vec::new();
    for r in rows {
        let Some(ev) = doc.event(r) else { continue };
        let syls = crate::tags::karaoke_syllables(&ev.text);
        if syls.len() < 2 {
            continue;
        }
        // the parts go where the line was, once the split lines are removed
        let at = r - gone.len();
        gone.push(r);
        for s in syls {
            let mut e = ev.clone();
            e.start = ev.start + Time(s.start_ms);
            e.end = e.start + Time(s.duration_ms);
            e.text = s.text;
            items.push((at, e));
        }
    }
    doc.remove_events(&gone);
    doc.insert_events(items)
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
                    e.text // tags and all, as Aegisub joins them
                ));
                pos = pos.max(e.end);
            }
            s
        }
    };
    doc.remove_events(&rows[1..]);
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

/// Link each `(previous, next)` pair of rows: the previous one ends at the next one's start, or
/// with `use_next_start` false the next one starts at the previous one's end.
pub fn make_continuous(doc: &mut Document, pairs: &[(usize, usize)], use_next_start: bool) {
    for &(a, b) in pairs {
        let (Some(end), Some(start)) = (doc.event(a).map(|e| e.end), doc.event(b).map(|e| e.start))
        else {
            continue;
        };
        if use_next_start {
            doc.event_mut(a).unwrap().end = start;
        } else {
            doc.event_mut(b).unwrap().start = end;
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
    /// Link lines whose gap is below `adjacent_threshold`, or which overlap by no more than
    /// `adjacent_overlap` (Aegisub's "Max overlap"); `adjacent_bias` (0..1) chooses where in
    /// the gap they meet (0 = previous end, 1 = next start).
    pub adjacent_threshold: Option<Time>,
    pub adjacent_overlap: Option<Time>,
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
    idx.sort_by_key(|&i| doc.event(i).unwrap().start);
    // Aegisub's lead-in, then lead-out: a line grows up to the lines it doesn't overlap yet, and
    // lead-out stops at their new starts. Aegisub compares every pair; a line before this one
    // is clear of it when it ends by this start, so the latest such end is the limit.
    if let Some(li) = pp.lead_in {
        let mut ends = std::collections::BinaryHeap::new();
        let mut limit = Time::ZERO;
        for &i in &idx {
            let ev = doc.event_mut(i).unwrap();
            while let Some(&std::cmp::Reverse(e)) = ends.peek()
                && e <= ev.start
            {
                limit = limit.max(e);
                ends.pop();
            }
            ends.push(std::cmp::Reverse(ev.end));
            ev.start = (ev.start - li).max(limit).max(Time::ZERO);
        }
    }
    // the new starts are still in order (each is the larger of two growing values), so the
    // first line after this one that starts at or past its end is the limit
    if let Some(lo) = pp.lead_out {
        let starts: Vec<Time> = idx.iter().map(|&i| doc.event(i).unwrap().start).collect();
        for (k, &i) in idx.iter().enumerate() {
            let ev = doc.event_mut(i).unwrap();
            let later = &starts[k + 1..];
            let next = later.get(later.partition_point(|&s| s < ev.end));
            ev.end = next.map_or(ev.end + lo, |&n| (ev.end + lo).min(n));
        }
    }
    // adjacent linking
    if pp.adjacent_threshold.is_some() || pp.adjacent_overlap.is_some() {
        let gap_max = pp.adjacent_threshold.unwrap_or(Time(-1));
        let overlap_max = pp.adjacent_overlap.unwrap_or(Time(-1));
        for w in idx.windows(2) {
            let (a, b) = (w[0], w[1]);
            let (ae, bs) = (doc.event(a).unwrap().end, doc.event(b).unwrap().start);
            let gap = bs - ae;
            if (gap.ms() > 0 && gap <= gap_max) || (gap.ms() < 0 && -gap <= overlap_max) {
                let meet =
                    ae + Time((gap.ms() as f64 * pp.adjacent_bias.clamp(0.0, 1.0)).round() as i64);
                doc.event_mut(a).unwrap().end = meet;
                doc.event_mut(b).unwrap().start = meet;
            }
        }
    }
    // keyframe snapping
    if let Some((kfs, fr)) = pp.keyframes {
        let mut kf_times: Vec<i64> = kfs
            .iter()
            .map(|&f| fr.time_at_frame(f, TimeType::Start))
            .collect();
        kf_times.sort_unstable();
        let snap = |t: Time, before: Time, after: Time| snap_to(&kf_times, t, before, after);
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
}

/// The keyframe nearest `t` within `before`/`after` of it (the earlier one on a tie), else `t`.
/// `kfs` sorted: only the neighbours on each side can be nearest.
fn snap_to(kfs: &[i64], t: Time, before: Time, after: Time) -> Time {
    let p = kfs.partition_point(|&k| k < t.ms());
    let below = p
        .checked_sub(1)
        .map(|i| kfs[i])
        .filter(|&k| t.ms() - k <= before.ms());
    let above = kfs.get(p).copied().filter(|&k| k - t.ms() <= after.ms());
    Time(match (below, above) {
        (Some(b), Some(a)) if a - t.ms() < t.ms() - b => a,
        (Some(b), _) => b,
        (None, a) => a.unwrap_or(t.ms()),
    })
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

/// What [`resample`] does when the aspect ratio changes (Aegisub's `ResampleARMode`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ArMode {
    /// Scale X and Y apart; glyphs widen through `\fscx`.
    #[default]
    Stretch,
    /// Keep proportions, adding borders to the source.
    AddBorder,
    /// Keep proportions, cutting the source.
    RemoveBorder,
    /// Add `margin` to the source and stretch what remains.
    Manual,
}

#[derive(Debug, Clone, Default)]
pub struct Resample {
    pub from: (i32, i32),
    pub to: (i32, i32),
    /// Added to the source on the left, right, top and bottom (negative cuts);
    /// read in `Manual` mode, the border modes compute their own.
    pub margin: [i32; 4],
    pub ar_mode: ArMode,
    /// Convert colours between these "YCbCr Matrix" values (`TV.601` to
    /// `TV.709`, ...) and write the second to the header.
    pub matrix: Option<(String, String)>,
}

/// Change the script resolution like Aegisub's "Resample Resolution": styles,
/// margins, positions, clips, drawings, size tags and, if asked, colours.
pub fn resample(doc: &mut Document, r: &Resample) {
    let (mut sx, mut sy) = (r.from.0.max(1), r.from.1.max(1));
    let (dx, dy) = (r.to.0.max(1), r.to.1.max(1));
    let new_ar = dx as f64 / dy as f64;
    // close enough is left alone: 848x480 <-> 1280x720 is .006
    let differs = |old_ar: f64| (old_ar - new_ar).abs() / new_ar > 0.01;
    let mut margin = [0; 4];
    let mut ar = 1.0; // the horizontal stretch, applied through \fscx
    let old_ar = sx as f64 / sy as f64;
    if differs(old_ar) {
        match r.ar_mode {
            ArMode::Stretch => ar = new_ar / old_ar,
            ArMode::AddBorder | ArMode::RemoveBorder => {
                if (new_ar > old_ar) == (r.ar_mode == ArMode::AddBorder) {
                    let m = ((sy as f64 * new_ar - sx as f64) / 2.0) as i32;
                    margin[..2].fill(m);
                } else {
                    let m = ((sx as f64 / new_ar - sy as f64) / 2.0) as i32;
                    margin[2..].fill(m);
                }
            }
            ArMode::Manual => {
                margin = r.margin;
                let with_margins = (sx + margin[0] + margin[1]) as f64
                    / (sy + margin[2] + margin[3]).max(1) as f64;
                if differs(with_margins) {
                    ar = new_ar / with_margins;
                }
            }
        }
    }
    sx = (sx + margin[0] + margin[1]).max(1);
    sy = (sy + margin[2] + margin[3]).max(1);
    let (rx, ry) = (dx as f64 / sx as f64, dy as f64 / sy as f64);
    let (ml, mt) = (margin[0] as f64, margin[2] as f64);
    let px = |x: f64| (x + ml) * rx;
    let py = |y: f64| (y + mt) * ry;
    // margins 0..4 are left, right, top (= vertical) and bottom
    let scale_margin =
        |m: i32, k: usize| ((m + margin[k]) as f64 * if k < 2 { rx } else { ry }).round() as i32;
    let conv = r
        .matrix
        .as_ref()
        .and_then(|(a, b)| MatrixConversion::new(a, b));

    for i in 0..doc.style_count() {
        let st = doc.style_mut(i).unwrap();
        st.size = (st.size * ry).round(); // whole, as Aegisub writes it
        st.outline *= ry;
        st.shadow *= ry;
        st.spacing *= ry; // \fscx, so the stretch, applies to it when rendering
        st.scale_x *= ar;
        st.margin_l = scale_margin(st.margin_l, 0);
        st.margin_r = scale_margin(st.margin_r, 1);
        st.margin_v = scale_margin(st.margin_v, 2);
        st.margin_b = scale_margin(st.margin_b, 3);
        if let Some(c) = conv {
            for col in [
                &mut st.primary,
                &mut st.secondary,
                &mut st.outline_color,
                &mut st.back,
            ] {
                *col = c.convert(*col);
            }
        }
    }

    for i in 0..doc.event_count() {
        let ev = doc.event(i).unwrap();
        // karaoke templates are code, not lines
        if ev.is_comment() && (ev.effect.starts_with("template") || ev.effect.starts_with("code")) {
            continue;
        }
        let mut line = Line::parse(&ev.text);
        let scale_tag = |t: &mut Tag| {
            let num = |t: &Tag, k: usize| t.arg_f64(k).unwrap_or(0.0);
            let s = |v: f64| crate::model::fmt_f64((v * 1000.0).round() / 1000.0);
            let int = |v: f64| (v.round() as i64).to_string();
            let factor = match t.name.as_str() {
                // \fs+n and \fs-n change the size by n tenths of itself: nothing to scale
                "fs" if t.args.first().is_some_and(|a| a.starts_with(['+', '-'])) => None,
                // \blur too, unlike Aegisub: libass measures it in script pixels
                "fs" | "bord" | "shad" | "ybord" | "yshad" | "fsp" | "pbo" | "blur" => Some(ry),
                "xbord" | "xshad" => Some(rx),
                "fscx" if ar != 1.0 => Some(ar),
                _ => None,
            };
            if let Some(f) = factor {
                if let Some(v) = t.arg_f64(0) {
                    t.set_args(vec![s(v * f)]);
                }
                return;
            }
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
                        int(px(num(t, 0))),
                        int(py(num(t, 1))),
                        int(px(num(t, 2))),
                        int(py(num(t, 3))),
                    ];
                    t.set_args(a);
                }
                "clip" | "iclip" => {
                    let (scale, d) = if t.args.len() == 2 {
                        (t.arg_f64(0).unwrap_or(1.0), t.args[1].clone())
                    } else {
                        (1.0, t.args.first().cloned().unwrap_or_default())
                    };
                    let f = tags::drawing_divisor(scale);
                    let mut cmds = tags::parse_drawing(&d);
                    tags::transform_drawing(&mut cmds, |x, y| (px(x / f) * f, py(y / f) * f));
                    let mut a = t.args.clone();
                    *a.last_mut().unwrap() = tags::write_drawing(&cmds);
                    t.set_args(a);
                }
                "c" | "1c" | "2c" | "3c" | "4c" => {
                    if let (Some(c), Some(v)) =
                        (conv, t.args.first().and_then(|a| Color::parse_tag(a)))
                    {
                        t.set_args(vec![c.convert(v).to_tag_string()]);
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
                    // placed by \pos; X divides out the stretch \fscx already gives it
                    let mut cmds = tags::parse_drawing(commands);
                    if !cmds.is_empty() {
                        tags::transform_drawing(&mut cmds, |x, y| (x * rx / ar, y * ry));
                        *commands = tags::write_drawing(&cmds);
                        changed = true;
                    }
                }
                _ => {}
            }
        }
        let new_text = changed.then(|| line.to_text());
        let ev = doc.event(i).unwrap();
        let old = [ev.margin_l, ev.margin_r, ev.margin_v, ev.margin_b];
        let mut margins = old;
        for (k, m) in margins.iter_mut().enumerate() {
            if *m != 0 {
                *m = scale_margin(*m, k);
            }
        }
        // a line that stays the same is not copied: the undo history keeps sharing it
        if new_text.is_some() || margins != old {
            let ev = doc.event_mut(i).unwrap();
            if let Some(t) = new_text {
                ev.text = t;
            }
            [ev.margin_l, ev.margin_r, ev.margin_v, ev.margin_b] = margins;
        }
    }
    doc.set_info("PlayResX", &dx.to_string());
    doc.set_info("PlayResY", &dy.to_string());
    if let Some((_, to)) = &r.matrix {
        doc.set_info("YCbCr Matrix", to);
    }
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
                    let f = tags::drawing_divisor(if t.args.len() == 2 {
                        t.arg_f64(0).unwrap_or(1.0)
                    } else {
                        1.0
                    });
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
                    let f = tags::drawing_divisor(f64::from(*scale));
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

/// Kainote's "preserve proportions" of the scale and Z rotation tools: one line's place carried
/// through `f`, the same scale or turn around one point for every selected line, so the lines keep
/// their places against each other while their `\fscx`/`\frz` change. `\pos` and both points of
/// `\move` go through `f`; for a turn, a line with `\org` moves only that, as libass already turns
/// the line around it. Clips, drawn in screen space, go along; a rectangle a turn tilts becomes the
/// vector of its corners. A line with neither `\pos` nor `\move` gets a `\pos` from `place`.
pub fn transform_place(
    text: &str,
    f: &dyn Fn(f64, f64) -> (f64, f64),
    turn: bool,
    place: (f64, f64),
) -> String {
    let mut line = Line::parse(text);
    let s = |v: f64| crate::model::fmt_f64((v * 1000.0).round() / 1000.0);
    let has = |name: &str| line.tags().any(|t| t.name == name);
    let org_only = turn && has("org");
    let placed = has("pos") || has("move");
    line.for_each_tag_mut(|t| {
        let n = |k: usize| t.arg_f64(k).unwrap_or(0.0);
        match t.name.as_str() {
            "pos" | "org" if t.args.len() >= 2 && (t.name == "org" || !org_only) => {
                let (x, y) = f(n(0), n(1));
                t.set_args(vec![s(x), s(y)]);
            }
            "move" if !org_only && t.args.len() >= 4 => {
                let ((x1, y1), (x2, y2)) = (f(n(0), n(1)), f(n(2), n(3)));
                let mut a = vec![s(x1), s(y1), s(x2), s(y2)];
                a.extend(t.args[4..].iter().cloned());
                t.set_args(a);
            }
            "clip" | "iclip" if t.args.len() == 4 => {
                let (x1, y1, x2, y2) = (n(0), n(1), n(2), n(3));
                if turn {
                    let c = [f(x1, y1), f(x2, y1), f(x2, y2), f(x1, y2)];
                    let d = format!(
                        "m {} {} l {} {} {} {} {} {}",
                        s(c[0].0),
                        s(c[0].1),
                        s(c[1].0),
                        s(c[1].1),
                        s(c[2].0),
                        s(c[2].1),
                        s(c[3].0),
                        s(c[3].1)
                    );
                    t.set_args(vec![d]);
                } else {
                    let ((a, b), (c, d)) = (f(x1, y1), f(x2, y2));
                    t.set_args(vec![s(a.min(c)), s(b.min(d)), s(a.max(c)), s(b.max(d))]);
                }
            }
            "clip" | "iclip" => {
                let k = tags::drawing_divisor(if t.args.len() == 2 {
                    t.arg_f64(0).unwrap_or(1.0)
                } else {
                    1.0
                });
                let mut cmds = tags::parse_drawing(t.args.last().map(String::as_str).unwrap_or(""));
                tags::transform_drawing(&mut cmds, |x, y| {
                    let (x, y) = f(x / k, y / k);
                    (x * k, y * k)
                });
                let mut a = t.args.clone();
                if let Some(l) = a.last_mut() {
                    *l = tags::write_drawing(&cmds);
                }
                t.set_args(a);
            }
            _ => {}
        }
    });
    if !placed && !org_only {
        let (x, y) = f(place.0, place.1);
        line.set_line_tag(Tag::new("pos", vec![s(x), s(y)]));
    }
    line.to_text()
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

    #[test]
    fn new_end_times_by_characters() {
        let mut d = Document::new();
        d.remove_event(0);
        d.push_event(Event::dialogue(
            Time(0),
            Time(9000),
            "Default",
            r"{\i1}abcdefghij",
        ));
        d.push_event(Event::dialogue(Time(1500), Time(2000), "Default", "ab"));
        new_end_times(&mut d, 200);
        // ten characters (the tag not counted) would end at 2 s: cut to the next start
        assert_eq!(d.event(0).unwrap().end, Time(1500));
        assert_eq!(d.event(1).unwrap().end, Time(2500)); // at least a second
    }

    #[test]
    fn transform_place_scales_and_turns_around_one_point() {
        let twice = |x: f64, y: f64| (100.0 + (x - 100.0) * 2.0, 100.0 + (y - 100.0) * 2.0);
        assert_eq!(
            transform_place(
                r"{\pos(110,90)\clip(90,90,110,110)}a",
                &twice,
                false,
                (0.0, 0.0)
            ),
            r"{\pos(120,80)\clip(80,80,120,120)}a"
        );
        assert_eq!(
            transform_place(
                r"{\move(100,100,110,100,0,500)}a",
                &twice,
                false,
                (0.0, 0.0)
            ),
            r"{\move(100,100,120,100,0,500)}a"
        );
        // no \pos: one from where the line is
        assert_eq!(
            transform_place("a", &twice, false, (105.0, 100.0)),
            r"{\pos(110,100)}a"
        );
        // a quarter turn: a line with \org keeps its \pos, the \org goes round
        let quarter = |x: f64, y: f64| (100.0 + (y - 100.0), 100.0 - (x - 100.0));
        assert_eq!(
            transform_place(r"{\pos(5,5)\org(110,100)}a", &quarter, true, (0.0, 0.0)),
            r"{\pos(5,5)\org(100,90)}a"
        );
        assert_eq!(
            transform_place(
                r"{\pos(110,100)\clip(100,100,110,110)}a",
                &quarter,
                true,
                (0.0, 0.0)
            ),
            r"{\pos(100,90)\clip(m 100 100 l 100 90 110 90 110 100)}a"
        );
    }

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
                keep_case: false,
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
                keep_case: false,
            },
        );
        assert_eq!(d.event(0).unwrap().text, r"{\i1}$o$ is\N{\i0}$o$");
        // without a regex "$1" is just text, also for a matcher anchored by the find options
        let mut m = TextMatcher::new(
            &anchored("$o$", false, false, true),
            true,
            true,
            Field::Text,
        )
        .unwrap();
        m.is_regex = false;
        let r = Replace {
            matcher: m,
            replacement: "$1".into(),
            first_only: false,
            keep_case: false,
        };
        assert_eq!(replace(&mut d, &Rows::All, &r), 1);
        assert_eq!(d.event(0).unwrap().text, r"{\i1}$o$ is\N{\i0}$1");
        assert_eq!(anchored("a.", true, false, true), "(?:a.)$");
        // Skip text: only inside the blocks, where the text's "o" is not
        let mut m = TextMatcher::new("i", false, true, Field::Text).unwrap();
        m.only_tags = true;
        assert!(m.is_match(d.event(0).unwrap()));
        let r = Replace {
            matcher: m,
            replacement: "b".into(),
            first_only: false,
            keep_case: false,
        };
        assert_eq!(replace(&mut d, &Rows::All, &r), 2);
        assert_eq!(d.event(0).unwrap().text, r"{\b1}$o$ is\N{\b0}$1");
        assert_eq!(tags::block_spans("a{b}c{d"), vec![2..3]);
    }

    #[test]
    fn skip_tags_matches_what_shows() {
        let rep = |text: &str, find: &str, regex: bool, with: &str| {
            let mut m = TextMatcher::new(find, regex, true, Field::Text).unwrap();
            m.strip_tags = true;
            let r = Replace {
                matcher: m,
                replacement: with.into(),
                first_only: false,
                keep_case: false,
            };
            let (out, n) = r.apply(&r.rep(), text, 0, false);
            if n == 0 { text.to_string() } else { out }
        };
        // a match may span a tag, which stays; ^ and $ are the ends of what shows
        assert_eq!(rep(r"a{\i1}b c", "ab", false, "x"), r"x{\i1} c");
        assert_eq!(
            rep(r"{\an8}abc{\i0}", "^a|c$", true, "_"),
            r"{\an8}_b_{\i0}"
        );
        assert_eq!(rep(r"{\an8}abc", "^", true, ">"), r"{\an8}>abc");
        assert_eq!(rep(r"(a){b}", r"\((\w)\)", true, "[$1x]"), r"[ax]{b}");
        assert_eq!(rep("ab{c", "b", false, "B"), "aB{c");
        // where Find next starts and what it shows, as byte ranges of the text
        let mut m = TextMatcher::new("o", false, true, Field::Text).unwrap();
        m.strip_tags = true;
        assert_eq!(m.find_at(r"{\pos(1,2)}bob {o}o", 0), Some(12..13));
        assert_eq!(m.find_at(r"{\pos(1,2)}bob {o}o", 13), Some(18..19));
        assert_eq!(m.find_at(r"{\pos(1,2)}bob {o}o", 19), None);
        m.strip_tags = false;
        assert_eq!(m.find_at(r"{\pos(1,2)}bob {o}o", 13), Some(16..17));
        // one match alone, and only where it was shown
        let r = Replace {
            matcher: TextMatcher::new(r"(\w)o", true, true, Field::Text).unwrap(),
            replacement: "$1$1".into(),
            first_only: false,
            keep_case: false,
        };
        assert_eq!(r.replace_one("bo go", 3..5), Some(("bo gg".into(), 5)));
        assert_eq!(r.replace_one("bo go", 2..4), None);
        // every match as the regex crate counts them, empty ones too
        for (find, text) in [("a*", "baaac"), ("x?", "ąx"), (r"\b", "ab cd"), ("$", "a")] {
            let m = TextMatcher::new(find, true, true, Field::Text).unwrap();
            let r = Replace {
                matcher: m.clone(),
                replacement: "-".into(),
                first_only: false,
                keep_case: false,
            };
            let re = Regex::new(find).unwrap();
            assert_eq!(r.apply("-", text, 0, false).0, re.replace_all(text, "-"));
            assert_eq!(r.apply("-", text, 0, false).1, re.find_iter(text).count());
        }
        assert_eq!(
            perl_groups("$1a $2 ${3}b $$1x $name $"),
            "${1}a $2 ${3}b $$1x $name $"
        );
        assert_eq!(
            perl_groups(r"{\1c&H0&\3a&HFF&}\1x \2 \\N \N \\ \0 \$5 \"),
            r"{\1c&H0&\3a&HFF&}${1}x ${2} \N \N \ \0 $$5 \"
        );
        let r = Replace {
            matcher: TextMatcher::new(r"(\w+) (\w+)", true, true, Field::Text).unwrap(),
            replacement: r"{\1c&HFF&}\2\N\1".into(),
            first_only: false,
            keep_case: false,
        };
        assert_eq!(r.apply(&r.rep(), "ab cd", 0, false).0, r"{\1c&HFF&}cd\Nab");
    }

    #[test]
    fn replace_following_case() {
        let mut d = doc(&[(
            "0:00:00.00",
            "0:00:01.00",
            "Default",
            r"Sie sie SIE {\i1}sie",
        )]);
        let mut m = TextMatcher::new(r"\bsie\b", true, false, Field::Text).unwrap();
        m.strip_tags = true;
        let r = Replace {
            matcher: m,
            replacement: "się".into(),
            first_only: false,
            keep_case: true,
        };
        assert_eq!(replace(&mut d, &Rows::All, &r), 4);
        assert_eq!(d.event(0).unwrap().text, r"Się się SIĘ {\i1}się");
        assert_eq!(follow_case("a..B", "a...B".into()), "a...B");
        assert_eq!(follow_case("W ogole", "w ogóle".into()), "W ogóle");
        assert_eq!(follow_case("-San", String::new()), "");
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
            ("0:00:00.70", "0:00:01.00", "Default", r"{\i1}ra"),
        ]);
        join(&mut d, &[0, 1], JoinMode::AsKaraoke);
        assert_eq!(d.event(0).unwrap().text, r"{\k50}ka{\k20}{\k30}{\i1}ra");
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
    fn duplicates_follow_their_block() {
        let lines: Vec<_> = ["a", "b", "c", "d", "e"]
            .map(|t| ("0:00:00.00", "0:00:01.00", "Default", t))
            .to_vec();
        let mut d = doc(&lines);
        let new = duplicate(&mut d, &[3, 0, 1]);
        let texts: Vec<_> = (0..d.event_count())
            .map(|i| d.event(i).unwrap().text.as_str())
            .collect();
        assert_eq!(texts, ["a", "b", "a", "b", "c", "d", "d", "e"]);
        assert_eq!(new, [2, 3, 6]);
    }

    /// Aegisub's lead-in/out, checked against its pairwise version (every line clear of this one
    /// limits it), with a gap smaller than both leads and lines that overlap already.
    #[test]
    fn leads_match_aegisub() {
        let times = |d: &Document| -> Vec<(i64, i64)> {
            (0..d.event_count())
                .map(|i| d.event(i).unwrap())
                .map(|e| (e.start.ms(), e.end.ms()))
                .collect()
        };
        let aegisub = |mut v: Vec<(i64, i64)>, li: i64, lo: i64| {
            let n = v.len();
            let collide = |a: (i64, i64), b: (i64, i64)| a.0 < b.1 && a.1 > b.0;
            for i in 0..n {
                let mut s = v[i].0 - li;
                for j in 0..i {
                    if !collide(v[i], v[j]) {
                        s = s.max(v[j].1);
                    }
                }
                v[i].0 = s.max(0);
            }
            for i in 0..n {
                let mut e = v[i].1 + lo;
                for j in i + 1..n {
                    if !collide(v[i], v[j]) {
                        e = e.min(v[j].0);
                    }
                }
                v[i].1 = e;
            }
            v
        };
        let pp = PostProcess {
            lead_in: Some(Time(100)),
            lead_out: Some(Time(350)),
            ..Default::default()
        };
        let cases: [&[(&str, &str)]; 3] = [
            &[("0:00:01.00", "0:00:02.00"), ("0:00:02.20", "0:00:03.00")],
            &[("0:00:01.00", "0:00:03.00"), ("0:00:02.00", "0:00:04.00")],
            &[
                ("0:00:00.05", "0:00:01.00"),
                ("0:00:00.50", "0:00:05.00"),
                ("0:00:01.10", "0:00:02.00"),
                ("0:00:02.05", "0:00:02.50"),
                ("0:00:05.20", "0:00:06.00"),
            ],
        ];
        for case in cases {
            let lines: Vec<_> = case.iter().map(|&(s, e)| (s, e, "Default", "x")).collect();
            let mut d = doc(&lines);
            let want = aegisub(times(&d), 100, 350);
            post_process(&mut d, &Rows::All, &pp);
            assert_eq!(times(&d), want);
        }
        // the audit's example: the lead-in of the second line wins the gap first
        let mut d = doc(&[
            ("0:00:01.00", "0:00:02.00", "Default", "a"),
            ("0:00:02.20", "0:00:03.00", "Default", "b"),
        ]);
        post_process(&mut d, &Rows::All, &pp);
        assert_eq!(times(&d), [(900, 2100), (2100, 3350)]);
    }

    #[test]
    fn overlaps_link_only_within_their_own_threshold() {
        let mut d = doc(&[
            ("0:00:01.00", "0:00:03.00", "Default", "a"),
            ("0:00:02.60", "0:00:04.00", "Default", "b"),
        ]);
        let mut pp = PostProcess {
            adjacent_threshold: Some(Time(500)),
            adjacent_bias: 1.0,
            ..Default::default()
        };
        post_process(&mut d, &Rows::All, &pp);
        assert_eq!(d.event(0).unwrap().end, Time(3000)); // a 400 ms overlap is no gap
        pp.adjacent_overlap = Some(Time(400));
        post_process(&mut d, &Rows::All, &pp);
        assert_eq!(d.event(0).unwrap().end, Time(2600));
    }

    #[test]
    fn snap_to_matches_a_scan_of_every_keyframe() {
        let kfs = [100, 300, 500, 520, 900];
        let scan = |t: i64, before: i64, after: i64| {
            let mut best: Option<i64> = None;
            for &k in &kfs {
                let d = k - t;
                let ok = (d <= 0 && -d <= before) || (d >= 0 && d <= after);
                if ok && best.is_none_or(|b| (b - t).abs() > d.abs()) {
                    best = Some(k);
                }
            }
            best.unwrap_or(t)
        };
        for t in (0..1000).step_by(10) {
            for (before, after) in [(0, 0), (100, 100), (50, 200), (250, 10)] {
                let got = snap_to(&kfs, Time(t), Time(before), Time(after)).ms();
                assert_eq!(
                    got,
                    scan(t, before, after),
                    "t {t} before {before} after {after}"
                );
            }
        }
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
            r"{\pos(100,50)\fs30\fs+2\clip(0,0,10,10)\p1}m 0 0 l 10 10",
        )]);
        resample(
            &mut d,
            &Resample {
                from: (640, 360),
                to: (1280, 720),
                ..Resample::default()
            },
        );
        assert_eq!(
            d.event(0).unwrap().text,
            r"{\pos(200,100)\fs60\fs+2\clip(0,0,20,20)\p1}m 0 0 l 20 20"
        );
        let st = d.style("Default").unwrap();
        assert_eq!(st.size, 40.0);
        assert_eq!(st.margin_l, 20);
        assert_eq!(d.play_res(), (1280, 720));
        // 720p to 1080p: Fontsize 45 becomes 68, not 67.5
        d.style_mut(0).unwrap().size = 45.0;
        let r = Resample {
            from: (1280, 720),
            to: (1920, 1080),
            ..Resample::default()
        };
        resample(&mut d, &r);
        assert_eq!(d.style("Default").unwrap().size, 68.0);
    }

    #[test]
    fn resample_aspect_modes() {
        let line = r"{\pos(0,0)\fsp2\p1}m 0 0 l 10 10";
        let run = |ar_mode, matrix: Option<(&str, &str)>| {
            let mut d = doc(&[
                ("0:00:00.00", "0:00:01.00", "Default", line),
                ("0:00:00.00", "0:00:01.00", "Default", r"{\c&H0000FF&}x"),
            ]);
            d.event_mut(0).unwrap().margin_l = 10;
            let mut tpl = d.event(0).unwrap().clone();
            tpl.kind = EventKind::Comment;
            tpl.effect = "template line".into();
            d.push_event(tpl);
            resample(
                &mut d,
                &Resample {
                    from: (640, 480),
                    to: (1280, 720),
                    ar_mode,
                    matrix: matrix.map(|(a, b)| (a.to_string(), b.to_string())),
                    ..Resample::default()
                },
            );
            d
        };
        // stretch: X doubles, Y x1.5, glyphs widen by 4/3 through \fscx
        let d = run(ArMode::Stretch, None);
        let e = d.event(0).unwrap();
        assert_eq!(e.text, r"{\pos(0,0)\fsp3\p1}m 0 0 l 15 15");
        assert_eq!(e.margin_l, 20);
        let st = d.style("Default").unwrap();
        assert!((st.scale_x - 400.0 / 3.0).abs() < 1e-9 && st.spacing == 0.0);
        assert_eq!(d.event(2).unwrap().text, line, "templates stay");
        // add borders: 106 px each side of the 4:3 source, uniform scale
        let d = run(ArMode::AddBorder, None);
        assert_eq!(
            d.event(0).unwrap().text,
            r"{\pos(159.249,0)\fsp3\p1}m 0 0 l 15.0235 15"
        );
        assert_eq!(d.style("Default").unwrap().margin_l, 174);
        // remove borders: cut 60 px off the top and the bottom instead
        let d = run(ArMode::RemoveBorder, None);
        assert_eq!(
            d.event(0).unwrap().text,
            r"{\pos(0,-120)\fsp4\p1}m 0 0 l 20 20"
        );
        // colours move between matrices
        let d = run(ArMode::Stretch, Some(("TV.601", "TV.709")));
        assert_eq!(d.event(1).unwrap().text, r"{\c&H0019FF&}x");
        assert_eq!(d.info("YCbCr Matrix"), Some("TV.709"));
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
            (
                "0:00:07.00",
                "0:00:08.00",
                "Default",
                r"{\k10}a{\k10}b{\k10}c",
            ),
        ]);
        assert_eq!(split_by_karaoke(&mut d, &[0, 1, 2]), [0, 1, 3, 4, 5]);
        let e = |i: usize| d.event(i).unwrap().clone();
        assert_eq!(
            (e(0).start.ms(), e(0).end.ms(), e(0).text),
            (1000, 1200, "ka".into())
        );
        assert_eq!((e(1).start.ms(), e(1).end.ms()), (1200, 1500));
        assert_eq!(e(2).text, "plain");
        let texts: Vec<String> = (3..6).map(|i| e(i).text).collect();
        assert_eq!(texts, ["a", "b", "c"]);
        assert_eq!(
            (e(5).start.ms(), e(5).end.ms(), d.event_count()),
            (7200, 7300, 6)
        );
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
