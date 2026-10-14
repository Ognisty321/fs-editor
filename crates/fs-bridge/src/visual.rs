//! Read the values the visual tools edit from a line (`\pos`, `\move`,
//! `\org`, `\clip`, `\p`, scale and rotation).

use fs_core::tags::{Block, Line};
use fs_core::{Document, Event};

use crate::ffi::Visual;

/// Legacy `\a` (1-3 bottom, +4 top, +8 middle) to numpad `\an`.
fn legacy_align(a: i32) -> i32 {
    let h = (a - 1) % 4 + 1;
    match a {
        5..=7 => h + 6,
        9..=11 => h + 3,
        _ => h,
    }
}

/// Position of a line without `\pos`/`\move`, as Aegisub computes it.
fn default_pos(doc: &Document, ev: &Event, align: i32) -> (f64, f64) {
    let (sw, sh) = doc.play_res();
    let style = doc.style(&ev.style);
    let m = |line: i32, style_m: Option<i32>| {
        if line != 0 {
            line
        } else {
            style_m.unwrap_or(0)
        }
    };
    let ml = m(ev.margin_l, style.map(|s| s.margin_l));
    let mr = m(ev.margin_r, style.map(|s| s.margin_r));
    let mv = m(ev.margin_v, style.map(|s| s.margin_v));
    let x = match align % 3 {
        1 => ml,
        0 => sw - mr,
        _ => (ml + sw - mr) / 2,
    };
    let y = match align {
        7..=9 => mv,
        1..=3 => sh - mv,
        _ => sh / 2,
    };
    (x as f64, y as f64)
}

pub fn of_event(doc: &Document, ev: &Event) -> Visual {
    let line = Line::parse(&ev.text);
    let style = doc.style(&ev.style);
    let f = |name: &str, i: usize| line.first_tag(name).and_then(|t| t.arg_f64(i));
    let align = f("an", 0)
        .map(|a| a as i32)
        .or_else(|| f("a", 0).map(|a| legacy_align(a as i32)))
        .filter(|a| (1..=9).contains(a))
        .unwrap_or_else(|| style.map_or(2, |s| s.alignment as i32));

    let mut v = Visual {
        align,
        fscx: f("fscx", 0).unwrap_or_else(|| style.map_or(100.0, |s| s.scale_x)),
        fscy: f("fscy", 0).unwrap_or_else(|| style.map_or(100.0, |s| s.scale_y)),
        frz: f("frz", 0)
            .or_else(|| f("fr", 0))
            .unwrap_or_else(|| style.map_or(0.0, |s| s.angle)),
        frx: f("frx", 0).unwrap_or(0.0),
        fry: f("fry", 0).unwrap_or(0.0),
        ..Default::default()
    };

    if let Some(t) = line.first_tag("pos") {
        v.has_pos = true;
        v.x = t.arg_f64(0).unwrap_or(0.0);
        v.y = t.arg_f64(1).unwrap_or(0.0);
    } else if let Some(t) = line.first_tag("move")
        && t.args.len() >= 4
    {
        v.has_move = true;
        v.x = t.arg_f64(0).unwrap_or(0.0);
        v.y = t.arg_f64(1).unwrap_or(0.0);
        v.x2 = t.arg_f64(2).unwrap_or(0.0);
        v.y2 = t.arg_f64(3).unwrap_or(0.0);
        v.t1 = t.arg_f64(4).unwrap_or(0.0) as i64;
        v.t2 = t.arg_f64(5).unwrap_or(0.0) as i64;
    } else {
        (v.x, v.y) = default_pos(doc, ev, align);
    }
    if !v.has_move {
        (v.x2, v.y2) = (v.x, v.y);
    }

    match line.first_tag("org") {
        Some(t) => {
            v.has_org = true;
            v.org_x = t.arg_f64(0).unwrap_or(v.x);
            v.org_y = t.arg_f64(1).unwrap_or(v.y);
        }
        None => (v.org_x, v.org_y) = (v.x, v.y),
    }

    let clip = line.first_tag("clip").or_else(|| line.first_tag("iclip"));
    if let Some(t) = clip {
        v.inverse = t.name == "iclip";
        if t.args.len() == 4 {
            v.clip = 1;
            v.clip_x1 = t.arg_f64(0).unwrap_or(0.0);
            v.clip_y1 = t.arg_f64(1).unwrap_or(0.0);
            v.clip_x2 = t.arg_f64(2).unwrap_or(0.0);
            v.clip_y2 = t.arg_f64(3).unwrap_or(0.0);
        } else if let Some(last) = t.args.last() {
            v.clip = 2;
            v.clip_scale = if t.args.len() == 2 {
                t.arg_f64(0).unwrap_or(1.0) as i32
            } else {
                1
            };
            v.clip_drawing = last.trim().to_string();
        }
    }

    if let Some((scale, cmds)) = line.blocks.iter().find_map(|b| match b {
        Block::Drawing { scale, commands } => Some((*scale, commands)),
        _ => None,
    }) {
        v.drawing = cmds.trim().to_string();
        v.drawing_scale = scale as i32;
    }
    v
}

#[cfg(test)]
mod tests {
    #[test]
    fn legacy() {
        assert_eq!(
            [1, 2, 3, 5, 6, 7, 9, 10, 11].map(super::legacy_align),
            [1, 2, 3, 7, 8, 9, 4, 5, 6]
        );
    }

    #[test]
    fn drawing_and_clip() {
        let mut doc = fs_core::Document::new();
        let text = r"{\an7\pos(40,40)\clip(m 1 2 l 3 4)\p1\c&H3060FF&}m 0 0 l 80 0 80 50 b 60 70 20 70 0 50{\p0}";
        doc.event_mut(0).unwrap().text = text.into();
        let v = super::of_event(&doc, doc.event(0).unwrap());
        assert_eq!(v.drawing, "m 0 0 l 80 0 80 50 b 60 70 20 70 0 50");
        assert_eq!((v.align, v.x, v.y, v.drawing_scale), (7, 40.0, 40.0, 1));
        assert_eq!((v.fscx, v.fscy), (100.0, 100.0));
        assert_eq!((v.clip, v.clip_drawing.as_str()), (2, "m 1 2 l 3 4"));
    }
}
