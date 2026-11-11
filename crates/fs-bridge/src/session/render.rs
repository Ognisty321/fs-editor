//! The script as libass takes it: all of it, one frame's lines, or one line to measure.

use fs_core::ass::SectionKind;
use fs_core::ops;
use fs_core::tags::Line;
use fs_core::tlmode;
use fs_core::{Document, Time};

use super::Session;

/// The text libass gets: without the uuencoded attachments, whose fonts the renderer
/// registers once (`Renderer::load_subs`) instead of decoding them on every edit.
/// What `Session::render_subs` hands to the render thread.
pub struct RenderSubs {
    pub(crate) doc: Document,
    tl_only: bool,
    at_ms: i64,
}

impl RenderSubs {
    /// The script for libass. `at_ms >= 0` keeps only the lines shown at that
    /// time: Aegisub sends just those after an edit, and the whole file once
    /// another frame is wanted.
    pub(crate) fn script(&self) -> String {
        let mut doc = if self.tl_only {
            tlmode::translation_only(&self.doc)
        } else {
            self.doc.clone() // sections are shared, only the kept lines are copied
        };
        if self.at_ms >= 0 {
            // libass reads centiseconds: keep lines within 10 ms of the frame time
            let (t, slack) = (Time(self.at_ms), Time(10));
            doc.event_entries_mut().retain(|e| {
                let e = &e.event;
                !e.is_comment() && e.start <= t + slack && e.end + slack > t
            });
        }
        render_script(doc)
    }
}

impl Session {
    /// The script libass renders: in TL mode with "hide original" only the
    /// translations are shown.
    /// The subtitles for the video, taken in O(1) (the sections are shared); the render
    /// thread turns them into libass's script, so an edit never waits for a frame.
    pub fn render_subs(&self, hide_original: bool, at_ms: i64) -> Box<RenderSubs> {
        Box::new(RenderSubs {
            doc: self.ed.doc.clone(),
            tl_only: self.tl && hide_original,
            at_ms,
        })
    }

    /// The row's line alone for libass to measure: moved by the script's size into the middle of
    /// a script three times as large, so nothing of it is cut at the screen's edges, without its
    /// clips and wrapping as wide as before. Returns the script, the time in it to render, its
    /// size, where the line's anchor is in it at that time and where in the real script.
    #[allow(clippy::type_complexity)] // one caller, which takes it apart at once
    pub(crate) fn measure_script(
        &self,
        row: usize,
        at_ms: i64,
    ) -> Option<(String, i64, (i32, i32), (f64, f64), (f64, f64))> {
        let e = *self.main_events(&[row]).first()?;
        let mut doc = self.ed.doc.clone();
        let ev = doc.event(e)?.clone();
        let (w, h) = doc.play_res();
        let v = crate::visual::of_event(&doc, &ev);
        let dur = (ev.end.0 - ev.start.0).max(1);
        let at = (at_ms - ev.start.0).clamp(0, dur - 1);
        let (mut x, mut y) = (v.x, v.y);
        if v.has_move {
            // libass: no times (or none past 0) is the whole line
            let (t1, t2) = if v.t1 <= 0 && v.t2 <= 0 {
                (0, dur)
            } else {
                (v.t1, v.t2)
            };
            let u = if at <= t1 {
                0.0
            } else if at >= t2 {
                1.0
            } else {
                (at - t1) as f64 / (t2 - t1) as f64
            };
            x += (v.x2 - v.x) * u;
            y += (v.y2 - v.y) * u;
        }
        let (dx, dy) = (w as f64, h as f64);
        let mut line = Line::parse(&ops::transform_place(
            &ev.text,
            &|a, b| (a + dx, b + dy),
            false,
            (v.x, v.y),
        ));
        line.remove_tags(&["clip", "iclip"]);
        let style = doc.style(&ev.style);
        let eff = |m: i32, s: Option<i32>| if m != 0 { m } else { s.unwrap_or(0) };
        let mut one = ev.clone();
        one.text = line.to_text();
        one.kind = fs_core::EventKind::Dialogue;
        one.margin_l = eff(ev.margin_l, style.map(|s| s.margin_l)) + w;
        one.margin_r = eff(ev.margin_r, style.map(|s| s.margin_r)) + w;
        (one.start, one.end) = (Time(0), Time(dur));
        doc.retain_events(|_| false);
        let entry = doc.new_entry(one);
        doc.event_entries_mut().push(entry);
        for (k, n) in [("PlayResX", w), ("PlayResY", h)] {
            doc.set_info(k, &(3 * n).to_string());
        }
        for k in ["LayoutResX", "LayoutResY"] {
            if let Some(n) = doc.info(k).and_then(|v| v.trim().parse::<i32>().ok()) {
                doc.set_info(k, &(3 * n).to_string());
            }
        }
        Some((
            render_script(doc),
            at,
            (3 * w, 3 * h),
            (x + dx, y + dy),
            (x, y),
        ))
    }

