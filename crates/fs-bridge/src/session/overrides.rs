//! Override tags written by the visual tools, the all-tags tool and Hydra.

use fs_core::Document;
use fs_core::ops::{self, Rows};
use fs_core::tags::{self, Line, Tag};

use super::Session;
use crate::ffi::Visual;

impl Session {
    pub fn split_positioned(&self, row: usize, text: &str, how: u8) -> Vec<String> {
        let Some(r) = self.tl_row(row) else {
            return Vec::new();
        };
        let ev = self.ed.doc.event(r.main()).unwrap();
        crate::split::split_positioned(&self.ed.doc, ev, text, how)
    }

    pub fn visual(&self, row: usize) -> Visual {
        let Some(r) = self.tl_row(row) else {
            return Visual::default();
        };
        crate::visual::of_event(&self.ed.doc, self.ed.doc.event(r.main()).unwrap())
    }

    /// Set override tags (`\pos(1,2)\frz30`) on the first block of each row.
    pub fn set_tags(&mut self, rows: &[usize], tags_text: &str, amend: &str) {
        let evs = self.main_events(rows);
        let new = tags::parse_tags(tags_text);
        self.edit_amend(amend, "Visual typesetting", move |d| {
            for e in evs {
                put_line_tags(d, e, &new);
            }
        });
    }

    /// [`set_tags`](Self::set_tags) with each row's own tags (`tags_texts[i]` for `rows[i]`), in
    /// one edit: a drag of many lines is one change per mouse move, not one per line.
    pub fn set_each_tags(&mut self, rows: &[usize], tags_texts: &[String], amend: &str) {
        let jobs: Vec<(usize, Vec<Tag>)> = rows
            .iter()
            .zip(tags_texts)
            .filter_map(|(&r, t)| Some((self.tl_row(r)?.main(), tags::parse_tags(t))))
            .collect();
        self.edit_amend(amend, "Visual typesetting", move |d| {
            for (e, new) in &jobs {
                put_line_tags(d, *e, new);
            }
        });
    }

