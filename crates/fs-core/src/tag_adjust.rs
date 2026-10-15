//! Change one override tag across lines (Kainote's "all tags" tool, a.k.a. Hydra): add to its
//! values, set them everywhere or in one place, step them from line to line, or spread them as a
//! gradient over lines or characters; and the scale and rotation tools' "change all tags", which
//! scale or turn every occurrence. A tag's values are its alpha (`\1a`), a colour's red, green and
//! blue (`\1c`), or its leading numbers (`\blur`, `\pos`, `\fad`, the times of `\t`). Relative
//! changes reach the tags inside `\t` too, and a line whose start has no such tag gets one there,
//! from the style, so all of it changes.
//!
//! The functions are pure: the caller passes the text as it was before the
//! drag, so moving the slider back and forth never compounds.

use crate::color::Color;
use crate::model::{Style, fmt_f64};
use crate::tags::{Block, Line, Tag};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TagAdjust {
    /// Every occurrence gets `delta` added (a missing tag starts from the style).
    Add,
    /// Every occurrence multiplied by `delta` (a missing one starts from the style).
    Scale,
    /// Every occurrence becomes `delta` (inserted when missing).
    Set,
    /// `delta` in one place (Kainote's insert): the override block at the byte range `at` of the
    /// text (a new one there in plain text), else the line's start. For `\t` the selected tags
    /// become what it animates.
    Insert { at: Option<(usize, usize)> },
    /// Like `Add` with `delta × line index` (the first line stays).
    Multiply,
    /// `Add` scaled 0→1 (or 1→0) from the first to the last line.
    GradientLine { increasing: bool },
    /// The tag before every character, scaled 0→1 (or 1→0) along the text.
    GradientText { increasing: bool },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Kind {
    Alpha,
    Color,
    /// The leading numeric arguments.
    Numbers,
}

fn kind(tag: &str) -> Kind {
    match tag {
        "alpha" | "1a" | "2a" | "3a" | "4a" => Kind::Alpha,
        "c" | "1c" | "2c" | "3c" | "4c" => Kind::Color,
        _ => Kind::Numbers,
    }
}

/// How many values the tag has, `wanted` for plain numbers.
fn arity(tag: &str, wanted: usize) -> usize {
    match kind(tag) {
        Kind::Alpha => 1,
        Kind::Color => 3,
        Kind::Numbers => wanted.clamp(1, 4),
    }
}

/// Tags a line without them does not get from nothing: an empty `\t`, a `\move`, a clip.
fn made_from_nothing(tag: &str) -> bool {
    !matches!(tag, "t" | "move" | "clip" | "iclip" | "p")
}

/// What the line shows without the tag; nothing for a tag the style does not hold.
fn style_value(style: Option<&Style>, tag: &str, n: usize) -> Option<Vec<f64>> {
    let s = style?;
    let rgb = |c: Color| vec![f64::from(c.r), f64::from(c.g), f64::from(c.b)];
    Some(match tag {
        "c" | "1c" => rgb(s.primary),
        "2c" => rgb(s.secondary),
        "3c" => rgb(s.outline_color),
        "4c" => rgb(s.back),
        "alpha" | "1a" => vec![f64::from(s.primary.a)],
        "2a" => vec![f64::from(s.secondary.a)],
        "3a" => vec![f64::from(s.outline_color.a)],
        "4a" => vec![f64::from(s.back.a)],
        _ => {
            let one = match tag {
                "bord" | "xbord" | "ybord" => s.outline,
                "shad" | "xshad" | "yshad" => s.shadow,
                "fs" => s.size,
                "fsp" => s.spacing,
                "fscx" => s.scale_x,
                "fscy" => s.scale_y,
                "frz" => s.angle,
                _ => return None,
            };
            let mut v = vec![0.0; n];
            v[0] = one;
            v
        }
    })
}

/// The tag's `n` values, when it has them (`\t` only with its times).
fn value_of(t: &Tag, tag: &str, n: usize) -> Option<Vec<f64>> {
    match kind(tag) {
        Kind::Alpha => Some(vec![f64::from(Color::parse_alpha(t.args.first()?)?)]),
        Kind::Color => {
            let c = Color::parse_tag(t.args.first()?)?;
            Some(vec![f64::from(c.r), f64::from(c.g), f64::from(c.b)])
        }
        Kind::Numbers => {
            // \t's last argument is the tags; its times come first, when it has them
            let numbers = if tag == "t" {
                if t.args.len() < 3 {
                    return None;
                }
                t.args.len() - 1
            } else {
                t.args.len()
            };
            (n <= numbers).then_some(())?;
            (0..n).map(|i| t.arg_f64(i)).collect()
        }
    }
}

fn round(v: f64) -> String {
    let r = (v * 100.0).round() / 100.0;
    fmt_f64(if r == 0.0 { 0.0 } else { r })
}

/// The tag with `v`, its other arguments kept from `like`.
fn tag_with(tag: &str, v: &[f64], like: Option<&Tag>) -> Tag {
    let byte = |x: f64| x.round().clamp(0.0, 255.0) as u8;
    match kind(tag) {
        Kind::Alpha => Tag::new(tag, vec![Color::alpha_tag_string(byte(v[0]))]),
        Kind::Color => {
            let c = Color {
                r: byte(v[0]),
                g: byte(v[1]),
                b: byte(v[2]),
                a: 0,
            };
            Tag::new(tag, vec![c.to_tag_string()])
        }
        Kind::Numbers => {
            let non_negative = matches!(
                tag,
                "blur" | "be" | "bord" | "xbord" | "ybord" | "fs" | "fscx" | "fscy"
            );
            let mut args: Vec<String> = like.map(|t| t.args.clone()).unwrap_or_default();
            for (i, &x) in v.iter().enumerate() {
                let s = round(if non_negative { x.max(0.0) } else { x });
                match args.get_mut(i) {
                    Some(a) => *a = s,
                    None => args.push(s),
                }
            }
            Tag::new(tag, args)
        }
    }
}

fn is_named(t: &Tag, tag: &str) -> bool {
    t.name == tag
        || (tag == "frz" && t.name == "fr")
        || (tag == "1c" && t.name == "c")
        || (tag == "fad" && t.name == "fade" && t.args.len() == 2)
}

/// Every occurrence of `tag` with its values, in the blocks and inside their `\t`, through `f`.
fn each_value(line: &mut Line, tag: &str, n: usize, f: &impl Fn(&[f64]) -> Vec<f64>) {
    line.for_each_tag_mut(|t| {
        if is_named(t, tag) {
            if let Some(v) = value_of(t, tag, n) {
                *t = tag_with(tag, &f(&v), Some(t));
            }
        } else if tag != "t"
            && let Some(mut inner) = t.transform_tags()
            && inner.iter().any(|i| is_named(i, tag))
        {
            for i in &mut inner {
                if is_named(i, tag)
                    && let Some(v) = value_of(i, tag, n)
                {
                    *i = tag_with(tag, &f(&v), Some(i));
                }
            }
            let mut args = t.args.clone();
            *args.last_mut().unwrap() = inner.iter().map(Tag::to_text).collect();
            t.set_args(args);
        }
    });
}

/// The values the line starts with: the last of its first block when the line starts with one.
fn start_value(line: &Line, tag: &str, n: usize) -> Option<Vec<f64>> {
    match line.blocks.first()? {
        Block::Override { tags, .. } => tags
            .iter()
            .rev()
            .filter(|t| is_named(t, tag))
            .find_map(|t| value_of(t, tag, n)),
        _ => None,
    }
}

/// The values the line starts with, the style's when its start has no such tag (the all-tags
/// tool's sliders); `count` for plain numbers. Empty when neither has the tag.
pub fn line_values(text: &str, style: Option<&Style>, tag: &str, count: usize) -> Vec<f64> {
    let n = arity(tag, count);
    (start_value(&Line::parse(text), tag, n))
        .or_else(|| style_value(style, tag, n))
        .unwrap_or_default()
}

/// Rewrite `text` (line `index` of `count` selected lines). `delta` holds the tag's values (one
/// for plain numbers, as many as it has for the others; a missing one adds nothing).
pub fn adjust_tag(
    text: &str,
    style: Option<&Style>,
    tag: &str,
    mode: TagAdjust,
    delta: &[f64],
    index: usize,
    count: usize,
) -> String {
    let n = arity(tag, delta.len());
    let d = |i: usize| {
        (delta.get(i).copied()).unwrap_or(if mode == TagAdjust::Scale { 1.0 } else { 0.0 })
    };
    let mut line = Line::parse(text);
    let start = start_value(&line, tag, n);
    let base = (start.clone())
        .or_else(|| style_value(style, tag, n))
        .unwrap_or_else(|| vec![0.0; n]);
    let ramp = |i: usize, n: usize, increasing: bool| {
        let f = if n < 2 {
            1.0
        } else {
            i as f64 / (n - 1) as f64
        };
        if increasing { f } else { 1.0 - f }
    };
    let k = match mode {
        TagAdjust::Scale => {
            let scaled = |v: &[f64]| (0..n).map(|i| v[i] * d(i)).collect::<Vec<_>>();
            each_value(&mut line, tag, n, &scaled);
            if start.is_none() && made_from_nothing(tag) {
                line.set_line_tag(tag_with(tag, &scaled(&base), None));
            }
            return line.to_text();
        }
        TagAdjust::Add => 1.0,
        TagAdjust::Multiply => index as f64,
        TagAdjust::GradientLine { increasing } => ramp(index, count, increasing),
        TagAdjust::Set => {
            let v: Vec<f64> = (0..n)
                .map(|i| delta.get(i).copied().unwrap_or(base[i]))
                .collect();
            let mut found = false;
            line.for_each_tag_mut(|t| {
                if is_named(t, tag) && (tag != "t" || value_of(t, tag, n).is_some()) {
                    *t = tag_with(tag, &v, Some(t));
                    found = true;
                }
            });
            if !found && made_from_nothing(tag) {
                line.set_line_tag(tag_with(tag, &v, None));
            }
            return line.to_text();
        }
        TagAdjust::Insert { at } => {
            let v: Vec<f64> = (0..n)
                .map(|i| delta.get(i).copied().unwrap_or(base[i]))
                .collect();
            return insert(text, line, tag, &v, at);
        }
        TagAdjust::GradientText { increasing } => {
            return gradient_text(
                line,
                tag,
                &base,
                &(0..n).map(d).collect::<Vec<_>>(),
                increasing,
            );
        }
    };
    let moved = |v: &[f64]| (0..n).map(|i| v[i] + d(i) * k).collect::<Vec<_>>();
    each_value(&mut line, tag, n, &moved);
    if start.is_none() && made_from_nothing(tag) {
        line.set_line_tag(tag_with(tag, &moved(&base), None));
    }
    line.to_text()
}

/// The top-level tags of the override block `{…}` at `open..=close` of `text`: where each starts
/// and ends.
fn block_tags(text: &str, open: usize, close: usize) -> Vec<(usize, usize)> {
    let mut starts = Vec::new();
    let mut depth = 0i32;
    for (i, c) in text[open + 1..close].char_indices() {
        match c {
            '(' => depth += 1,
            ')' => depth -= 1,
            '\\' if depth <= 0 => starts.push(open + 1 + i),
            _ => {}
        }
    }
    let ends = starts.iter().skip(1).copied().chain([close]);
    starts.iter().copied().zip(ends).collect()
}

fn insert(text: &str, line: Line, tag: &str, v: &[f64], at: Option<(usize, usize)>) -> String {
    let at = at.filter(|&(a, b)| {
        a <= b && b <= text.len() && text.is_char_boundary(a) && text.is_char_boundary(b)
    });
    let Some((mut a, mut b)) =
        at.or_else(|| (tag == "t").then(|| (usize::from(text.starts_with('{')), 0)))
    else {
        // the line's start, keeping its tag's other arguments
        let mut line = line;
        let like = match line.blocks.first() {
            Some(Block::Override { tags, .. }) => {
                tags.iter().rev().find(|t| is_named(t, tag)).cloned()
            }
            _ => None,
        };
        line.set_line_tag(tag_with(tag, v, like.as_ref()));
        return line.to_text();
    };
    b = b.max(a);
    // right after a block counts as inside it
    if a == b && text[..a].ends_with('}') {
        a -= 1;
        b = a;
    }
    let open = text[..a]
        .rfind('{')
        .filter(|&o| text[..a].rfind('}').is_none_or(|c| c < o));
    let block = open.and_then(|o| text[o..].find('}').map(|e| (o, o + e)));
    let new = |like: Option<&str>| -> String {
        if tag == "t" {
            // the selected tags, when they are tags, become what \t animates
            let sel = text[a..b].trim();
            let inner = if sel.starts_with('\\') { sel } else { "" };
            let mut t = Tag::new("t", Vec::new());
            t.set_args(
                v.iter()
                    .map(|&x| round(x))
                    .chain([inner.to_string()])
                    .collect(),
            );
            return t.to_text();
        }
        let like = like.and_then(|s| Line::parse(&format!("{{{s}}}")).tags().next().cloned());
        tag_with(tag, v, like.as_ref()).to_text()
    };
    let Some((o, e)) = block else {
        return format!("{}{{{}}}{}", &text[..a], new(None), &text[a..]);
    };
    let tags = block_tags(text, o, e);
    if tag == "t" && a < b {
        return format!("{}{}{}", &text[..a], new(None), &text[b..]);
    }
    if tag != "t"
        && let Some(&(s, f)) = tags.iter().rev().find(|&&(s, f)| {
            Line::parse(&format!("{{{}}}", &text[s..f]))
                .tags()
                .next()
                .is_some_and(|t| is_named(t, tag))
        })
    {
        return format!("{}{}{}", &text[..s], new(Some(&text[s..f])), &text[f..]);
    }
    // at the cursor, moved to where a tag starts
    let p = tags.iter().map(|&(s, _)| s).find(|&s| s >= a).unwrap_or(e);
    format!("{}{}{}", &text[..p], new(None), &text[p..])
}

fn gradient_text(line: Line, tag: &str, base: &[f64], delta: &[f64], increasing: bool) -> String {
    // visible characters: everything but spaces and the \N \n \h escapes
    let chars = |s: &str| {
        let mut out = Vec::new();
        let mut it = s.char_indices().peekable();
        while let Some((i, c)) = it.next() {
            if c == '\\' && matches!(it.peek(), Some((_, 'N' | 'n' | 'h'))) {
                let (j, e) = it.next().unwrap();
                out.push((i, j + e.len_utf8(), false));
            } else {
                out.push((i, i + c.len_utf8(), !c.is_whitespace()));
            }
        }
        out
    };
    let total: usize = line
        .blocks
        .iter()
        .map(|b| match b {
            Block::Text(s) => chars(s).iter().filter(|c| c.2).count(),
            _ => 0,
        })
        .sum();
    let mut k = 0usize;
    let mut blocks = Vec::new();
    for b in line.blocks {
        let Block::Text(s) = b else {
            blocks.push(b);
            continue;
        };
        let mut plain = String::new();
        for (from, to, visible) in chars(&s) {
            if visible {
                if !plain.is_empty() {
                    blocks.push(Block::Text(std::mem::take(&mut plain)));
                }
                let f = if total < 2 {
                    1.0
                } else {
                    k as f64 / (total - 1) as f64
                };
                let f = if increasing { f } else { 1.0 - f };
                let v: Vec<f64> = base.iter().zip(delta).map(|(b, d)| b + d * f).collect();
                blocks.push(Block::Override {
                    tags: vec![tag_with(tag, &v, None)],
                    raw: None,
                });
                k += 1;
            }
            plain.push_str(&s[from..to]);
        }
        if !plain.is_empty() {
            blocks.push(Block::Text(plain));
        }
    }
    Line { blocks }.to_text()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn one(text: &str, tag: &str, mode: TagAdjust, d: f64) -> String {
        adjust_tag(text, None, tag, mode, &[d], 0, 1)
    }

    #[test]
    fn add_set_multiply() {
        assert_eq!(
            one(r"{\blur1}a{\blur2}b", "blur", TagAdjust::Add, 0.5),
            r"{\blur1.5}a{\blur2.5}b"
        );
        assert_eq!(one("a", "bord", TagAdjust::Add, 2.0), r"{\bord2}a");
        assert_eq!(
            one(r"{\1a&H10&}a", "1a", TagAdjust::Add, 16.0),
            r"{\1a&H20&}a"
        );
        assert_eq!(one(r"{\fs20}a", "fs", TagAdjust::Set, 40.0), r"{\fs40}a");
        assert_eq!(
            adjust_tag("a", None, "fsp", TagAdjust::Multiply, &[2.0], 3, 5),
            r"{\fsp6}a"
        );
    }

    #[test]
    fn colours_positions_and_times_have_several_values() {
        // red, green and blue each by its own, clamped; \c is \1c
        assert_eq!(
            adjust_tag(
                r"{\c&H102030&}a",
                None,
                "1c",
                TagAdjust::Add,
                &[16.0, 0.0, 250.0],
                0,
                1
            ),
            r"{\1c&HFF2040&}a"
        );
        // the style's colour where the line has none
        let style = Style::default();
        assert_eq!(
            adjust_tag("a", Some(&style), "3c", TagAdjust::Set, &[1.0, 2.0], 0, 1),
            r"{\3c&H000201&}a"
        );
        assert_eq!(
            adjust_tag(
                r"{\pos(10,20)}a",
                None,
                "pos",
                TagAdjust::Add,
                &[5.0, -5.0],
                0,
                1
            ),
            r"{\pos(15,15)}a"
        );
        // \t's times, not its accel or tags; a \t without times stays
        assert_eq!(
            adjust_tag(
                r"{\t(0,100,2,\blur3)\t(\bord2)}a",
                None,
                "t",
                TagAdjust::Add,
                &[50.0, 50.0],
                0,
                1
            ),
            r"{\t(50,150,2,\blur3)\t(\bord2)}a"
        );
        // and no \t made from nothing
        assert_eq!(
            adjust_tag("a", None, "t", TagAdjust::Set, &[0.0, 9.0], 0, 1),
            "a"
        );
        assert_eq!(
            line_values(r"{\fad(100,200)}a", None, "fad", 2),
            [100.0, 200.0]
        );
        assert!(line_values("a", Some(&Style::default()), "fad", 2).is_empty());
    }

    #[test]
    fn insert_goes_to_the_cursor_or_the_start() {
        let ins = |text: &str, tag: &str, v: &[f64], at: Option<(usize, usize)>| {
            adjust_tag(text, None, tag, TagAdjust::Insert { at }, v, 0, 1)
        };
        // in plain text: a block of its own there
        assert_eq!(ins("ab", "blur", &[2.0], Some((1, 1))), r"a{\blur2}b");
        // right after a block, or in it: into that block, over its own tag of the name
        assert_eq!(
            ins(r"{\bord1}a{\blur1}b", "blur", &[3.0], Some((15, 15))),
            r"{\bord1}a{\blur3}b"
        );
        assert_eq!(
            ins(r"{\bord1\shad2}a", "blur", &[3.0], Some((7, 7))),
            r"{\bord1\blur3\shad2}a"
        );
        // no cursor: the line's start, other blocks left
        assert_eq!(
            ins(r"a{\blur1}b", "blur", &[4.0], None),
            r"{\blur4}a{\blur1}b"
        );
        // \t around the selected tags, or empty at the start to be filled in
        assert_eq!(
            ins(r"{\bord1\blur5}a", "t", &[0.0, 500.0], Some((7, 13))),
            r"{\bord1\t(0,500,\blur5)}a"
        );
        assert_eq!(ins("a", "t", &[0.0, 500.0], None), r"{\t(0,500,)}a");
    }

    #[test]
    fn change_all_reaches_transforms_and_the_start() {
        // the start shows the style's 100%: it gets a tag of its own, scaled like the rest
        let style = Style::default();
        assert_eq!(
            adjust_tag(
                r"a{\fscx50\t(\fscx80)}b",
                Some(&style),
                "fscx",
                TagAdjust::Scale,
                &[2.0],
                0,
                1
            ),
            r"{\fscx200}a{\fscx100\t(\fscx160)}b"
        );
        assert_eq!(
            adjust_tag(
                r"{\fscx50}a",
                Some(&style),
                "fscx",
                TagAdjust::Scale,
                &[0.5],
                0,
                1
            ),
            r"{\fscx25}a"
        );
        // \fr is \frz: turned, not lost
        assert_eq!(
            one(r"{\fr10}a{\frz20}b", "frz", TagAdjust::Add, 5.0),
            r"{\frz15}a{\frz25}b"
        );
        assert_eq!(
            one(r"a{\blur2}b", "blur", TagAdjust::Add, 0.5),
            r"{\blur0.5}a{\blur2.5}b"
        );
    }

    #[test]
    fn gradients() {
        assert_eq!(
            one(
                "ab c",
                "blur",
                TagAdjust::GradientText { increasing: true },
                2.0
            ),
            r"{\blur0}a{\blur1}b {\blur2}c"
        );
        let line = |i| {
            adjust_tag(
                "x",
                None,
                "be",
                TagAdjust::GradientLine { increasing: false },
                &[4.0],
                i,
                3,
            )
        };
        assert_eq!(
            [line(0), line(1), line(2)],
            [r"{\be4}x", r"{\be2}x", r"{\be0}x"]
        );
    }
}
