//! Override tag parsing (`{\tag...}` blocks) and vector drawings (`\p`).
//!
//! Parsing is lossless: every block and tag keeps its exact source text, and
//! [`Line::to_text`] reproduces the input byte-for-byte unless something was
//! modified.

use std::fmt::Write as _;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::model::fmt_f64;

/// Known tag names, longest first so prefix matching picks e.g. `fscx` over
/// `fs`. Case matters (`K` ≠ `k`).
const TAG_NAMES: &[&str] = &[
    "xbord", "ybord", "xshad", "yshad", "iclip", "blur", "bord", "shad", "fscx", "fscy", "fade",
    "move", "clip", "alpha", "kf", "ko", "kt", "fax", "fay", "frx", "fry", "frz", "fsp", "pbo",
    "pos", "org", "fad", "1a", "2a", "3a", "4a", "1c", "2c", "3c", "4c", "an", "be", "fe", "fn",
    "fr", "fs", "a", "b", "c", "i", "k", "K", "p", "q", "r", "s", "t", "u",
];

/// Tags whose arguments are written in parentheses.
fn is_paren_tag(name: &str) -> bool {
    matches!(
        name,
        "pos" | "move" | "org" | "fad" | "fade" | "clip" | "iclip" | "t"
    )
}

#[derive(Debug, Clone, PartialEq)]
pub struct Tag {
    /// Tag name without backslash (`pos`, `1c`, `fn`, …). For unknown tags,
    /// the leading alphabetic part.
    pub name: String,
    /// Arguments. For parenthesised tags each comma-separated argument; for
    /// `\t` the last argument is the nested tag string. For simple tags a
    /// single element (possibly empty).
    pub args: Vec<String>,
    pub paren: bool,
    /// Exact source text including the leading backslash. Cleared by setters;
    /// when `None` the tag is serialised from `name`/`args`.
    pub raw: Option<String>,
}

impl Tag {
    pub fn new(name: &str, args: Vec<String>) -> Tag {
        Tag {
            name: name.to_string(),
            paren: is_paren_tag(name),
            args,
            raw: None,
        }
    }

    pub fn simple(name: &str, arg: impl Into<String>) -> Tag {
        Tag {
            name: name.to_string(),
            paren: false,
            args: vec![arg.into()],
            raw: None,
        }
    }

    pub fn is_known(&self) -> bool {
        TAG_NAMES.contains(&self.name.as_str())
    }

    pub fn arg(&self, i: usize) -> Option<&str> {
        self.args.get(i).map(|s| s.as_str())
    }

    pub fn arg_f64(&self, i: usize) -> Option<f64> {
        let a = self.args.get(i)?.trim();
        a.parse::<f64>().ok().or_else(|| {
            // tolerate trailing junk ("\fs20.5x")
            let end = a
                .char_indices()
                .find(|&(j, c)| {
                    !(c.is_ascii_digit() || c == '.' || ((c == '-' || c == '+') && j == 0))
                })
                .map(|(j, _)| j)
                .unwrap_or(a.len());
            a[..end].parse().ok()
        })
    }

    pub fn set_args(&mut self, args: Vec<String>) {
        self.args = args;
        self.raw = None;
    }

    /// For `\t`: the nested tags.
    pub fn transform_tags(&self) -> Option<Vec<Tag>> {
        if self.name != "t" {
            return None;
        }
        let inner = self.args.last()?;
        Some(parse_tags(inner))
    }

    pub fn write(&self, out: &mut String) {
        if let Some(r) = &self.raw {
            out.push_str(r);
            return;
        }
        out.push('\\');
        out.push_str(&self.name);
        if self.paren {
            out.push('(');
            out.push_str(&self.args.join(","));
            out.push(')');
        } else if let Some(a) = self.args.first() {
            out.push_str(a);
        }
    }

    pub fn to_text(&self) -> String {
        let mut s = String::new();
        self.write(&mut s);
        s
    }
}

/// A piece of a dialogue line.
#[derive(Debug, Clone, PartialEq)]
pub enum Block {
    /// Plain text (may contain `\N`, `\n`, `\h`).
    Text(String),
    /// Text while a drawing mode `\p` > 0 is active.
    Drawing { scale: u32, commands: String },
    /// `{...}` override block containing tags (and possibly junk between
    /// tags, kept in `junk`).
    Override { tags: Vec<Tag>, raw: Option<String> },
    /// `{...}` block without any backslash — a comment.
    Comment(String),
}

