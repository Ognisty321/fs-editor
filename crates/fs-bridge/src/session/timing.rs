//! Timing of the rows: Shift Times, continuous lines, snapping to the video, the timing
//! post-processor and retiming between frame rates.

use std::collections::HashMap;

use fs_core::ops::{self, Rows, ShiftAmount, ShiftWhich};
use fs_core::tags::{self, Line};
use fs_core::vfr::Framerate;
use fs_core::{Document, Event, Time};

use super::{Session, sync_originals};
use crate::ffi::{PostSpec, ShiftSpec};

impl Session {
    /// Retime the whole script between frame rates (Aegisub "Transform
    /// Framerate"); `video` as a rate stands for the video's timecodes.
    pub fn change_fps(&mut self, from: &str, to: &str, video: &[i64]) -> Result<(), String> {
        let rate = |s: &str| {
            if s == "video" {
                Framerate::from_timecodes(video.to_vec())
            } else {
                Framerate::parse(s)
            }
            .map_err(|e| format!("{s}: {e}"))
        };
        let (from, to) = (rate(from)?, rate(to)?);
        self.edit("Change frame rate", |d| ops::change_fps(d, &from, &to));
        Ok(())
    }

    /// Kainote's "Set FPS from video": every time of the script stretched about `anchor`, so
    /// that `from` lands on `to` (subtitles timed to another frame rate).
    pub fn stretch_times(&mut self, anchor: i64, from: i64, to: i64) {
        if from == anchor {
            return;
        }
        let k = (to - anchor) as f64 / (from - anchor) as f64;
        let at = |t: Time| {
            Time(
                (anchor as f64 + (t.ms() - anchor) as f64 * k)
                    .round()
                    .max(0.0) as i64,
            )
        };
        self.edit("Set FPS from video", |d| {
            for i in 0..d.events().len() {
                let ev = d.event_mut(i).unwrap();
                (ev.start, ev.end) = (at(ev.start), at(ev.end));
            }
        });
    }

    /// Aegisub's "Change End" / "Change Start": a row ends at the next row's start (or starts
    /// at the previous row's end) when that row is selected too; a single row uses its neighbour.
    pub fn make_continuous(&mut self, rows: &[usize], next: bool) {
        let mut rows = rows.to_vec();
        rows.sort_unstable();
        rows.dedup();
        if let [r] = rows[..] {
            rows = if next {
                vec![r, r + 1]
            } else {
                r.checked_sub(1).into_iter().chain([r]).collect()
            };
        }
        let pairs: Vec<(usize, usize)> = rows
            .windows(2)
            .filter(|w| w[1] == w[0] + 1)
            .filter_map(|w| Some((self.tl_row(w[0])?.main(), self.tl_row(w[1])?.main())))
            .collect();
        let tl = self.tl_pairs(&rows);
        if !pairs.is_empty() {
            self.edit("Continuous times", |d| {
                ops::make_continuous(d, &pairs, next);
                sync_originals(d, &tl);
            });
        }
    }

    /// Rows picked by the Shift Times "Which lines" choice (0 all, 1 selected,
    /// 2 from the first selected, 3/4 starting at or after/before the first
    /// selected line (Kainote), 5 the given styles).
    pub fn spec_rows(
        &self,
        lines: u8,
        selected: &[usize],
        active: usize,
        styles: &[String],
    ) -> Vec<usize> {
        let n = self.rows.len();
        let start_of = |r: usize| self.row(r).start;
        let first = selected.iter().copied().min();
        match lines {
            1 => selected.to_vec(),
            2 => (first.unwrap_or(active)..n).collect(),
            3 => {
                let t = start_of(first.unwrap_or(active));
                (0..n).filter(|&r| start_of(r) >= t).collect()
            }
            4 => {
                let t = start_of(first.unwrap_or(active));
                (0..n).filter(|&r| start_of(r) <= t).collect()
            }
            5 => (0..n)
                .filter(|&r| styles.contains(&self.row(r).style))
                .collect(),
            _ => (0..n).collect(),
        }
    }

