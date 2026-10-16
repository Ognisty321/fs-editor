//! Kainote's splits that keep the look: the pieces of a line (characters, words, wraps) each get the
//! line's times and their own `\pos` (or `\move`) where they were, and the frames of a moving line
//! each get the `\pos` the `\move` is at on that frame.

use fs_core::model::Style;
use fs_core::tags::{Block, Line, Tag};
use fs_core::{Document, Event};

fn num(v: f64) -> String {
    let r = (v * 100.0).round() / 100.0;
    if r == 0.0 { "0".into() } else { format!("{r}") }
}

/// One character, word or wrap's worth of plain text as it is laid out.
struct Unit {
    text: String,
    width: f64,
    height: f64,
    space: bool,
    /// The override tags met between the unit before and this one.
    before: String,
    /// Every override tag met before it (positioning ones left out).
    carried: String,
}

fn extents(style: &Style, text: &str) -> (f64, f64) {
    let (w, h, _, _) =
        fs_automation::text_extents(style, &text.replace("\\h", "\u{a0}").replace("\\n", " "));
    (w, h)
}

/// `text` (a part of a plain text block between `\N`s) cut into units: every character (`how` 2), runs of
/// spaces and of the rest (3), or all of it (4); `\h` and `\n` are spaces.
fn cut(text: &str, how: u8) -> Vec<(String, bool)> {
    let mut chars: Vec<(String, bool)> = Vec::new();
    let mut rest = text;
    while let Some(c) = rest.chars().next() {
        let esc = rest.starts_with("\\h") || rest.starts_with("\\n");
        let len = if esc { 2 } else { c.len_utf8() };
        chars.push((rest[..len].to_string(), esc || c.is_whitespace()));
        rest = &rest[len..];
    }
    match how {
        2 => chars,
        3 => {
            let mut out: Vec<(String, bool)> = Vec::new();
            for (s, space) in chars {
                match out.last_mut() {
                    Some((t, sp)) if *sp == space => t.push_str(&s),
                    _ => out.push((s, space)),
                }
            }
            out
        }
        _ if text.is_empty() => Vec::new(),
        _ => vec![(text.to_string(), false)],
    }
}