/// A parsed dialogue line.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Line {
    pub blocks: Vec<Block>,
}

impl Line {
    pub fn parse(text: &str) -> Line {
        let mut blocks = Vec::new();
        let mut drawing: u32 = 0;
        let mut rest = text;
        while !rest.is_empty() {
            if let Some(after) = rest.strip_prefix('{') {
                match after.find('}') {
                    Some(end) => {
                        let inner = &after[..end];
                        if inner.contains('\\') {
                            let tags = parse_tags(inner);
                            for t in &tags {
                                if t.name == "p" {
                                    drawing = t.arg_f64(0).map(|v| v.max(0.0) as u32).unwrap_or(0);
                                }
                            }
                            blocks.push(Block::Override {
                                tags,
                                raw: Some(format!("{{{inner}}}")),
                            });
                        } else {
                            blocks.push(Block::Comment(inner.to_string()));
                        }
                        rest = &after[end + 1..];
                    }
                    None => {
                        // unterminated '{': rest is text (VSFilter shows it)
                        push_text(&mut blocks, rest, drawing);
                        rest = "";
                    }
                }
            } else {
                let end = rest.find('{').unwrap_or(rest.len());
                push_text(&mut blocks, &rest[..end], drawing);
                rest = &rest[end..];
            }
        }
        Line { blocks }
    }

    pub fn to_text(&self) -> String {
        let mut s = String::new();
        for b in &self.blocks {
            match b {
                Block::Text(t) => s.push_str(t),
                Block::Drawing { commands, .. } => s.push_str(commands),
                Block::Comment(c) => {
                    s.push('{');
                    s.push_str(c);
                    s.push('}');
                }
                Block::Override { tags, raw } => match raw {
                    Some(r) => s.push_str(r),
                    None => {
                        s.push('{');
                        for t in tags {
                            t.write(&mut s);
                        }
                        s.push('}');
                    }
                },
            }
        }
        s
    }

    /// All tags in order (excluding those nested inside `\t`).
    pub fn tags(&self) -> impl Iterator<Item = &Tag> {
        self.blocks.iter().flat_map(|b| match b {
            Block::Override { tags, .. } => tags.as_slice(),
            _ => &[],
        })
    }

    /// Last occurrence of a tag (the one that takes effect for line-wide tags
    /// like `\pos`, VSFilter uses the first `\pos`; libass the first too).
    pub fn first_tag(&self, name: &str) -> Option<&Tag> {
        self.tags().find(|t| t.name == name)
    }

    /// Mutable access to every tag (not those nested inside `\t`). A tag changed through its
    /// setters, or replaced by a new one, has no source text; its block then loses its own (a
    /// copy of each block to compare against was made for every call).
    pub fn for_each_tag_mut(&mut self, mut f: impl FnMut(&mut Tag)) {
        for b in &mut self.blocks {
            if let Block::Override { tags, raw } = b {
                tags.iter_mut().for_each(&mut f);
                if tags.iter().any(|t| t.raw.is_none()) {
                    *raw = None;
                }
            }
        }
    }

    /// Set a tag at the start of the line, as Aegisub's `SetOverride` does: in the first override
    /// block (made if the line starts with text), where it takes the place of the first tag of
    /// the same name or an alias and the others go, so no later one in that block wins. Tags
    /// libass takes once for the whole line (`\pos`, `\org`, `\clip`, `\fad`, `\an`) leave the
    /// later blocks too; others there, such as `\frz`, change the text from there on and stay.
    pub fn set_line_tag(&mut self, tag: Tag) {
        let aliases: &[&str] = match tag.name.as_str() {
            "pos" | "move" => &["pos", "move"],
            "clip" | "iclip" => &["clip", "iclip"],
            "fad" | "fade" => &["fad", "fade"],
            "an" | "a" => &["an", "a"],
            // Aegisub's SetOverride drops the legacy spelling too
            "frz" | "fr" => &["frz", "fr"],
            "1c" | "c" => &["1c", "c"],
            _ => &[],
        };
        let whole_line = matches!(
            tag.name.as_str(),
            "pos" | "move" | "org" | "clip" | "iclip" | "fad" | "fade" | "an" | "a"
        );
        let name = tag.name.clone();
        let same = |t: &Tag| t.name == name || aliases.contains(&t.name.as_str());
        let mut tag = Some(tag);
        let mut first = true;
        self.blocks.retain_mut(|b| {
            let at_start = std::mem::replace(&mut first, false);
            let Block::Override { tags, raw } = b else {
                return true;
            };
            if !at_start && !whole_line {
                return true;
            }
            let n = tags.len();
            let mut kept = Vec::with_capacity(n + 1);
            for t in tags.drain(..) {
                if !same(&t) {
                    kept.push(t);
                } else if at_start && let Some(new) = tag.take() {
                    kept.push(new);
                }
            }
            if at_start && let Some(new) = tag.take() {
                kept.push(new);
            }
            if at_start || kept.len() != n {
                *raw = None;
            }
            *tags = kept;
            // a block left empty goes, one that was empty already stays
            at_start || n == 0 || !tags.is_empty()
        });
        if let Some(t) = tag {
            self.blocks.insert(
                0,
                Block::Override {
                    tags: vec![t],
                    raw: None,
                },
            );
        }
    }