    /// Shift Times panel. `timecodes` (from the video) are needed for frames.
    pub fn shift(&mut self, spec: &ShiftSpec, timecodes: &[i64]) -> usize {
        let rows = self.spec_rows(spec.lines, &spec.selected, spec.active, &spec.styles);
        let evs = self.all_events(&rows);
        // the end fixes compare lines: the rows' own, so a pair is not one line and the next
        let (mains, tl) = (self.main_events(&rows), self.tl_pairs(&rows));
        let which = match spec.which {
            1 => ShiftWhich::Start,
            2 => ShiftWhich::End,
            _ => ShiftWhich::Both,
        };
        let fr = if spec.frames {
            match Framerate::from_timecodes(timecodes.to_vec())
                .ok()
                .or_else(|| self.framerate.clone())
            {
                Some(f) => Some(f),
                None => return 0,
            }
        } else {
            None
        };
        let spec = spec.clone();
        self.edit("Shift times", move |d| {
            let times = |d: &Document, e: usize| d.event(e).map(|ev| (ev.start, ev.end));
            let old: Vec<_> = mains.iter().map(|&e| times(d, e)).collect();
            let before: Vec<i64> = evs
                .iter()
                .map(|&e| d.event(e).unwrap().start.ms())
                .collect();
            let amount = match &fr {
                Some(f) => ShiftAmount::Frames(spec.amount, f),
                None => ShiftAmount::Time(Time(spec.amount)),
            };
            ops::shift_times(d, &Rows::Selected(evs.clone()), amount, which, None);
            if spec.tag_times && which == ShiftWhich::Start {
                // the tags' times keep their place on the video
                for (k, &e) in evs.iter().enumerate() {
                    let delta = d.event(e).unwrap().start.ms() - before[k];
                    shift_tag_times(d.event_mut(e).unwrap(), -delta);
                }
            }
            match spec.end_fix {
                1 => fix_overlaps(d, &mains),
                2 => {
                    new_end_times(d, &mains);
                    fix_overlaps(d, &mains);
                }
                _ => {}
            }
            sync_originals(d, &tl);
            // lines, not events: a translation pair is one
            (mains.iter().zip(&old))
                .filter(|&(&e, o)| times(d, e) != *o)
                .count()
        })
    }

    pub fn snap_to_video(&mut self, rows: &[usize], start: i64, end: i64, set_start: bool) {
        let evs = self.all_events(rows);
        let (start, end) = (Time(start), Time(end));
        self.edit("Snap to video", |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                if set_start || ev.start > start {
                    ev.start = start;
                }
                if !set_start || ev.end < end {
                    ev.end = end;
                }
            }
        });
    }

    pub fn post_process(&mut self, spec: &PostSpec, keyframes: &[i64], timecodes: &[i64]) {
        // the rows' own events: with both halves of a pair, one line's original became the
        // "next line" of the one before
        let (evs, tl) = (self.main_events(&spec.rows), self.tl_pairs(&spec.rows));
        let fr = Framerate::from_timecodes(timecodes.to_vec())
            .ok()
            .or_else(|| self.framerate.clone());
        let kf = keyframes.to_vec();
        let spec = spec.clone();
        self.edit("Post-process timing", move |d| {
            let opt = |v: i64| (v > 0).then_some(Time(v));
            let pp = ops::PostProcess {
                lead_in: opt(spec.lead_in),
                lead_out: opt(spec.lead_out),
                adjacent_threshold: opt(spec.adjacent),
                adjacent_overlap: opt(spec.overlap),
                adjacent_bias: spec.bias,
                keyframes: match (&fr, spec.snap_keyframes && !kf.is_empty()) {
                    (Some(f), true) => Some((kf.as_slice(), f)),
                    _ => None,
                },
                kf_before_start: Time(spec.kf_before_start),
                kf_after_start: Time(spec.kf_after_start),
                kf_before_end: Time(spec.kf_before_end),
                kf_after_end: Time(spec.kf_after_end),
                styles: Vec::new(),
            };
            ops::post_process(d, &Rows::Selected(evs), &pp);
            sync_originals(d, &tl);
        });
    }
}

