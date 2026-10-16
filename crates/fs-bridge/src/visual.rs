//! Read the values the visual tools edit from a line (`\pos`, `\move`,
//! `\org`, `\clip`, `\p`, scale and rotation).

use fs_core::tags::{Block, Line};
use fs_core::{Document, Event};

use crate::ffi::Visual;

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

/// See the bridge's `fit_anchor`.
pub fn fit_anchor(b: &[f64], r: &[f64], align: u8) -> [f64; 2] {
    if b.len() < 4 || r.len() < 4 {
        return [0.0, 0.0];
    }
    let an = align as i32 + 1;
    // the box's left (1), centre (2) or right (0) on the rectangle's
    let x = |k: i32| match k {
        1 => r[0] - b[0],
        0 => r[2] - b[2],
        _ => (r[0] + r[2] - b[0] - b[2]) / 2.0,
    };
    // its top (0), middle (1) or bottom (2) on the rectangle's
    let y = |k: i32| match k {
        0 => r[1] - b[1],
        1 => (r[1] + r[3] - b[1] - b[3]) / 2.0,
        _ => r[3] - b[3],
    };
    match an {
        1..=9 => [x(an % 3), y(2 - (an - 1) / 3)],
        10..=12 => [x(an % 3), r[3] - b[1]], // below: its top on the rectangle's bottom
        13..=15 => [x(an % 3), r[1] - b[3]], // above
        16..=18 => [r[0] - b[2], y(an - 16)], // before: its right on the rectangle's left
        _ => [r[2] - b[0], y(an - 19)],      // after
    }
}

/// libass's 3D transform of a line (`calc_transform_matrix`) as a 3×4 projective matrix, row
/// after row: a point (x, y, z) of the line as laid out, unturned and unsheared, in script pixels
/// goes to `(r0·p / r2·p, r1·p / r2·p)` on the screen, p = (x, y, z, 1). `rot`: \frx, \fry,
/// \frz around `org`; `shear`: \fax and \fay from `shear_at`; `dist`: the camera's distance
/// in script pixels: libass's 20000 in 26.6 fixed point, 312.5 storage pixels
/// (`312.5 · PlayResY / storage height`), Aegisub's too.
pub fn projection(
    org: [f64; 2],
    rot: [f64; 3],
    shear: [f64; 2],
    shear_at: [f64; 2],
    dist: f64,
) -> [f64; 12] {
    type V = [f64; 4];
    let lin =
        |a: V, ka: f64, b: V, kb: f64| -> V { std::array::from_fn(|i| a[i] * ka + b[i] * kb) };
    let [frx, fry, frz] = rot.map(f64::to_radians);
    let [fax, fay] = shear;
    // from the turning point, sheared
    let x1: V = [1.0, fax, 0.0, -org[0] - fax * shear_at[1]];
    let y1: V = [fay, 1.0, 0.0, -org[1] - fay * shear_at[0]];
    let z1: V = [0.0, 0.0, 1.0, 0.0];
    // z, then x, then y, with libass's signs
    let (sz, cz) = frz.sin_cos();
    let (x2, y2) = (lin(x1, cz, y1, sz), lin(x1, -sz, y1, cz));
    let (sx, cx) = ((-frx).sin(), frx.cos());
    let (y3, z3) = (lin(y2, cx, z1, -sx), lin(y2, sx, z1, cx));
    let (sy, cy) = fry.sin_cos();
    let (x4, z4) = (lin(x2, cy, z3, -sy), lin(x2, sy, z3, cy));
    let mut w = z4;
    w[3] += dist;
    let (hx, hy) = (lin(w, org[0], x4, dist), lin(w, org[1], y3, dist));
    let mut m = [0.0; 12];
    m[..4].copy_from_slice(&hx);
    m[4..8].copy_from_slice(&hy);
    m[8..].copy_from_slice(&w);
    m
}

/// The line's projection (see `projection`) for a drawing whose (0,0) libass puts at `at`, the
/// video (libass's storage) `storage_height` high. libass shears in the scaled glyph, so
/// \fax counts fscx/fscy times and \fay fscy/fscx.
pub fn line_projection(v: &Visual, at: [f64; 2], storage_height: f64) -> [f64; 12] {
    let ratio = if v.fscx > 0.0 && v.fscy > 0.0 {
        v.fscx / v.fscy
    } else {
        1.0
    };
    projection(
        [v.org_x, v.org_y],
        [v.frx, v.fry, v.frz],
        [v.fax * ratio, v.fay / ratio],
        at,
        312.5 * v.play_res_y / storage_height.max(1.0),
    )
}