    /// Remove all tags with any of the given names. Empty override blocks are
    /// dropped.
    pub fn remove_tags(&mut self, names: &[&str]) {
        for b in &mut self.blocks {
            if let Block::Override { tags, raw } = b {
                let n = tags.len();
                tags.retain(|t| !names.contains(&t.name.as_str()));
                if tags.len() != n {
                    *raw = None;
                }
            }
        }
        self.blocks
            .retain(|b| !matches!(b, Block::Override { tags, .. } if tags.is_empty()));
    }

    /// Plain text with all override blocks and comments removed.
    pub fn plain_text(&self) -> String {
        let mut s = String::new();
        for b in &self.blocks {
            if let Block::Text(t) = b {
                s.push_str(t);
            }
        }
        s
    }
}

fn push_text(blocks: &mut Vec<Block>, t: &str, drawing: u32) {
    if t.is_empty() {
        return;
    }
    if drawing > 0 {
        blocks.push(Block::Drawing {
            scale: drawing,
            commands: t.to_string(),
        });
    } else {
        blocks.push(Block::Text(t.to_string()));
    }
}

/// Byte ranges of the line's text to translate (Aegisub's translation assistant): its plain
/// text blocks, not override blocks, comments, drawings or only whitespace.
pub fn translatable_ranges(text: &str) -> Vec<std::ops::Range<usize>> {
    let mut at = 0;
    let mut out = Vec::new();
    for b in Line::parse(text).blocks {
        let len = match &b {
            Block::Text(t) => t.len(),
            Block::Drawing { commands, .. } => commands.len(),
            Block::Comment(c) => c.len() + 2,
            Block::Override { raw, .. } => raw.as_ref().map_or(0, String::len),
        };
        if let Block::Text(t) = &b
            && !t.chars().all(char::is_whitespace)
        {
            out.push(at..at + len);
        }
        at += len;
    }
    out
}

/// Parse the inside of an override block into tags. Text before the first
/// backslash and between tags that does not belong to any tag is attached to
/// the preceding tag's raw text (or kept as an unnamed tag) so nothing is lost.
pub fn parse_tags(s: &str) -> Vec<Tag> {
    let mut tags = Vec::new();
    let b = s.as_bytes();
    let mut i = 0;
    // leading junk before the first backslash
    if let Some(first) = s.find('\\') {
        if first > 0 {
            tags.push(Tag {
                name: String::new(),
                args: vec![s[..first].to_string()],
                paren: false,
                raw: Some(s[..first].to_string()),
            });
        }
        i = first;
    } else if !s.is_empty() {
        tags.push(Tag {
            name: String::new(),
            args: vec![s.to_string()],
            paren: false,
            raw: Some(s.to_string()),
        });
        return tags;
    }
    while i < b.len() {
        debug_assert_eq!(b[i], b'\\');
        let start = i;
        i += 1;
        // skip whitespace after backslash (VSFilter tolerates "\ pos")
        while i < b.len() && (b[i] == b' ' || b[i] == b'\t') {
            i += 1;
        }
        let name_start = i;
        let rest = &s[name_start..];
        let name: String = match TAG_NAMES.iter().find(|n| rest.starts_with(**n)) {
            Some(n) => n.to_string(),
            None => rest
                .chars()
                .take_while(|c| c.is_ascii_alphabetic())
                .collect(),
        };
        i = name_start + name.len();
        let paren = i < b.len()
            && b[i] == b'('
            && (is_paren_tag(&name) || !TAG_NAMES.contains(&name.as_str()));
        let args;
        if paren {
            // find the matching ')' (nested for \t), or end of block
            let mut depth = 0i32;
            let mut j = i;
            let mut close = None;
            while j < b.len() {
                match b[j] {
                    b'(' => depth += 1,
                    b')' => {
                        depth -= 1;
                        if depth == 0 {
                            close = Some(j);
                            break;
                        }
                    }
                    // a backslash at depth 1 outside \t ends an unclosed paren tag
                    b'\\' if name != "t" && depth == 1 => break,
                    _ => {}
                }
                j += 1;
            }
            let inner_end = close.unwrap_or(j);
            let inner = &s[i + 1..inner_end];
            args = split_paren_args(&name, inner);
            i = match close {
                Some(c) => c + 1,
                None => inner_end,
            };
            // junk after ')' up to the next backslash belongs to this tag's raw
            while i < b.len() && b[i] != b'\\' {
                i += 1;
            }
        } else {
            let end = s[i..].find('\\').map(|p| i + p).unwrap_or(b.len());
            args = vec![s[i..end].to_string()];
            i = end;
        }
        tags.push(Tag {
            name,
            args,
            paren,
            raw: Some(s[start..i].to_string()),
        });
    }
    tags
}