/// Keep `\move`/`\t` absolute timing when only the line start moved by -`delta`.
fn shift_tag_times(ev: &mut Event, delta: i64) {
    let mut line = Line::parse(&ev.text);
    line.for_each_tag_mut(|t| {
        // the times counted from the line's start (\fad's fade-out counts from its end)
        let idx: &[usize] = match t.name.as_str() {
            "move" if t.args.len() >= 6 => &[4, 5],
            "t" if t.args.len() >= 3 => &[0, 1],
            "fad" if t.args.len() >= 2 => &[0],
            "fade" if t.args.len() >= 7 => &[3, 4, 5, 6],
            _ => &[],
        };
        if idx.is_empty() {
            return;
        }
        let mut a = t.args.clone();
        for &i in idx {
            if let Some(v) = t.arg_f64(i) {
                a[i] = ((v as i64) + delta).max(0).to_string();
            }
        }
        t.set_args(a);
    });
    ev.text = line.to_text();
}

/// Kainote's "new times": a line that does not run into the next shifted one (nor ends with
/// it) lasts 110 ms per shown character, at least a second.
fn new_end_times(d: &mut Document, evs: &[usize]) {
    let mut order = evs.to_vec();
    order.sort_by_key(|&e| {
        let ev = d.event(e).unwrap();
        (ev.start, ev.end)
    });
    for w in order.windows(2) {
        let (next_start, next_end) = d.event(w[1]).map(|n| (n.start, n.end)).unwrap();
        let ev = d.event_mut(w[0]).unwrap();
        if ev.end <= next_start && ev.end != next_end {
            let chars = tags::char_count(&ev.text, true, false) as i64;
            ev.end = ev.start + Time((110 * chars).max(1000));
        }
    }
}

