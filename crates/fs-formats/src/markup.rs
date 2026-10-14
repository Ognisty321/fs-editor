//! Conversion between ASS override tags and the simpler markups used by
//! SRT (`<i>`, `<b>`, `<u>`, `<s>`, `<font color>`), MPL2 (`/` italics) and
//! MicroDVD (`{y:i}` …).

use fs_core::Color;
use fs_core::tags::{Block, Line};

/// Formatting state while walking a line.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Fmt {
    pub i: bool,
    pub b: bool,
    pub u: bool,
    pub s: bool,
    pub color: Option<Color>,
}

/// A run of text with uniform formatting. `\N` is represented as '\n'.
#[derive(Debug, Clone, PartialEq)]
pub struct Run {
    pub fmt: Fmt,
    pub text: String,
}

/// Split ASS text into formatted runs; unsupported tags are dropped except
/// `\anN`, which is returned separately (players understand `{\an8}` in SRT).
pub fn ass_to_runs(text: &str) -> (Vec<Run>, Option<u8>) {
    let line = Line::parse(text);
    let mut fmt = Fmt::default();
    let mut runs: Vec<Run> = Vec::new();
    let mut an = None;
    for b in &line.blocks {
        match b {
            Block::Override { tags, .. } => {
                for t in tags {
                    let on =
                        |t: &fs_core::tags::Tag| t.arg_f64(0).map(|v| v != 0.0).unwrap_or(false);
                    match t.name.as_str() {
                        "i" => fmt.i = on(t),
                        "b" => {
                            fmt.b = t
                                .arg_f64(0)
                                .map(|v| v == 1.0 || v >= 700.0)
                                .unwrap_or(false)
                        }
                        "u" => fmt.u = on(t),
                        "s" => fmt.s = on(t),
                        "c" | "1c" => {
                            fmt.color = t
                                .arg(0)
                                .and_then(Color::parse_tag)
                                .filter(|_| !t.args[0].trim().is_empty())
                        }
                        "r" => fmt = Fmt::default(),
                        "an" => {
                            an = t
                                .arg_f64(0)
                                .map(|v| v as u8)
                                .filter(|v| (1..=9).contains(v))
                        }
                        "a" => {
                            an = t
                                .arg_f64(0)
                                .map(|v| fs_core::model::ssa_to_numpad(v as i32))
                        }
                        _ => {}
                    }
                }
            }
            Block::Text(t) => {
                let t = t
                    .replace("\\N", "\n")
                    .replace("\\n", "\n")
                    .replace("\\h", "\u{a0}");
                match runs.last_mut() {
                    Some(r) if r.fmt == fmt => r.text.push_str(&t),
                    _ => runs.push(Run {
                        fmt: fmt.clone(),
                        text: t,
                    }),
                }
            }
            Block::Drawing { .. } | Block::Comment(_) => {}
        }
    }
    (runs, an)
}

/// Build ASS text from runs (`\n` → `\N`), emitting only tags that change.
pub fn runs_to_ass(runs: &[Run], an: Option<u8>) -> String {
    let mut out = String::new();
    let mut cur = Fmt::default();
    let mut first_block = String::new();
    if let Some(a) = an {
        first_block.push_str(&format!("\\an{a}"));
    }
    let mut pending = first_block;
    for r in runs {
        let f = &r.fmt;
        if f.i != cur.i {
            pending.push_str(if f.i { "\\i1" } else { "\\i0" });
        }
        if f.b != cur.b {
            pending.push_str(if f.b { "\\b1" } else { "\\b0" });
        }
        if f.u != cur.u {
            pending.push_str(if f.u { "\\u1" } else { "\\u0" });
        }
        if f.s != cur.s {
            pending.push_str(if f.s { "\\s1" } else { "\\s0" });
        }
        if f.color != cur.color {
            match f.color {
                Some(c) => pending.push_str(&format!("\\c{}", c.to_tag_string())),
                None => pending.push_str("\\c"),
            }
        }
        if !pending.is_empty() {
            out.push('{');
            out.push_str(&pending);
            out.push('}');
            pending.clear();
        }
        cur = f.clone();
        out.push_str(&r.text.replace("\r\n", "\n").replace('\n', "\\N"));
    }
    if !pending.is_empty() {
        out.insert_str(0, &format!("{{{pending}}}"));
    }
    out
}

