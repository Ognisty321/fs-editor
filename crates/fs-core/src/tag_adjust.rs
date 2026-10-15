//! Change one numeric override tag across lines (Kainote's "all tags" tool,
//! a.k.a. Hydra): add a value, set it, multiply it by the line's position in
//! the selection, or spread it as a gradient over lines or characters; and the
//! scale and rotation tools' "change all tags", which scale or turn every
//! occurrence. Relative changes reach the tags inside `\t` too, and a line whose
//! start has no such tag gets one there, from the style, so all of it changes.
//!
//! The functions are pure: the caller passes the text as it was before the
//! drag, so moving the slider back and forth never compounds.

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
    /// Like `Add` with `delta × line index` (the first line stays).
    Multiply,
    /// `Add` scaled 0→1 (or 1→0) from the first to the last line.
    GradientLine { increasing: bool },
    /// The tag before every character, scaled 0→1 (or 1→0) along the text.
    GradientText { increasing: bool },
}

/// Tags the tool edits, in the order the GUI lists them.
pub const ADJUSTABLE_TAGS: &[&str] = &[
    "blur", "bord", "be", "alpha", "1a", "3a", "4a", "fs", "fsp", "shad", "xbord", "ybord",
    "xshad", "yshad", "fax", "fay", "fscx", "fscy", "frz", "frx", "fry",
];

fn is_alpha(tag: &str) -> bool {
    matches!(tag, "alpha" | "1a" | "2a" | "3a" | "4a")
}

/// What the line shows without the tag.
fn style_value(style: Option<&Style>, tag: &str) -> f64 {
    let Some(s) = style else { return 0.0 };
    match tag {
        "bord" | "xbord" | "ybord" => s.outline,
        "shad" | "xshad" | "yshad" => s.shadow,
        "fs" => s.size,
        "fsp" => s.spacing,
        "fscx" => s.scale_x,
        "fscy" => s.scale_y,
        "frz" => s.angle,
        "alpha" | "1a" => f64::from(s.primary.a),
        "3a" => f64::from(s.outline_color.a),
        "4a" => f64::from(s.back.a),
        _ => 0.0,
    }
}

fn value_of(t: &Tag) -> Option<f64> {
    let a = t.args.first()?;
    if is_alpha(&t.name) {
        let h = a
            .trim()
            .trim_start_matches(['&', 'H', 'h'])
            .trim_end_matches('&');
        return u8::from_str_radix(h, 16).ok().map(f64::from);
    }
    t.arg_f64(0)
}

fn tag_with(tag: &str, v: f64) -> Tag {
    let arg = if is_alpha(tag) {
        format!("&H{:02X}&", v.round().clamp(0.0, 255.0) as u8)
    } else {
        let v = match tag {
            "blur" | "be" | "bord" | "xbord" | "ybord" | "fs" | "fscx" | "fscy" => v.max(0.0),
            _ => v,
        };
        fmt_f64((v * 100.0).round() / 100.0)
    };
    Tag::new(tag, vec![arg])
}

fn is_named(t: &Tag, tag: &str) -> bool {
    t.name == tag || (tag == "frz" && t.name == "fr")
}

/// Every occurrence of `tag` with a value, in the blocks and inside their `\t`, through `f`.
fn each_value(line: &mut Line, tag: &str, f: &impl Fn(f64) -> f64) {
    line.for_each_tag_mut(|t| {
        if is_named(t, tag) {
            if let Some(v) = value_of(t) {
                *t = tag_with(tag, f(v));
            }
        } else if let Some(mut inner) = t.transform_tags()
            && inner.iter().any(|i| is_named(i, tag))
        {
            for i in &mut inner {
                if is_named(i, tag)
                    && let Some(v) = value_of(i)
                {
                    *i = tag_with(tag, f(v));
                }
            }
            let mut args = t.args.clone();
            *args.last_mut().unwrap() = inner.iter().map(Tag::to_text).collect();
            t.set_args(args);
        }
    });
}