/// Characters (`how` 2), words (3) or wraps (4) of `text` (the line `ev`'s, or its original in translator
/// mode), each a line of its own placed where it was: `\an`, `\pos` or `\move` and the tags before it.
/// Empty when there is not more than one piece, or the line is a drawing.
pub fn split_positioned(doc: &Document, ev: &Event, text: &str, how: u8) -> Vec<String> {
    let mut ev = ev.clone();
    ev.text = text.to_string();
    let v = crate::visual::of_event(doc, &ev);
    let line = Line::parse(text);
    if line
        .blocks
        .iter()
        .any(|b| matches!(b, Block::Drawing { .. }))
    {
        return Vec::new();
    }
    let base = doc.style(&ev.style).cloned().unwrap_or_default();
    let mut style = base.clone();
    let mut lines: Vec<Vec<Unit>> = vec![Vec::new()];
    let mut heights: Vec<f64> = Vec::new(); // of an empty wrap, in the style it starts in
    let (mut carried, mut before) = (String::new(), String::new());
    let mut rotated = false;
    let empty_height = |s: &Style| extents(s, "Ag").1;
    for b in &line.blocks {
        match b {
            Block::Override { tags, .. } => {
                for t in tags {
                    let arg = t.arg(0).unwrap_or("");
                    let val = t.arg_f64(0);
                    match t.name.as_str() {
                        "pos" | "move" | "an" | "a" => continue,
                        "r" => {
                            style = doc
                                .style(arg)
                                .filter(|_| !arg.is_empty())
                                .unwrap_or(&base)
                                .clone()
                        }
                        "fn" if !arg.is_empty() => style.font = arg.to_string(),
                        "fs" => style.size = val.filter(|v| *v > 0.0).unwrap_or(base.size),
                        "fscx" => style.scale_x = val.unwrap_or(base.scale_x),
                        "fscy" => style.scale_y = val.unwrap_or(base.scale_y),
                        "fsp" => style.spacing = val.unwrap_or(base.spacing),
                        "b" => style.bold = val.map_or(base.bold, |v| v as i32),
                        "i" => style.italic = val.map_or(base.italic, |v| v != 0.0),
                        "fr" | "frz" | "frx" | "fry" => rotated = true,
                        _ => {}
                    }
                    t.write(&mut carried);
                    t.write(&mut before);
                }
            }
            Block::Text(s) => {
                for (k, part) in s.split("\\N").enumerate() {
                    if k > 0 {
                        heights.push(empty_height(&style));
                        lines.push(Vec::new());
                    }
                    for (text, space) in cut(part, how) {
                        let (width, height) = extents(&style, &text);
                        let before = std::mem::take(&mut before);
                        lines.last_mut().expect("one at least").push(Unit {
                            text,
                            width,
                            height,
                            space,
                            before,
                            carried: carried.clone(),
                        });
                    }
                }
            }
            Block::Comment(_) | Block::Drawing { .. } => {}
        }
    }
    heights.push(empty_height(&style));
    // the block of wraps stands on the position as the alignment says, each wrap aligned on its own
    let an = v.align;
    let line_h: Vec<f64> = (lines.iter().zip(&heights))
        .map(|(l, &e)| {
            l.iter()
                .map(|u| u.height)
                .fold(0.0, f64::max)
                .max(if l.is_empty() { e } else { 0.0 })
        })
        .collect();
    let total: f64 = line_h.iter().sum();
    let mut top = match an {
        7..=9 => v.y,
        4..=6 => v.y - total / 2.0,
        _ => v.y - total,
    };
    let anchor = |left: f64, w: f64| match an % 3 {
        1 => left,
        2 => left + w / 2.0,
        _ => left + w,
    };
    // shortcut: a line wrapped by the renderer (no \N) splits as one wrap, so only \N wraps keep their places
    let (dx, dy) = (v.x2 - v.x, v.y2 - v.y);
    let org = if rotated && !v.has_org && !v.has_move {
        format!("\\org({},{})", num(v.x), num(v.y))
    } else {
        String::new()
    };
    let place = |x: f64, y: f64| {
        let at = if v.has_move {
            let times = if v.t1 != 0 || v.t2 != 0 {
                format!(",{},{}", v.t1, v.t2)
            } else {
                String::new()
            };
            format!(
                "\\move({},{},{},{}{times})",
                num(x),
                num(y),
                num(x + dx),
                num(y + dy)
            )
        } else {
            format!("\\pos({},{})", num(x), num(y))
        };
        format!("{{\\an{an}{at}{org}")
    };
    let mut out = Vec::new();
    for (units, h) in lines.iter().zip(&line_h) {
        let width: f64 = units.iter().map(|u| u.width).sum();
        let y = top
            + match an {
                7..=9 => 0.0,
                4..=6 => h / 2.0,
                _ => *h,
            };
        top += h;
        if how == 4 {
            // the wrap from its first to its last visible unit, its tags inside kept
            let Some(a) = units.iter().position(|u| !u.space) else {
                continue;
            };
            let b = units
                .iter()
                .rposition(|u| !u.space)
                .expect("found one above");
            let mut s = place(v.x, y) + &units[a].carried + "}" + &units[a].text;
            for u in &units[a + 1..=b] {
                if !u.before.is_empty() {
                    s += &format!("{{{}}}", u.before);
                }
                s += &u.text;
            }
            out.push(s);
            continue;
        }
        let mut left = match an % 3 {
            1 => v.x,
            2 => v.x - width / 2.0,
            _ => v.x - width,
        };
        for u in units {
            if !u.space {
                out.push(place(anchor(left, u.width), y) + &u.carried + "}" + &u.text);
            }
            left += u.width;
        }
    }
    if out.len() < 2 { Vec::new() } else { out }
}

