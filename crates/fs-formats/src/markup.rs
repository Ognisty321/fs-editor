//! Conversion between ASS override tags and the simpler markups used by
//! SRT (`<i>`, `<b>`, `<u>`, `<s>`, `<font>`), MPL2 (`/` italics) and
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
    /// `\fn`, `<font face>`.
    pub face: Option<String>,
    /// `\fs`, `<font size>` (Aegisub's reading).
    pub size: Option<f64>,
}

impl Fmt {
    fn font(&self) -> (Option<Color>, Option<&str>, Option<f64>) {
        (self.color, self.face.as_deref(), self.size)
    }

    fn has_font(&self) -> bool {
        self.font() != (None, None, None)
    }
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
                        "fn" => {
                            fmt.face = t
                                .arg(0)
                                .map(str::trim)
                                .filter(|f| !f.is_empty())
                                .map(String::from)
                        }
                        "fs" => fmt.size = t.arg_f64(0).filter(|s| *s > 0.0),
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
                                .and_then(|v| fs_core::model::legacy_tag_align(v as i32))
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
        if f.face != cur.face {
            pending.push_str("\\fn");
            pending.push_str(f.face.as_deref().unwrap_or(""));
        }
        if f.size != cur.size {
            pending.push_str("\\fs");
            if let Some(s) = f.size {
                pending.push_str(&fs_core::model::fmt_f64(s));
            }
        }
        if !pending.is_empty() {
            // right after a block the text brought (`{\an8}<i>` in SRT): into it
            let open = out.rfind('{').filter(|&o| {
                out.ends_with('}')
                    && out[o..].starts_with("{\\")
                    && !out[o..out.len() - 1].contains('}')
            });
            if open.is_some() {
                out.pop();
            } else {
                out.push('{');
            }
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

/// Parse SRT/WebVTT-style markup into runs, as Aegisub reads it: ASS blocks in the text
/// (`{\an8}`, `{\pos(…)}`) and tags it does not know stay as they are; WebVTT's own
/// (`<c.yellow>`, `<v Name>`, `<ruby>`, timestamps) go.
pub fn html_to_runs(text: &str) -> Vec<Run> {
    let mut runs: Vec<Run> = Vec::new();
    let mut fmt = Fmt::default();
    // what each open <font> changed from
    let mut fonts: Vec<Fmt> = Vec::new();
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
    let timestamp = |t: &str| {
        t.contains(':')
            && t.chars()
                .all(|c| c.is_ascii_digit() || c == ':' || c == '.')
    };
    while !rest.is_empty() {
        let next = rest.find('<').unwrap_or(rest.len());
        push(&mut runs, &fmt, &rest[..next]);
        rest = &rest[next..];
        if rest.is_empty() {
            break;
        }
        // '<' — only a tag if followed by a letter or '/' (or a WebVTT timestamp), and closed
        // before any other '<'
        let looks_like_tag = rest[1..].starts_with(|c: char| c.is_ascii_alphabetic() || c == '/');
        let e = rest
            .find('>')
            .filter(|&e| !rest[1..e].contains('<') && (looks_like_tag || timestamp(&rest[1..e])));
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
        match name.split('.').next().unwrap_or("") {
            "i" | "em" => fmt.i = !close,
            "b" | "strong" => fmt.b = !close,
            "u" => fmt.u = !close,
            "s" | "strike" | "del" => fmt.s = !close,
            "br" | "br/" => push(&mut runs, &fmt, "\n"),
            "font" if close => {
                if let Some(f) = fonts.pop() {
                    (fmt.color, fmt.face, fmt.size) = (f.color, f.face, f.size);
                }
            }
            "font" => {
                fonts.push(fmt.clone());
                if let Some(c) = attr(tag, "color").and_then(|c| parse_html_color(&c)) {
                    fmt.color = Some(c);
                }
                if let Some(f) = attr(tag, "face").filter(|f| !f.trim().is_empty()) {
                    fmt.face = Some(f.trim().to_string());
                }
                if let Some(s) = attr(tag, "size").and_then(|s| s.trim().parse().ok()) {
                    fmt.size = Some(s).filter(|s: &f64| *s > 0.0).or(fmt.size);
                }
            }
            "c" | "v" | "ruby" | "rt" | "lang" => {}
            _ if timestamp(tag) => {}
            // anything else is text: `<Narrator>`, a stray `<3>` (Aegisub)
            _ => push(&mut runs, &fmt, &rest[..=e]),
        }
        rest = &rest[e + 1..];
    }
    runs
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
        if cur.has_font() && cur.font() != target.font() {
            out.push_str("</font>");
            (cur.color, cur.face, cur.size) = (None, None, None);
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
        if r.fmt.has_font() && cur.font() != r.fmt.font() {
            out.push_str("<font");
            if let Some(f) = &r.fmt.face {
                out.push_str(&format!(" face=\"{f}\""));
            }
            if let Some(s) = r.fmt.size {
                out.push_str(&format!(" size=\"{}\"", fs_core::model::fmt_f64(s)));
            }
            if let Some(c) = r.fmt.color {
                out.push_str(&format!(" color=\"#{:02x}{:02x}{:02x}\"", c.r, c.g, c.b));
            }
            out.push('>');
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
        assert_eq!(runs_to_ass(&html_to_runs(&html), None), ass);
        // face and size, Aegisub's way, there and back
        let ass = r"a {\c&H0000FF&\fnComic Sans\fs30}b{\c\fn\fs} c";
        let html = runs_to_html(&ass_to_runs(ass).0, None);
        assert_eq!(
            html,
            "a <font face=\"Comic Sans\" size=\"30\" color=\"#ff0000\">b</font> c"
        );
        assert_eq!(runs_to_ass(&html_to_runs(&html), None), ass);
    }

    #[test]
    fn html_keeps_what_it_does_not_know() {
        let ass = |html: &str| runs_to_ass(&html_to_runs(html), None);
        // ASS blocks and unknown tags stay (Aegisub); ours join the block before them
        assert_eq!(
            ass("{\\an8}<i>Top</i> <3 <unknown-tag>x"),
            r"{\an8\i1}Top{\i0} <3 <unknown-tag>x"
        );
        assert_eq!(
            ass("{\\pos(1,2)\\fad(0,200)}<b>a</b>{note}"),
            r"{\pos(1,2)\fad(0,200)\b1}a{\b0}{note}"
        );
        assert_eq!(ass("a}<i>b"), r"a}{\i1}b");
        // WebVTT's own tags and timestamps go
        assert_eq!(
            ass("<v Bob><c.yellow>Hi</c> <00:00:01.500>there</v> <ruby>漢<rt>kan</rt></ruby>"),
            "Hi there 漢kan"
        );
        // a font closes back to the one around it
        assert_eq!(
            ass("<font color=red>a<font size=20>b</font>c</font>d"),
            r"{\c&H0000FF&}a{\fs20}b{\fs}c{\c}d"
        );
    }
}