/// The value the line starts with: the last of its first block when the line starts with one.
fn start_value(line: &Line, tag: &str) -> Option<f64> {
    match line.blocks.first()? {
        Block::Override { tags, .. } => tags
            .iter()
            .rev()
            .filter(|t| is_named(t, tag))
            .find_map(value_of),
        _ => None,
    }
}

/// Rewrite `text` (line `index` of `count` selected lines).
pub fn adjust_tag(
    text: &str,
    style: Option<&Style>,
    tag: &str,
    mode: TagAdjust,
    delta: f64,
    index: usize,
    count: usize,
) -> String {
    let mut line = Line::parse(text);
    let start = start_value(&line, tag);
    let base = start.unwrap_or_else(|| style_value(style, tag));
    let ramp = |i: usize, n: usize, increasing: bool| {
        let f = if n < 2 {
            1.0
        } else {
            i as f64 / (n - 1) as f64
        };
        if increasing { f } else { 1.0 - f }
    };
    let add = match mode {
        TagAdjust::Scale => {
            each_value(&mut line, tag, &|v| v * delta);
            if start.is_none() {
                line.set_line_tag(tag_with(tag, base * delta));
            }
            return line.to_text();
        }
        TagAdjust::Add => delta,
        TagAdjust::Multiply => delta * index as f64,
        TagAdjust::GradientLine { increasing } => delta * ramp(index, count, increasing),
        TagAdjust::Set => {
            let mut found = false;
            line.for_each_tag_mut(|t| {
                if t.name == tag {
                    *t = tag_with(tag, delta);
                    found = true;
                }
            });
            if !found {
                line.set_line_tag(tag_with(tag, delta));
            }
            return line.to_text();
        }
        TagAdjust::GradientText { increasing } => {
            return gradient_text(line, tag, base, delta, increasing);
        }
    };
    each_value(&mut line, tag, &|v| v + add);
    if start.is_none() {
        line.set_line_tag(tag_with(tag, base + add));
    }
    line.to_text()
}

fn gradient_text(line: Line, tag: &str, base: f64, delta: f64, increasing: bool) -> String {
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
                blocks.push(Block::Override {
                    tags: vec![tag_with(tag, base + delta * f)],
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

    #[test]
    fn add_set_multiply() {
        assert_eq!(
            adjust_tag(
                r"{\blur1}a{\blur2}b",
                None,
                "blur",
                TagAdjust::Add,
                0.5,
                0,
                1
            ),
            r"{\blur1.5}a{\blur2.5}b"
        );
        assert_eq!(
            adjust_tag("a", None, "bord", TagAdjust::Add, 2.0, 0, 1),
            r"{\bord2}a"
        );
        assert_eq!(
            adjust_tag(r"{\1a&H10&}a", None, "1a", TagAdjust::Add, 16.0, 0, 1),
            r"{\1a&H20&}a"
        );
        assert_eq!(
            adjust_tag(r"{\fs20}a", None, "fs", TagAdjust::Set, 40.0, 0, 1),
            r"{\fs40}a"
        );
        assert_eq!(
            adjust_tag("a", None, "fsp", TagAdjust::Multiply, 2.0, 3, 5),
            r"{\fsp6}a"
        );
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
                2.0,
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
                0.5,
                0,
                1
            ),
            r"{\fscx25}a"
        );
        // \fr is \frz: turned, not lost
        assert_eq!(
            adjust_tag(r"{\fr10}a{\frz20}b", None, "frz", TagAdjust::Add, 5.0, 0, 1),
            r"{\frz15}a{\frz25}b"
        );
        assert_eq!(
            adjust_tag(r"a{\blur2}b", None, "blur", TagAdjust::Add, 0.5, 0, 1),
            r"{\blur0.5}a{\blur2.5}b"
        );
    }

    #[test]
    fn gradients() {
        assert_eq!(
            adjust_tag(
                "ab c",
                None,
                "blur",
                TagAdjust::GradientText { increasing: true },
                2.0,
                0,
                1
            ),
            r"{\blur0}a{\blur1}b {\blur2}c"
        );
        let line = |i| {
            adjust_tag(
                "x",
                None,
                "be",
                TagAdjust::GradientLine { increasing: false },
                4.0,
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