/// The text of the line `ev` for each of `times` (ms): its first `\move` as the `\pos` it is at then
/// (libass: no times move over the whole line). Unchanged without a `\move`.
pub fn frame_texts(ev: &Event, text: &str, times: &[i64]) -> Vec<String> {
    let mut line = Line::parse(text);
    let found = line
        .blocks
        .iter()
        .enumerate()
        .find_map(|(b, block)| match block {
            Block::Override { tags, .. } => tags
                .iter()
                .position(|t| t.name == "move" && t.args.len() >= 4)
                .map(|t| (b, t)),
            _ => None,
        });
    let Some((b, t)) = found else {
        return times.iter().map(|_| text.to_string()).collect();
    };
    let Block::Override { tags, .. } = &line.blocks[b] else {
        unreachable!()
    };
    let m = &tags[t];
    let a = |i: usize| m.arg_f64(i).unwrap_or(0.0);
    let (x1, y1, x2, y2) = (a(0), a(1), a(2), a(3));
    let (mut t1, mut t2) = (a(4), a(5));
    if t1 <= 0.0 && t2 <= 0.0 {
        (t1, t2) = (0.0, (ev.end.0 - ev.start.0) as f64);
    }
    times
        .iter()
        .map(|&at| {
            let rel = (at - ev.start.0) as f64;
            let k = if rel <= t1 {
                0.0
            } else if rel >= t2 || t2 <= t1 {
                1.0
            } else {
                (rel - t1) / (t2 - t1)
            };
            if let Block::Override { tags, raw } = &mut line.blocks[b] {
                tags[t] = Tag::new(
                    "pos",
                    vec![num(x1 + (x2 - x1) * k), num(y1 + (y2 - y1) * k)],
                );
                *raw = None;
            }
            line.to_text()
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn doc() -> Document {
        let text = "[Script Info]\nPlayResX: 1920\nPlayResY: 1080\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        Document::parse(text)
    }

    fn ev(text: &str) -> Event {
        Event {
            start: fs_core::Time(1000),
            end: fs_core::Time(2000),
            style: "Default".into(),
            text: text.into(),
            ..Default::default()
        }
    }

    fn pos(s: &str) -> (f64, f64) {
        let t = Line::parse(s).first_tag("pos").cloned().expect("a \\pos");
        (t.arg_f64(0).unwrap(), t.arg_f64(1).unwrap())
    }

    #[test]
    fn words_keep_their_places() {
        let d = doc();
        let out = split_positioned(
            &d,
            &ev(r"{\pos(960,500)\b1}ab cd"),
            r"{\pos(960,500)\b1}ab cd",
            3,
        );
        assert_eq!(out.len(), 2);
        assert!(
            out[0].starts_with(r"{\an2\pos(") && out[0].ends_with(r"\b1}ab"),
            "{}",
            out[0]
        );
        let ((x0, y0), (x1, y1)) = (pos(&out[0]), pos(&out[1]));
        // centred on 960 together: the left word left of it, the right one right, on the same baseline
        assert!(x0 < 960.0 && x1 > 960.0 && x0 < x1, "{x0} {x1}");
        assert_eq!((y0, y1), (500.0, 500.0));
        // characters: four, spaces dropped
        assert_eq!(split_positioned(&d, &ev("ab cd"), "ab cd", 2).len(), 4);
    }

    #[test]
    fn wraps_stack_and_keep_inline_tags() {
        let d = doc();
        let text = r"{\an7\pos(100,100)}one{\i1}two\Nthree";
        let out = split_positioned(&d, &ev(text), text, 4);
        assert_eq!(out.len(), 2);
        assert!(out[0].ends_with(r"}one{\i1}two"), "{}", out[0]);
        let ((x0, y0), (x1, y1)) = (pos(&out[0]), pos(&out[1]));
        assert_eq!((x0, x1), (100.0, 100.0));
        assert!(y0 == 100.0 && y1 > 100.0);
        // the second wrap starts in italics, as the first ended
        assert!(out[1].contains(r"\i1}three"), "{}", out[1]);
        // one wrap: nothing to split
        assert!(split_positioned(&d, &ev("one"), "one", 4).is_empty());
    }

    #[test]
    fn moves_become_positions_per_frame() {
        let e = ev(r"{\move(0,0,100,50)}a");
        let out = frame_texts(&e, &e.text, &[1000, 1500, 2000]);
        assert_eq!(
            out,
            [r"{\pos(0,0)}a", r"{\pos(50,25)}a", r"{\pos(100,50)}a"]
        );
        let e = ev(r"{\move(0,0,100,0,200,400)}a");
        assert_eq!(frame_texts(&e, &e.text, &[1100, 1300])[0], r"{\pos(0,0)}a");
        assert_eq!(frame_texts(&e, &e.text, &[1100, 1300])[1], r"{\pos(50,0)}a");
        assert_eq!(frame_texts(&ev("b"), "b", &[1000]), ["b"]);
    }
}