// ---------------------------------------------------------------------------
// HTML-ish (SRT)
// ---------------------------------------------------------------------------

/// Parse SRT/WebVTT-style markup into runs. Unknown tags are dropped; ASS
/// override blocks embedded in SRT (`{\an8}`) are honoured for alignment.
pub fn html_to_runs(text: &str) -> (Vec<Run>, Option<u8>) {
    let mut runs: Vec<Run> = Vec::new();
    let mut fmt = Fmt::default();
    let mut color_stack: Vec<Option<Color>> = Vec::new();
    let mut an = None;
    let mut rest = text;
    let push = |runs: &mut Vec<Run>, fmt: &Fmt, t: &str| {
        if t.is_empty() {
            return;
        }
        match runs.last_mut() {
            Some(r) if r.fmt == *fmt => r.text.push_str(t),
            _ => runs.push(Run {
                fmt: fmt.clone(),
                text: t.to_string(),
            }),
        }
    };
    while !rest.is_empty() {
        let next = rest.find(['<', '{']).unwrap_or(rest.len());
        push(&mut runs, &fmt, &rest[..next]);
        rest = &rest[next..];
        if rest.is_empty() {
            break;
        }
        if rest.starts_with('{') {
            match rest.find('}') {
                Some(e) => {
                    let inner = &rest[1..e];
                    if inner.contains('\\') {
                        // embedded ASS tags: take what we can represent
                        let (r, a) = ass_to_runs(&format!("{{{inner}}}x"));
                        if a.is_some() {
                            an = a;
                        }
                        if let Some(r0) = r.first() {
                            let f = &r0.fmt;
                            if inner.contains("\\i") {
                                fmt.i = f.i;
                            }
                            if inner.contains("\\b") {
                                fmt.b = f.b;
                            }
                        }
                    } else {
                        push(&mut runs, &fmt, &rest[..=e]);
                    }
                    rest = &rest[e + 1..];
                }
                None => {
                    push(&mut runs, &fmt, rest);
                    rest = "";
                }
            }
            continue;
        }
        // '<' — only a tag if followed by a letter or '/', and closed before
        // any other '<'
        let looks_like_tag = rest[1..].starts_with(|c: char| c.is_ascii_alphabetic() || c == '/');
        let e = rest
            .find('>')
            .filter(|&e| looks_like_tag && !rest[1..e].contains('<'));
        let Some(e) = e else {
            push(&mut runs, &fmt, "<");
            rest = &rest[1..];
            continue;
        };
        let tag = rest[1..e].trim();
        let lower = tag.to_ascii_lowercase();
        let (close, name) = match lower.strip_prefix('/') {
            Some(n) => (true, n.trim().to_string()),
            None => (
                false,
                lower.split_whitespace().next().unwrap_or("").to_string(),
            ),
        };
        match name.as_str() {
            "i" | "em" => fmt.i = !close,
            "b" | "strong" => fmt.b = !close,
            "u" => fmt.u = !close,
            "s" | "strike" | "del" => fmt.s = !close,
            "br" | "br/" => push(&mut runs, &fmt, "\n"),
            "font" => {
                if close {
                    fmt.color = color_stack.pop().unwrap_or(None);
                } else {
                    color_stack.push(fmt.color);
                    if let Some(c) = attr(tag, "color").and_then(|c| parse_html_color(&c)) {
                        fmt.color = Some(c);
                    }
                }
            }
            // unknown tags (<c.yellow>, <v Name>, <ruby> …) are dropped
            _ => {}
        }
        rest = &rest[e + 1..];
    }
    (runs, an)
}