fn split_paren_args(name: &str, inner: &str) -> Vec<String> {
    if name == "t" {
        // \t([t1,t2,][accel,]tags) — the tag list starts at the first backslash
        match inner.find('\\') {
            Some(p) => {
                let mut v: Vec<String> = inner[..p]
                    .split(',')
                    .map(|x| x.trim().to_string())
                    .filter(|x| !x.is_empty())
                    .collect();
                v.push(inner[p..].to_string());
                v
            }
            None => inner.split(',').map(|x| x.trim().to_string()).collect(),
        }
    } else if name == "clip" || name == "iclip" {
        // vector clip: \clip([scale,]drawing); rectangular: \clip(x1,y1,x2,y2)
        match inner.split_once(',') {
            Some((a, d)) if a.trim().parse::<i32>().is_ok() && looks_like_drawing(d) => {
                vec![a.trim().to_string(), d.to_string()]
            }
            _ if looks_like_drawing(inner) => vec![inner.to_string()],
            _ => inner.split(',').map(|x| x.trim().to_string()).collect(),
        }
    } else {
        inner.split(',').map(|x| x.trim().to_string()).collect()
    }
}

fn looks_like_drawing(s: &str) -> bool {
    s.trim_start().starts_with(['m', 'n', 'M', 'N'])
}

/// Remove all override blocks and comments; optionally convert `\N`/`\n` to
/// newlines and `\h` to non-breaking space.
pub fn strip_tags(text: &str) -> String {
    let mut s = String::with_capacity(text.len());
    let mut depth = false;
    for c in text.chars() {
        match c {
            '{' if !depth => depth = true,
            '}' if depth => depth = false,
            _ if !depth => s.push(c),
            _ => {}
        }
    }
    s
}

/// Byte ranges of what is inside the closed `{...}` blocks: what [`strip_tags`] removes.
pub fn block_spans(text: &str) -> Vec<std::ops::Range<usize>> {
    let mut out = Vec::new();
    let mut open = None;
    for (i, c) in text.char_indices() {
        match (c, open) {
            ('{', None) => open = Some(i + 1),
            ('}', Some(s)) => {
                out.push(s..i);
                open = None;
            }
            _ => {}
        }
    }
    out
}

/// The text shown as characters: without override blocks and without the
/// drawing commands between `\pN` (N > 0) and `\p0`.
pub fn visible_text(text: &str) -> String {
    let mut s = String::with_capacity(text.len());
    let (mut rest, mut drawing) = (text, false);
    while let Some(open) = rest.find('{') {
        if !drawing {
            s.push_str(&rest[..open]);
        }
        // an unclosed block hides the rest, as in strip_tags
        let Some(len) = rest[open..].find('}') else {
            return s;
        };
        let mut block = &rest[open + 1..open + len];
        while let Some(i) = block.find("\\p") {
            block = &block[i + 2..];
            let digits = block.bytes().take_while(u8::is_ascii_digit).count();
            if digits > 0 {
                drawing = block[..digits].bytes().any(|b| b != b'0');
            }
        }
        rest = &rest[open + len + 1..];
    }
    if !drawing {
        s.push_str(rest);
    }
    s
}