/// End each line no later than the next line of the same style starts.
fn fix_overlaps(d: &mut Document, evs: &[usize]) {
    // the starts by style, sorted once (only ends change here): a lookup per line instead of a scan
    let mut starts: HashMap<String, Vec<Time>> = HashMap::new();
    for o in d
        .events()
        .iter()
        .map(|x| &x.event)
        .filter(|o| !o.is_comment())
    {
        match starts.get_mut(o.style.as_str()) {
            Some(v) => v.push(o.start),
            None => drop(starts.insert(o.style.clone(), vec![o.start])),
        }
    }
    starts.values_mut().for_each(|v| v.sort_unstable());
    for &e in evs {
        let me = d.event(e).unwrap();
        let next = starts
            .get(me.style.as_str())
            .and_then(|v| v.get(v.partition_point(|&s| s <= me.start)).copied());
        if let Some(n) = next
            && me.end > n
        {
            d.event_mut(e).unwrap().end = n;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn continuous_times_as_aegisub() {
        let times = |s: &Session| -> Vec<(i64, i64)> {
            (0..s.row_count())
                .map(|i| (s.row(i).start, s.row(i).end))
                .collect()
        };
        // one row: its neighbour
        let mut s = session(ASS);
        s.make_continuous(&[1], false);
        s.make_continuous(&[1], true);
        assert_eq!(times(&s), [(1000, 2000), (2000, 5000), (5000, 6000)]);
        s.make_continuous(&[2], true); // the last row has no next one
        assert_eq!(times(&s)[2], (5000, 6000));
        // rows 0 and 2: not adjacent, so nothing links
        let mut s = session(ASS);
        s.make_continuous(&[0, 2], false);
        assert_eq!(times(&s), [(1000, 2000), (3000, 4000), (5000, 6000)]);
        // translator mode: rows, not the events between them, are the neighbours
        s.tl_enable(true);
        s.paste_translation(0, "jeden");
        s.paste_translation(1, "dwa");
        s.make_continuous(&[0, 1], true);
        assert_eq!((s.row(0).end, s.row(1).start), (3000, 3000));
    }

    #[test]
    fn timing_keeps_translation_pairs_together() {
        let mut s = session(ASS);
        s.tl_enable(true);
        for (r, t) in ["jeden", "dwa", "trzy"].into_iter().enumerate() {
            s.paste_translation(r, t);
        }
        let paired = |s: &Session| {
            (0..s.row_count()).all(|r| {
                let (t, d) = (s.rows[r], &s.ed.doc);
                let (o, m) = (d.event(t.original).unwrap(), d.event(t.main()).unwrap());
                t.translation.is_some() && (o.start, o.end) == (m.start, m.end)
            })
        };
        let times = |s: &Session| {
            (0..s.row_count())
                .map(|r| (s.row(r).start, s.row(r).end))
                .collect::<Vec<_>>()
        };
        s.make_continuous(&[0, 1], true);
        assert!(paired(&s));
        assert!(s.undo());
        // the gaps close between rows, not between a line's two halves
        s.post_process(
            &PostSpec {
                adjacent: 1500,
                bias: 0.5,
                rows: vec![0, 1, 2],
                ..Default::default()
            },
            &[],
            &[],
        );
        assert_eq!(times(&s), [(1000, 2500), (2500, 4500), (4500, 6000)]);
        assert!(paired(&s));
        assert!(s.undo());
        // ends pushed past the next line come back to it
        let n = s.shift(
            &ShiftSpec {
                amount: 1500,
                which: 2,
                end_fix: 1,
                ..Default::default()
            },
            &[],
        );
        assert_eq!(n, 3);
        assert_eq!(times(&s), [(1000, 3000), (3000, 5000), (5000, 7500)]);
        assert!(paired(&s));
    }

    #[test]
    fn stretch_times_keeps_the_anchor() {
        let mut s = session(ASS);
        let t0: Vec<_> = (0..3).map(|i| (s.row(i).start, s.row(i).end)).collect();
        let (a, b) = (t0[0].0, t0[1].0);
        s.stretch_times(a, b, b + (b - a)); // twice as long
        let t: Vec<_> = (0..3).map(|i| (s.row(i).start, s.row(i).end)).collect();
        for (o, n) in t0.iter().zip(&t) {
            assert_eq!((n.0, n.1), (a + 2 * (o.0 - a), a + 2 * (o.1 - a)));
        }
    }

    #[test]
    fn new_end_times_follow_the_text() {
        let mut s = session(ASS);
        s.set_texts(&[1], &["a long line of text here".to_string()], "edit");
        let d = &mut s.ed.doc;
        new_end_times(d, &[0, 1, 2]);
        fix_overlaps(d, &[0, 1, 2]);
        let ends: Vec<i64> = (0..3).map(|i| d.event(i).unwrap().end.ms()).collect();
        // a second at least; 19 characters run into the next line; the last line stays
        assert_eq!(ends, [2000, 5000, 6000]);
    }

    #[test]
    fn tag_times_follow_a_start_shift() {
        let mut ev = Event {
            text: r"{\move(0,0,10,10,500,900)\fad(300,200)\fade(255,0,255,0,100,400,800)\t(100,600,\fscx120)}x".into(),
            ..Default::default()
        };
        shift_tag_times(&mut ev, -200);
        assert_eq!(
            ev.text,
            r"{\move(0,0,10,10,300,700)\fad(100,200)\fade(255,0,255,0,0,200,600)\t(0,400,\fscx120)}x"
        );
    }

    #[test]
    fn snap_to_video_keeps_the_line_whole() {
        let mut s = session(ASS);
        // starts at 2.5 s: the first line (to 2 s) now ends with the frame, the second keeps its end
        s.snap_to_video(&[0, 1], 2500, 2540, true);
        assert_eq!(
            [
                (s.row(0).start, s.row(0).end),
                (s.row(1).start, s.row(1).end)
            ],
            [(2500, 2540), (2500, 4000)]
        );
        // ends at 0.5 s: a start after it comes back
        s.snap_to_video(&[2], 460, 500, false);
        assert_eq!((s.row(2).start, s.row(2).end), (460, 500));
    }

    #[test]
    fn continuity_closes_gaps_where_the_bias_puts_them() {
        let times = |s: &Session| {
            (0..3)
                .map(|i| (s.row(i).start, s.row(i).end))
                .collect::<Vec<_>>()
        };
        // Kainote's start 250 and end 750 thresholds: gaps up to 1000, a quarter from the start
        let mut s = session(ASS);
        let spec = PostSpec {
            adjacent: 1000,
            bias: 0.75,
            rows: vec![0, 1, 2],
            ..Default::default()
        };
        s.post_process(&spec, &[], &[]);
        assert_eq!(times(&s), [(1000, 2750), (2750, 4750), (4750, 6000)]);
        let mut s = session(ASS);
        s.post_process(
            &PostSpec {
                adjacent: 999,
                ..spec
            },
            &[],
            &[],
        );
        assert_eq!(times(&s)[1], (3000, 4000));
    }
}