pub fn of_event(doc: &Document, ev: &Event) -> Visual {
    let line = Line::parse(&ev.text);
    let style = doc.style(&ev.style);
    let f = |name: &str, i: usize| line.first_tag(name).and_then(|t| t.arg_f64(i));
    let align = f("an", 0)
        .map(|a| a as i32)
        .or_else(|| {
            f("a", 0).and_then(|a| fs_core::model::legacy_tag_align(a as i32).map(i32::from))
        })
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
        fax: f("fax", 0).unwrap_or(0.0),
        fay: f("fay", 0).unwrap_or(0.0),
        play_res_y: f64::from(doc.play_res().1),
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
    fn a_box_fits_a_rectangle_each_of_the_21_ways() {
        let (b, r) = ([-50.0, -20.0, 50.0, 20.0], [100.0, 100.0, 300.0, 200.0]);
        let at = |an: u8| super::fit_anchor(&b, &r, an - 1);
        assert_eq!(at(1), [150.0, 180.0]);
        assert_eq!(at(5), [200.0, 150.0]);
        assert_eq!(at(7), [150.0, 120.0]);
        assert_eq!(at(9), [250.0, 120.0]);
        assert_eq!(at(11), [200.0, 220.0]); // centre below
        assert_eq!(at(13), [150.0, 80.0]); // left above
        assert_eq!(at(16), [50.0, 120.0]); // before, top
        assert_eq!(at(17), [50.0, 150.0]);
        assert_eq!(at(21), [350.0, 180.0]); // after, bottom
    }

    /// The projection against what libass draws: the corners of a turned, sheared box projected
    /// bound what libass covers, at two frame sizes (the camera is 312.5 storage pixels away).
    /// \fax and \fay shear from the drawing's (0,0), \fax after the scale.
    #[test]
    fn the_projection_is_libass_s() {
        let mut r = fs_media::SubtitleRenderer::new().unwrap();
        // (tags, the drawing's (0,0) where libass puts it, the box's corners from it)
        // (tags, where libass puts the drawing's (0,0), the scale)
        let cases = [
            (r"\an7\pos(500,400)\frx40", [500.0, 400.0], [1.0, 1.0]),
            (r"\an7\pos(500,400)\fry-40", [500.0, 400.0], [1.0, 1.0]),
            (
                r"\an7\pos(500,400)\org(600,450)\frx30\fry-25\frz10",
                [500.0, 400.0],
                [1.0, 1.0],
            ),
            (
                r"\an7\pos(500,400)\frx-40\fry35",
                [500.0, 400.0],
                [1.0, 1.0],
            ),
            (
                r"\an7\pos(500,400)\fax0.3\fay0.2\frz15",
                [500.0, 400.0],
                [1.0, 1.0],
            ),
            (
                r"\an7\pos(500,400)\fax-0.4\frx20\fry20",
                [500.0, 400.0],
                [1.0, 1.0],
            ),
            // the drawing's (0,0) moved by the alignment's share of its box
            (
                r"\an5\pos(500,400)\fax0.3\frx25",
                [400.0, 350.0],
                [1.0, 1.0],
            ),
            (
                r"\an3\pos(700,600)\fay0.2\fry30",
                [500.0, 500.0],
                [1.0, 1.0],
            ),
            (
                r"\an7\pos(500,400)\fscx150\fscy50\fax0.3\fay0.1\frz20",
                [500.0, 400.0],
                [1.5, 0.5],
            ),
        ];
        for (tags, at, k2) in cases {
            for h in [1080usize, 540] {
                let ass = format!(
                    "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n\n\
                     [V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, \
                     OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, \
                     Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, \
                     MarginV, Encoding\nStyle: D,Arial,20,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,\
                     0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1\n\n[Events]\nFormat: Layer, Start, End, \
                     Style, Name, MarginL, MarginR, MarginV, Effect, Text\n\
                     Dialogue: 0,0:00:00.00,0:00:05.00,D,,0,0,0,,{{{tags}\\p1}}m 20 10 l 220 10 220 110 20 110\n"
                );
                r.set_script(&ass).unwrap();
                let b = r.bounds(1000, h * 16 / 9, h).unwrap().unwrap();
                let k = h as f64 / 1080.0;
                let doc = fs_core::Document::parse(&ass);
                let v = super::of_event(&doc, doc.event(0).unwrap());
                let m = super::line_projection(&v, at, h as f64);
                let (mut lo, mut hi) = ([f64::MAX; 2], [f64::MIN; 2]);
                for (x, y) in [(20.0, 10.0), (220.0, 10.0), (220.0, 110.0), (20.0, 110.0)] {
                    let p = [at[0] + x * k2[0], at[1] + y * k2[1], 0.0, 1.0];
                    let dot = |r: &[f64]| r.iter().zip(p).map(|(a, b)| a * b).sum::<f64>();
                    let w = dot(&m[8..]);
                    let s = [dot(&m[..4]) / w * k, dot(&m[4..8]) / w * k];
                    for i in 0..2 {
                        lo[i] = lo[i].min(s[i]);
                        hi[i] = hi[i].max(s[i]);
                    }
                }
                let want = [lo[0], lo[1], hi[0], hi[1]];
                let got = [b[4], b[5], b[6], b[7]].map(f64::from);
                assert!(
                    want.iter().zip(got).all(|(a, b)| (a - b).abs() <= 2.0),
                    "{tags} at {h}: projected {want:?}, libass {got:?}"
                );
            }
        }
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