/// Plain readable text: tags stripped, `\N`/`\n` → ' ', `\h` → ' '.
pub fn display_text(text: &str) -> String {
    strip_tags(text)
        .replace("\\N", " ")
        .replace("\\n", " ")
        .replace("\\h", "\u{a0}")
}

/// Characters of the visible text: tags are left out, `\N` counts as nothing, `\h`
/// as whitespace.
pub fn char_count(text: &str, ignore_whitespace: bool, ignore_punctuation: bool) -> usize {
    let t = visible_text(text)
        .replace("\\N", "")
        .replace("\\n", "")
        .replace("\\h", " ");
    t.chars()
        .filter(|c| !(ignore_whitespace && c.is_whitespace()))
        .filter(|c| !(ignore_punctuation && (c.is_ascii_punctuation() || is_unicode_punct(*c))))
        .count()
}

fn is_unicode_punct(c: char) -> bool {
    matches!(
        c,
        '…' | '–'
            | '—'
            | '„'
            | '”'
            | '“'
            | '«'
            | '»'
            | '‘'
            | '’'
            | '¿'
            | '¡'
            | '。'
            | '、'
            | '「'
            | '」'
            | '・'
    )
}

static CPS_IGNORE_WHITESPACE: AtomicBool = AtomicBool::new(true);
static CPS_IGNORE_PUNCTUATION: AtomicBool = AtomicBool::new(true);

/// What `cps` leaves out: Aegisub's character counter options, both on by default
/// (counting them is the Netflix way). A user preference, so process-wide.
pub fn set_cps_ignore(whitespace: bool, punctuation: bool) {
    CPS_IGNORE_WHITESPACE.store(whitespace, Ordering::Relaxed);
    CPS_IGNORE_PUNCTUATION.store(punctuation, Ordering::Relaxed);
}

/// Characters per second as Aegisub computes it (0 for zero-length lines).
pub fn cps(text: &str, duration_ms: i64) -> f64 {
    if duration_ms <= 0 {
        return 0.0;
    }
    let count = char_count(
        text,
        CPS_IGNORE_WHITESPACE.load(Ordering::Relaxed),
        CPS_IGNORE_PUNCTUATION.load(Ordering::Relaxed),
    );
    count as f64 * 1000.0 / duration_ms as f64
}

// ---------------------------------------------------------------------------
// Karaoke
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Syllable {
    /// Start relative to line start, ms.
    pub start_ms: i64,
    pub duration_ms: i64,
    /// `k`, `K`, `kf`, `ko`.
    pub kind: String,
    /// Syllable text including any tags other than the karaoke tag.
    pub text: String,
}

/// Split a line into karaoke syllables (like karaskel / Aegisub's karaoke
/// mode). Text before the first `\k` forms a zero-length syllable if non-empty.
pub fn karaoke_syllables(text: &str) -> Vec<Syllable> {
    let line = Line::parse(text);
    let mut syls: Vec<Syllable> = Vec::new();
    let mut cur = Syllable {
        start_ms: 0,
        duration_ms: 0,
        kind: "k".into(),
        text: String::new(),
    };
    let mut pos = 0i64;
    let mut started = false;
    for b in &line.blocks {
        match b {
            Block::Override { tags, .. } => {
                let mut other = String::new();
                for t in tags {
                    if matches!(t.name.as_str(), "k" | "K" | "kf" | "ko") {
                        if started || !cur.text.is_empty() {
                            syls.push(std::mem::take(&mut cur));
                        }
                        started = true;
                        let cs = t.arg_f64(0).unwrap_or(0.0).max(0.0);
                        cur = Syllable {
                            start_ms: pos,
                            duration_ms: (cs * 10.0).round() as i64,
                            kind: t.name.clone(),
                            text: String::new(),
                        };
                        pos += cur.duration_ms;
                    } else if t.name == "kt" {
                        // \kt sets the absolute syllable start
                        pos = (t.arg_f64(0).unwrap_or(0.0) * 10.0).round() as i64;
                    } else {
                        t.write(&mut other);
                    }
                }
                if !other.is_empty() {
                    cur.text.push('{');
                    cur.text.push_str(&other);
                    cur.text.push('}');
                }
            }
            Block::Text(t) => cur.text.push_str(t),
            Block::Drawing { commands, .. } => cur.text.push_str(commands),
            Block::Comment(c) => {
                cur.text.push('{');
                cur.text.push_str(c);
                cur.text.push('}');
            }
        }
    }
    if started || !cur.text.is_empty() {
        syls.push(cur);
    }
    syls
}