fn attr(tag: &str, name: &str) -> Option<String> {
    let lower = tag.to_ascii_lowercase();
    let p = lower.find(&format!("{name}="))?;
    let v = &tag[p + name.len() + 1..];
    let v = v.trim_start();
    if let Some(q) = v.strip_prefix('"') {
        Some(q[..q.find('"').unwrap_or(q.len())].to_string())
    } else if let Some(q) = v.strip_prefix('\'') {
        Some(q[..q.find('\'').unwrap_or(q.len())].to_string())
    } else {
        Some(v.split_whitespace().next().unwrap_or("").to_string())
    }
}

fn parse_html_color(s: &str) -> Option<Color> {
    let s = s.trim();
    if s.starts_with('#') {
        return Color::parse_html(s);
    }
    Some(match s.to_ascii_lowercase().as_str() {
        "white" => Color::WHITE,
        "black" => Color::BLACK,
        "red" => Color::RED,
        "green" => Color::rgb(0, 128, 0),
        "lime" => Color::rgb(0, 255, 0),
        "blue" => Color::rgb(0, 0, 255),
        "yellow" => Color::rgb(255, 255, 0),
        "cyan" | "aqua" => Color::rgb(0, 255, 255),
        "magenta" | "fuchsia" => Color::rgb(255, 0, 255),
        "gray" | "grey" => Color::rgb(128, 128, 128),
        "orange" => Color::rgb(255, 165, 0),
        _ => return Color::parse_html(&format!("#{s}")),
    })
}

pub fn runs_to_html(runs: &[Run], an: Option<u8>) -> String {
    let mut out = String::new();
    if let Some(a) = an.filter(|a| *a != 2) {
        out.push_str(&format!("{{\\an{a}}}"));
    }
    let mut cur = Fmt::default();
    let close_all = |out: &mut String, cur: &mut Fmt, target: &Fmt| {
        // close in reverse order of opening (font, s, u, b, i)
        if cur.color.is_some() && cur.color != target.color {
            out.push_str("</font>");
            cur.color = None;
        }
        if cur.s && !target.s {
            out.push_str("</s>");
            cur.s = false;
        }
        if cur.u && !target.u {
            out.push_str("</u>");
            cur.u = false;
        }
        if cur.b && !target.b {
            out.push_str("</b>");
            cur.b = false;
        }
        if cur.i && !target.i {
            out.push_str("</i>");
            cur.i = false;
        }
    };
    for r in runs {
        close_all(&mut out, &mut cur, &r.fmt);
        if r.fmt.i && !cur.i {
            out.push_str("<i>");
        }
        if r.fmt.b && !cur.b {
            out.push_str("<b>");
        }
        if r.fmt.u && !cur.u {
            out.push_str("<u>");
        }
        if r.fmt.s && !cur.s {
            out.push_str("<s>");
        }
        if let Some(c) = r.fmt.color
            && cur.color != Some(c)
        {
            out.push_str(&format!(
                "<font color=\"#{:02x}{:02x}{:02x}\">",
                c.r, c.g, c.b
            ));
        }
        cur = r.fmt.clone();
        out.push_str(&r.text);
    }
    close_all(&mut out, &mut cur, &Fmt::default());
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ass_html_roundtrip() {
        let ass = r"{\i1}Hello{\i0} {\b1}world{\b0}\Nline {\c&H0000FF&}red{\c} x";
        let (runs, an) = ass_to_runs(ass);
        let html = runs_to_html(&runs, an);
        assert_eq!(
            html,
            "<i>Hello</i> <b>world</b>\nline <font color=\"#ff0000\">red</font> x"
        );
        let (runs2, an2) = html_to_runs(&html);
        assert_eq!(runs_to_ass(&runs2, an2), ass);
    }

    #[test]
    fn html_alignment_and_junk() {
        let (runs, an) = html_to_runs("{\\an8}<i>Top</i> <3 <unknown-tag>x");
        assert_eq!(an, Some(8));
        assert_eq!(runs_to_ass(&runs, an), r"{\an8\i1}Top{\i0} <3 x");
    }
}