    /// Kainote's all-tags tool: rewrite `tag` in the rows from `base` (their
    /// texts when the drag started). `mode`: 0 add, 1 set, 2 multiply,
    /// 3/4 gradient over characters up/down, 5/6 gradient over lines up/down, 7 scale (every
    /// occurrence times `delta`).
    pub fn adjust_tag(
        &mut self,
        rows: &[usize],
        base: &[String],
        tags: &str,
        mode: u8,
        deltas: &[f64],
        amend: &str,
    ) {
        use fs_core::tag_adjust::{TagAdjust, adjust_tag};
        let mode = match mode {
            1 => TagAdjust::Set,
            2 => TagAdjust::Multiply,
            3 | 4 => TagAdjust::GradientText {
                increasing: mode == 3,
            },
            5 | 6 => TagAdjust::GradientLine {
                increasing: mode == 5,
            },
            7 => TagAdjust::Scale,
            _ => TagAdjust::Add,
        };
        let evs = self.main_events(rows);
        let base = base.to_vec();
        let changes: Vec<(String, f64)> = tags
            .split(',')
            .map(str::to_string)
            .zip(deltas.iter().copied())
            .collect();
        self.edit_amend(amend, "Change tags", move |d| {
            let n = evs.len().min(base.len());
            for (k, &e) in evs.iter().take(n).enumerate() {
                let style = d.style(&d.event(e).unwrap().style).cloned();
                let text = changes.iter().fold(base[k].clone(), |text, (tag, delta)| {
                    adjust_tag(&text, style.as_ref(), tag, mode, &[*delta], k, n)
                });
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    #[allow(clippy::too_many_arguments)] // the cxx bridge passes plain numbers, no tuples
    pub fn hydra(
        &mut self,
        rows: &[usize],
        active: usize,
        base: &[String],
        tag: &str,
        mode: u8,
        values: &[f64],
        from: &[f64],
        sel_from: i64,
        sel_to: i64,
        amend: &str,
    ) {
        use fs_core::tag_adjust::{TagAdjust, adjust_tag};
        let evs = self.main_events(rows);
        let active = self.main_events(&[active]).first().copied();
        let base = base.to_vec();
        let tag = tag.to_string();
        let values = values.to_vec();
        let delta: Vec<f64> = values.iter().zip(from).map(|(v, f)| v - f).collect();
        let from = from.to_vec();
        let at = (sel_from >= 0).then(|| (sel_from as usize, sel_to.max(sel_from) as usize));
        self.edit_amend(amend, "Change tags", move |d| {
            let n = evs.len().min(base.len());
            for (k, &e) in evs.iter().take(n).enumerate() {
                let ev = d.event(e).unwrap();
                let style = d.style(&ev.style).cloned();
                let mut text = base[k].clone();
                if tag == "pos" && mode != 7 {
                    // a line without \pos moves from where it is shown (Kainote)
                    let mut placed = ev.clone();
                    placed.text = text.clone();
                    let v = crate::visual::of_event(d, &placed);
                    text = ops::transform_place(&text, &|x, y| (x, y), false, (v.x, v.y));
                }
                let (how, by) = match mode {
                    1 => (TagAdjust::Set, values.clone()),
                    2 => (TagAdjust::Multiply, delta.clone()),
                    3 | 4 => (
                        TagAdjust::GradientText {
                            increasing: mode == 3,
                        },
                        delta.clone(),
                    ),
                    5 | 6 => (
                        TagAdjust::GradientLine {
                            increasing: mode == 5,
                        },
                        delta.clone(),
                    ),
                    7 => (
                        TagAdjust::Insert {
                            at: at.filter(|_| Some(e) == active),
                        },
                        values.clone(),
                    ),
                    8 => (
                        TagAdjust::Set,
                        from.iter()
                            .zip(&delta)
                            .map(|(f, x)| f + x * k as f64)
                            .collect(),
                    ),
                    _ => (TagAdjust::Add, delta.clone()),
                };
                let text = adjust_tag(&text, style.as_ref(), &tag, how, &by, k, n);
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    pub fn tag_values(&self, row: usize, tag: &str, count: usize) -> Vec<f64> {
        let Some(r) = self.tl_row(row) else {
            return Vec::new();
        };
        let d = &self.ed.doc;
        let Some(ev) = d.event(r.main()) else {
            return Vec::new();
        };
        if tag == "pos" {
            let v = crate::visual::of_event(d, ev);
            return vec![v.x, v.y];
        }
        fs_core::tag_adjust::line_values(&ev.text, d.style(&ev.style), tag, count)
    }

    #[allow(clippy::too_many_arguments)] // the cxx bridge passes plain numbers, no tuples
    pub fn transform_places(
        &mut self,
        rows: &[usize],
        cx: f64,
        cy: f64,
        sx: f64,
        sy: f64,
        turn: f64,
        amend: &str,
    ) {
        let evs = self.main_events(rows);
        let (s, c) = turn.to_radians().sin_cos();
        self.edit_amend(amend, "Change tags", move |d| {
            let f = move |x: f64, y: f64| {
                let (dx, dy) = ((x - cx) * sx, (y - cy) * sy);
                (cx + dx * c + dy * s, cy - dx * s + dy * c)
            };
            for e in evs {
                let ev = d.event(e).unwrap();
                let v = crate::visual::of_event(d, ev);
                let text = fs_core::ops::transform_place(&ev.text, &f, turn != 0.0, (v.x, v.y));
                if text != ev.text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    /// Remove tags by name (`pos,move`).
    pub fn remove_tags(&mut self, rows: &[usize], names: &str, amend: &str) {
        let evs = self.main_events(rows);
        let names: Vec<String> = names.split(',').map(|s| s.trim().to_string()).collect();
        self.edit_amend(amend, "Visual typesetting", move |d| {
            let n: Vec<&str> = names.iter().map(String::as_str).collect();
            for e in evs {
                let mut line = Line::parse(&d.event(e).unwrap().text);
                line.remove_tags(&n);
                let text = line.to_text();
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    /// Replace the line's `\p` drawing (adding `{\p1}` when it has none).
    pub fn set_drawing(&mut self, row: usize, drawing: &str, amend: &str) {
        let Some(r) = self.tl_row(row) else { return };
        let drawing = drawing.to_string();
        self.edit_amend(amend, "Vector drawing", move |d| {
            let e = r.main();
            let mut line = Line::parse(&d.event(e).unwrap().text);
            match line.blocks.iter_mut().find_map(|b| match b {
                tags::Block::Drawing { commands, .. } => Some(commands),
                _ => None,
            }) {
                Some(c) => *c = drawing,
                None => {
                    line.blocks.retain(|b| !matches!(b, tags::Block::Text(_)));
                    line.set_line_tag(Tag::simple("p", "1"));
                    line.blocks.push(tags::Block::Drawing {
                        scale: 1,
                        commands: drawing,
                    });
                }
            }
            d.event_mut(e).unwrap().text = line.to_text();
        });
    }

    /// `what` bits: 1 \pos, 2 \move start, 4 \move end, 8 clips, 16 drawings, 32 \org.
    /// With an `amend` key (a drag) `dx`, `dy` are the whole way dragged: every edit of the key
    /// shifts the lines from where its first one found them. Steps written one over another
    /// would add up the rounding of each.
    pub fn shift_positions(&mut self, rows: &[usize], dx: f64, dy: f64, what: u8, amend: &str) {
        let evs = self.main_events(rows);
        let from = match &self.shift_from {
            Some((key, from)) if key == amend && self.ed.would_amend(amend) => from.clone(),
            _ => {
                let doc = &self.ed.doc;
                let from: Vec<(usize, String)> = (evs.iter())
                    .filter_map(|&e| Some((e, doc.event(e)?.text.clone())))
                    .collect();
                self.shift_from = (!amend.is_empty()).then(|| (amend.to_owned(), from.clone()));
                from
            }
        };
        let what = ops::MoveTargets {
            pos: what & 1 != 0,
            move_start: what & 2 != 0,
            move_end: what & 4 != 0,
            clip: what & 8 != 0,
            drawing: what & 16 != 0,
            org: what & 32 != 0,
        };
        self.edit_amend(amend, "Shift positions", move |d| {
            for (e, text) in from {
                if let Some(ev) = d.event_mut(e) {
                    ev.text = text;
                }
            }
            let rest = ops::MoveTargets {
                drawing: false,
                ..what
            };
            ops::translate_positions(d, &Rows::Selected(evs.clone()), dx, dy, rest);
            if !what.drawing {
                return;
            }
            // drawing points are in the line's units: the screen step unturned by \frz, unscaled
            let only = ops::MoveTargets {
                pos: false,
                move_start: false,
                move_end: false,
                clip: false,
                drawing: true,
                org: false,
            };
            for e in evs {
                let v = crate::visual::of_event(d, d.event(e).unwrap());
                if v.fscx == 0.0 || v.fscy == 0.0 {
                    continue; // nothing is shown
                }
                let (s, c) = v.frz.to_radians().sin_cos();
                let (x, y) = (dx * c - dy * s, dx * s + dy * c);
                let (x, y) = (x * 100.0 / v.fscx, y * 100.0 / v.fscy);
                ops::translate_positions(d, &Rows::Selected(vec![e]), x, y, only);
            }
        });
    }
}

/// Set line tags (\pos, \clip…) in the event's first block, leaving an unchanged text alone.
fn put_line_tags(d: &mut Document, e: usize, new: &[Tag]) {
    let mut line = Line::parse(&d.event(e).unwrap().text);
    for t in new {
        line.set_line_tag(t.clone());
    }
    let text = line.to_text();
    if d.event(e).unwrap().text != text {
        d.event_mut(e).unwrap().text = text;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn scaling_all_tags_with_places_is_one_step_from_the_base() {
        let mut s = session(ASS);
        for (i, t) in [r"{\pos(100,100)}one", r"{\pos(200,100)\fscx50}two"]
            .into_iter()
            .enumerate()
        {
            let mut r = s.row(i);
            r.text = t.into();
            s.set_row(i, &r, "");
        }
        let base = vec![s.row(0).text, s.row(1).text];
        for f in [2.0, 3.0] {
            s.adjust_tag(&[0, 1], &base, "fscx,fscy", 7, &[f, f], "drag");
            s.transform_places(&[0, 1], 100.0, 100.0, f, f, 0.0, "drag");
        }
        assert_eq!(s.row(0).text, r"{\pos(100,100)\fscx300\fscy300}one");
        assert_eq!(s.row(1).text, r"{\pos(400,100)\fscx150\fscy300}two");
        s.undo();
        assert_eq!(
            [s.row(0).text, s.row(1).text],
            [base[0].clone(), base[1].clone()]
        );
    }

    #[test]
    fn shifted_drawings_follow_the_screen_step() {
        let mut s = session(ASS);
        let text = r"{\pos(0,0)\fscx200\frz90\p1}m 0 0 l 10 0 10 10";
        s.set_texts(&[0], &[text.to_string()], "edit");
        s.shift_positions(&[0], 10.0, 0.0, 16 | 1, "");
        // right on screen is down in a drawing turned by 90°; \pos moves as asked
        assert_eq!(
            s.row(0).text,
            r"{\pos(10,0)\fscx200\frz90\p1}m 0 10 l 10 10 10 20"
        );
        // a drag: every step from the line as it was, in one undo step
        let start = [r"{\pos(0.5,0)\p1}m 0 0 l 3 0".to_string()];
        s.set_texts(&[0], &start, "edit");
        for k in 0..=30 {
            s.shift_positions(&[0], f64::from(k) / 3.0, 0.0, 16 | 1, "drag-1");
        }
        assert_eq!(s.row(0).text, r"{\pos(10.5,0)\p1}m 10 0 l 13 0");
        // the next drag starts from there
        s.shift_positions(&[0], 1.0, 2.0, 1, "drag-2");
        s.shift_positions(&[0], 2.0, 4.0, 1, "drag-2");
        assert_eq!(s.row(0).text, r"{\pos(12.5,4)\p1}m 10 0 l 13 0");
        assert!(s.undo() && s.undo());
        assert_eq!(s.row(0).text, start[0]);
    }

    #[test]
    fn each_row_gets_its_own_tags_in_one_step() {
        let mut s = session(ASS);
        let t = [r"\pos(1,2)".to_string(), r"\pos(3,4)".to_string()];
        s.set_each_tags(&[2, 0], &t, "drag-1");
        s.set_each_tags(&[2, 0], &t, "drag-1");
        assert_eq!(s.row(2).text, r"{\pos(1,2)}three");
        assert_eq!(s.row(0).text, r"{\pos(3,4)}one");
        assert!(s.undo());
        assert_eq!(s.row(0).text, "one");
    }

    #[test]
    fn hydra_inserts_at_the_cursor_and_steps_from_the_active_line() {
        let mut s = session(ASS);
        let base: Vec<String> = (0..3).map(|i| s.row(i).text).collect();
        // insert: at the active line's cursor, the others' starts
        s.hydra(
            &[0, 1, 2],
            1,
            &base,
            "blur",
            7,
            &[2.0],
            &[0.0],
            1,
            1,
            "drag",
        );
        let texts = |s: &Session| (0..3).map(|i| s.row(i).text).collect::<Vec<_>>();
        assert_eq!(
            texts(&s),
            [r"{\blur2}one", r"t{\blur2}wo", r"{\blur2}three"]
        );
        // step: the active line's value + n·Δ, one undo step with the insert's amend
        s.hydra(
            &[0, 1, 2],
            1,
            &base,
            "bord",
            8,
            &[3.0],
            &[2.0],
            -1,
            -1,
            "drag",
        );
        assert_eq!(
            texts(&s),
            [r"{\bord2}one", r"{\bord3}two", r"{\bord4}three"]
        );
        // a line without \pos moves from where it is shown: the style's bottom centre
        s.hydra(
            &[0],
            0,
            &base[..1],
            "pos",
            0,
            &[970.0, 1060.0],
            &[960.0, 1070.0],
            -1,
            -1,
            "pos",
        );
        assert_eq!(s.row(0).text, r"{\pos(970,1060)}one");
        assert_eq!(s.tag_values(0, "pos", 2), [970.0, 1060.0]);
        assert_eq!(s.tag_values(1, "3c", 1), [0.0, 0.0, 0.0]);
    }

    #[test]
    fn adjust_tag_rewrites_from_base_in_one_step() {
        let mut s = session(ASS);
        let base = vec![s.row(0).text];
        s.adjust_tag(&[0], &base, "bord", 1, &[3.0], "drag");
        s.adjust_tag(&[0], &base, "bord", 1, &[5.0], "drag");
        assert!(s.row(0).text.starts_with(r"{\bord5}"));
        s.undo();
        assert_eq!(s.row(0).text, base[0]);
    }
}