    pub fn script_for_render(&self, hide_original: bool, at_ms: i64) -> String {
        self.render_subs(hide_original, at_ms).script()
    }

    pub fn frame_texts(&self, row: usize, text: &str, times: &[i64]) -> Vec<String> {
        let Some(r) = self.tl_row(row) else {
            return Vec::new();
        };
        crate::split::frame_texts(self.ed.doc.event(r.main()).unwrap(), text, times)
    }
}

pub(crate) fn render_script(mut doc: Document) -> String {
    (doc.sections).retain(|s| !matches!(s.kind, SectionKind::Fonts | SectionKind::Graphics));
    doc.to_ass_string()
}

#[cfg(test)]
mod tests {
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn a_line_is_measured_whole_past_the_screen_edges() {
        let mut s = session(ASS);
        for (i, t) in [
            r"{\an7\pos(-50,10)\bord3\shad4\p1}m 0 0 l 100 0 100 40 0 40",
            r"{\an5\move(0,0,200,0)\bord0\clip(0,0,1,1)\p1}m 0 0 l 100 0 100 40 0 40",
        ]
        .into_iter()
        .enumerate()
        {
            let mut r = s.row(i);
            r.text = t.into();
            s.set_row(i, &r, "");
        }
        let mut r = crate::media::Renderer::new().unwrap();
        // within the pixel libass's antialiasing may add
        let near = |a: Vec<f64>, b: [f64; 10]| {
            a.len() == 10 && a.iter().zip(b).all(|(x, y)| (x - y).abs() <= 1.0)
        };
        // past the left edge, the shadow left out, the border around the text alone
        assert!(near(
            r.line_box(&s, 0, 1500),
            [-3.0, -3.0, 103.0, 43.0, 0.0, 0.0, 100.0, 40.0, -50.0, 10.0]
        ));
        // centred on wherever the move has taken it, the clip left out
        assert!(near(
            r.line_box(&s, 1, 3500),
            [
                -50.0, -20.0, 50.0, 20.0, -50.0, -20.0, 50.0, 20.0, 100.0, 0.0
            ]
        ));
    }

    #[test]
    fn render_script_of_one_frame_keeps_its_lines() {
        let s = session(ASS);
        let text = s.script_for_render(false, 3500);
        assert!(text.contains(",,two") && !text.contains(",,one") && !text.contains(",,three"));
        assert!(text.contains("Style: Default"));
        // the end of a line (centisecond-rounded) still counts within 10 ms
        assert!(s.script_for_render(false, 1995).contains(",,one"));
        assert_eq!(s.script_for_render(false, -1), s.ed.doc.to_ass_string());
        // embedded fonts go to libass once, not in every script it parses
        let mut s = session(ASS);
        s.ed.doc.attach(true, "F.ttf", &[7; 300]);
        s.ed.doc.attach(false, "p.png", b"png");
        for at in [-1, 3500] {
            let text = s.script_for_render(false, at);
            assert!(
                !text.contains("[Fonts]")
                    && !text.contains("[Graphics]")
                    && text.contains("[Events]")
            );
        }
    }
}