/// Rebuild line text from syllables using `\k`-style tags (centiseconds).
pub fn join_syllables(syls: &[Syllable]) -> String {
    let mut s = String::new();
    for sy in syls {
        let _ = write!(
            s,
            "{{\\{}{}}}",
            sy.kind,
            (sy.duration_ms as f64 / 10.0).round() as i64
        );
        s.push_str(&sy.text);
    }
    s
}

// ---------------------------------------------------------------------------
// Drawings
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum DrawCmd {
    Move(f64, f64),
    /// `n`: move without closing the previous shape.
    MoveNoClose(f64, f64),
    Line(f64, f64),
    Bezier([(f64, f64); 3]),
    /// `s`: cubic b-spline points (variable count, here split per point).
    Spline(f64, f64),
    /// `p`: extend b-spline.
    Extend(f64, f64),
    /// `c`: close b-spline.
    Close,
}

/// What a `\p` or `\clip` drawing's coordinates are divided by at `scale`: libass's
/// `1 << (scale - 1)` on the scale as the integer it reads (`\clip(2.5,…)` is 2), within 1..=31
/// where libass defines it (below 1 is 1, as the visual tools take it).
pub fn drawing_divisor(scale: f64) -> f64 {
    f64::from(1u32 << ((scale as i32).clamp(1, 31) - 1))
}

/// Parse vector drawing commands (`m 0 0 l 100 0 100 100 b ...`). Nothing is allocated per
/// token: a `\p1` drawing can have tens of thousands.
pub fn parse_drawing(s: &str) -> Vec<DrawCmd> {
    let mut out = Vec::new();
    let mut cmd = b'm';
    let mut nums: Vec<f64> = Vec::new();
    let flush =
        |cmd: u8, nums: &mut Vec<f64>, out: &mut Vec<DrawCmd>| {
            let pairs = nums.as_chunks::<2>().0.iter().map(|&[x, y]| (x, y));
            match cmd {
                b'm' => out.extend(pairs.map(|(x, y)| DrawCmd::Move(x, y))),
                b'n' => out.extend(pairs.map(|(x, y)| DrawCmd::MoveNoClose(x, y))),
                b'l' => out.extend(pairs.map(|(x, y)| DrawCmd::Line(x, y))),
                b'b' => out.extend(nums.as_chunks::<6>().0.iter().map(
                    |&[x1, y1, x2, y2, x3, y3]| DrawCmd::Bezier([(x1, y1), (x2, y2), (x3, y3)]),
                )),
                b's' => out.extend(pairs.map(|(x, y)| DrawCmd::Spline(x, y))),
                b'p' => out.extend(pairs.map(|(x, y)| DrawCmd::Extend(x, y))),
                _ => {}
            }
            nums.clear();
        };
    for t in s.split_whitespace() {
        if let &[c] = t.as_bytes()
            && b"mnlbspc".contains(&c.to_ascii_lowercase())
        {
            flush(cmd, &mut nums, &mut out);
            cmd = c.to_ascii_lowercase();
            if cmd == b'c' {
                out.push(DrawCmd::Close);
            }
        } else if let Ok(v) = t.parse::<f64>() {
            nums.push(v);
        }
    }
    flush(cmd, &mut nums, &mut out);
    out
}

/// Serialise drawing commands, merging consecutive commands of the same kind
/// (`l 1 2 3 4`).
pub fn write_drawing(cmds: &[DrawCmd]) -> String {
    let mut s = String::new();
    let mut last: Option<char> = None;
    let mut emit = |s: &mut String, c: char, pts: &[(f64, f64)]| {
        if last != Some(c) || c == 'm' || c == 'n' {
            if !s.is_empty() {
                s.push(' ');
            }
            s.push(c);
        }
        for (x, y) in pts {
            let _ = write!(s, " {} {}", fmt_f64(*x), fmt_f64(*y));
        }
        last = Some(c);
    };
    for c in cmds {
        match *c {
            DrawCmd::Move(x, y) => emit(&mut s, 'm', &[(x, y)]),
            DrawCmd::MoveNoClose(x, y) => emit(&mut s, 'n', &[(x, y)]),
            DrawCmd::Line(x, y) => emit(&mut s, 'l', &[(x, y)]),
            DrawCmd::Bezier(p) => emit(&mut s, 'b', &p),
            DrawCmd::Spline(x, y) => emit(&mut s, 's', &[(x, y)]),
            DrawCmd::Extend(x, y) => emit(&mut s, 'p', &[(x, y)]),
            DrawCmd::Close => emit(&mut s, 'c', &[]),
        }
    }
    s
}

/// Apply a point transform to every coordinate of a drawing.
pub fn transform_drawing(cmds: &mut [DrawCmd], f: impl Fn(f64, f64) -> (f64, f64)) {
    for c in cmds {
        match c {
            DrawCmd::Move(x, y)
            | DrawCmd::MoveNoClose(x, y)
            | DrawCmd::Line(x, y)
            | DrawCmd::Spline(x, y)
            | DrawCmd::Extend(x, y) => {
                let (nx, ny) = f(*x, *y);
                *x = nx;
                *y = ny;
            }
            DrawCmd::Bezier(p) => {
                for (x, y) in p.iter_mut() {
                    let (nx, ny) = f(*x, *y);
                    *x = nx;
                    *y = ny;
                }
            }
            DrawCmd::Close => {}
        }
    }
}

/// Bounding box `(min_x, min_y, max_x, max_y)` of a drawing's control points.
pub fn drawing_bounds(cmds: &[DrawCmd]) -> Option<(f64, f64, f64, f64)> {
    let mut b: Option<(f64, f64, f64, f64)> = None;
    let mut add = |x: f64, y: f64| {
        b = Some(match b {
            None => (x, y, x, y),
            Some((a, c, d, e)) => (a.min(x), c.min(y), d.max(x), e.max(y)),
        });
    };
    for c in cmds {
        match *c {
            DrawCmd::Move(x, y)
            | DrawCmd::MoveNoClose(x, y)
            | DrawCmd::Line(x, y)
            | DrawCmd::Spline(x, y)
            | DrawCmd::Extend(x, y) => add(x, y),
            DrawCmd::Bezier(p) => p.iter().for_each(|&(x, y)| add(x, y)),
            DrawCmd::Close => {}
        }
    }
    b
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn translatable_ranges_are_the_plain_text() {
        let t = r"{\i1}Hello {note}wörld{\i0} {\p1}m 0 0 l 1 1{\p0}end";
        let got: Vec<&str> = translatable_ranges(t).into_iter().map(|r| &t[r]).collect();
        assert_eq!(got, ["Hello ", "wörld", "end"]);
        assert!(translatable_ranges("{\\pos(1,2)}  ").is_empty());
        let open = "{open"; // unterminated: text, as VSFilter shows it
        assert_eq!(
            translatable_ranges(open)
                .into_iter()
                .map(|r| &open[r])
                .collect::<Vec<_>>(),
            [open]
        );
    }

    #[test]
    fn lossless() {
        for t in [
            r"{\pos(100,200)\c&H0000FF&}Hello{comment} world\N{\i1}x",
            r"{\t(0,500,\fscx120\frz10)\fad(100,200)}abc",
            r"{\clip(m 0 0 l 100 0 100 100 0 100)}clip",
            r"{\p1}m 0 0 l 10 0 10 10{\p0}after",
            r"{unterminated",
            r"{\pos(1,2}broken",
            r"{junk\b1 trailing\i1}x",
            r"{\fn Arial\fs20.5}y",
            "",
        ] {
            assert_eq!(Line::parse(t).to_text(), t, "{t}");
        }
    }

    #[test]
    fn tag_names_and_args() {
        let l = Line::parse(r"{\fscx120\fs20\1c&HFF&\pos(10.5, 20)\move(1,2,3,4,0,500)\K30\kf20}x");
        let tags: Vec<_> = l
            .tags()
            .map(|t| (t.name.as_str(), t.args.clone()))
            .collect();
        assert_eq!(tags[0], ("fscx", vec!["120".to_string()]));
        assert_eq!(tags[1], ("fs", vec!["20".to_string()]));
        assert_eq!(tags[2], ("1c", vec!["&HFF&".to_string()]));
        assert_eq!(tags[3], ("pos", vec!["10.5".to_string(), "20".to_string()]));
        assert_eq!(tags[4].1.len(), 6);
        assert_eq!(tags[5].0, "K");
        assert_eq!(tags[6].0, "kf");
    }

    #[test]
    fn transform_nested() {
        let l = Line::parse(r"{\t(0,500,0.5,\fscx120\t(\frz10))}x");
        let t = l.first_tag("t").unwrap();
        assert_eq!(t.args[0], "0");
        assert_eq!(t.args[2], "0.5");
        let inner = t.transform_tags().unwrap();
        assert_eq!(inner[0].name, "fscx");
    }

    #[test]
    fn vector_clip() {
        let l = Line::parse(r"{\clip(2,m 0 0 l 1 1)}x{\iclip(1,2,3,4)}");
        let c = l.first_tag("clip").unwrap();
        assert_eq!(c.args, vec!["2".to_string(), "m 0 0 l 1 1".to_string()]);
        assert_eq!(l.first_tag("iclip").unwrap().args.len(), 4);
    }

    #[test]
    fn set_pos() {
        let mut l = Line::parse(r"{\an8\pos(1,2)}x");
        l.set_line_tag(Tag::new("pos", vec!["5".into(), "6".into()]));
        assert_eq!(l.to_text(), r"{\an8\pos(5,6)}x");
        let mut l = Line::parse("plain");
        l.set_line_tag(Tag::new("pos", vec!["5".into(), "6".into()]));
        assert_eq!(l.to_text(), r"{\pos(5,6)}plain");
        l.remove_tags(&["pos"]);
        assert_eq!(l.to_text(), "plain");
    }

    #[test]
    fn set_replaces_legacy_spelling() {
        let mut l = Line::parse(r"{\fr10\c&H0000FF&}x");
        l.set_line_tag(Tag::new("frz", vec!["20".into()]));
        l.set_line_tag(Tag::new("1c", vec!["&H00FF00&".into()]));
        assert_eq!(l.to_text(), r"{\frz20\1c&H00FF00&}x");
    }

    #[test]
    fn set_at_the_start_as_aegisub() {
        // a duplicate later in the first block can't win; a later block keeps its own rotation
        let mut l = Line::parse(r"{\frz10\b1\frz20}a{\frz30}b");
        l.set_line_tag(Tag::new("frz", vec!["5".into()]));
        assert_eq!(l.to_text(), r"{\frz5\b1}a{\frz30}b");
        // a line starting with text gets the block; whole-line tags leave the other blocks
        let mut l = Line::parse(r"a{\pos(1,2)\i1}b{\move(1,2,3,4)}c{}");
        l.set_line_tag(Tag::new("pos", vec!["5".into(), "6".into()]));
        assert_eq!(l.to_text(), r"{\pos(5,6)}a{\i1}bc{}");
    }

    #[test]
    fn karaoke() {
        let s = karaoke_syllables(r"{\k20}ka{\k30\b1}ra{\kf50}o ke");
        assert_eq!(s.len(), 3);
        assert_eq!(s[0].text, "ka");
        assert_eq!(s[1].start_ms, 200);
        assert_eq!(s[1].text, r"{\b1}ra");
        assert_eq!(s[2].kind, "kf");
        assert_eq!(s[2].duration_ms, 500);
        assert_eq!(join_syllables(&s), r"{\k20}ka{\k30}{\b1}ra{\kf50}o ke");
    }

    #[test]
    fn drawing() {
        let d = parse_drawing("m 0 0 l 100 0 100 100 b 1 2 3 4 5 6 c");
        assert_eq!(d.len(), 5);
        assert_eq!(write_drawing(&d), "m 0 0 l 100 0 100 100 b 1 2 3 4 5 6 c");
        assert_eq!(drawing_bounds(&d), Some((0.0, 0.0, 100.0, 100.0)));
    }

    #[test]
    fn counting() {
        assert_eq!(strip_tags(r"{\b1}Hel{x}lo"), "Hello");
        assert_eq!(char_count(r"{\b1}Hel lo,\Nworld!", true, true), 10);
        assert!((cps("abcdefghij", 1000) - 10.0).abs() < 1e-9);
        // drawing commands are not characters; \pos and \pbo are not \p
        assert_eq!(
            visible_text(r"{\an7\p1}m 0 0 l 10 0{\p0}ab{\pos(1,2)}c"),
            "abc"
        );
        assert_eq!(visible_text(r"a{\p2\pbo5}m 0 0{\p0\b1}b{\p1}m 1 1"), "ab");
        assert_eq!(visible_text(r"x{\p10}m 0 0 l{\p00}y{unclosed"), "xy");
        assert_eq!(char_count(r"{\p1}m 0 0 l 100 0 100 100", true, true), 0);
    }
}
