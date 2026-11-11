//! The karaoke bar: a line's syllables and the edits made on them.

use fs_core::karaoke::Karaoke;
use fs_core::{Event, Time};

use super::Session;
use crate::ffi::Syllable;

impl Session {
    fn kara(&self, row: usize) -> Option<(usize, Karaoke)> {
        let e = self.tl_row(row)?.main();
        Some((e, Karaoke::from_event(self.ed.doc.event(e)?)))
    }

    pub fn syllables(&self, row: usize) -> Vec<Syllable> {
        self.kara(row)
            .map_or_else(Vec::new, |(_, k)| bar_syllables(&k))
    }

    fn kara_edit(&mut self, row: usize, desc: &str, amend: &str, f: impl FnOnce(&mut Karaoke)) {
        let Some((e, mut k)) = self.kara(row) else {
            return;
        };
        f(&mut k);
        self.edit_amend(amend, desc, |d| k.apply(d.event_mut(e).unwrap()));
    }

    /// Split syllable `syl` at `char_pos` UTF-16 units into its visible text (the bar's).
    pub fn kara_split(&mut self, row: usize, syl: usize, char_pos: usize) {
        self.kara_edit(row, "Karaoke split", "", |k| {
            if syl < k.syls.len() {
                k.split_visible(syl, char_pos)
            }
        });
    }

    /// Karaoke mode's line times: the first syllable takes the start's change and the last one
    /// the end's, so the others keep their place against the audio (Aegisub `SetLineTimes`).
    pub fn kara_set_times(&mut self, row: usize, start: i64, end: i64, amend: &str) {
        let Some((e, mut k)) = self.kara(row) else {
            return;
        };
        if k.syls.len() > 1 {
            k.set_line_times(Time(start), Time(end));
        } else {
            (k.line_start, k.line_end) = (Time(start), Time(end.max(start)));
        }
        let single = k.syls.len() <= 1;
        self.edit_amend(amend, "Karaoke timing", |d| {
            let ev = d.event_mut(e).unwrap();
            if single {
                // no syllables to keep in place: the text stays as it is
                (ev.start, ev.end) = (k.line_start, k.line_end);
            } else {
                k.apply(ev);
            }
        });
    }

    pub fn kara_join(&mut self, row: usize, syl: usize) {
        self.kara_edit(row, "Karaoke join", "", |k| k.join(syl));
    }

    pub fn kara_kind(&self, row: usize) -> String {
        self.kara(row)
            .and_then(|(_, k)| k.syls.first().map(|s| s.kind.clone()))
            .unwrap_or_default()
    }

    /// Every syllable's tag becomes `kind` (Aegisub's karaoke bar menu).
    pub fn kara_set_kind(&mut self, row: usize, kind: &str) {
        let same = self
            .kara(row)
            .is_none_or(|(_, k)| k.syls.iter().all(|s| s.kind == kind));
        if !same {
            self.kara_edit(row, "Karaoke tag", "", |k| k.set_kind(kind));
        }
    }

    /// Move syllable boundary `index` (1..n-1) to `ms`; `shift_rest` moves
    /// the following boundaries by the same amount (right-drag). Steps with the same `amend`
    /// (one drag) make one undo step.
    pub fn kara_set_boundary(
        &mut self,
        row: usize,
        index: usize,
        ms: i64,
        shift_rest: bool,
        amend: &str,
    ) {
        self.kara_edit(row, "Karaoke timing", amend, |k| {
            if shift_rest && index > 0 && index < k.syls.len() {
                let delta = Time(ms) - k.syl_start(index);
                k.shift_markers(index, delta);
            } else {
                k.set_marker(index, Time(ms));
            }
        });
    }
}

pub fn syllables_of(text: &str, start: i64, end: i64) -> Vec<Syllable> {
    let ev = Event::dialogue(Time(start), Time(end), "", text);
    bar_syllables(&Karaoke::from_event(&ev))
}

fn bar_syllables(k: &Karaoke) -> Vec<Syllable> {
    (0..k.syls.len())
        .map(|i| Syllable {
            start: k.syl_start(i).ms(),
            end: k.syl_end(i).ms(),
            text: k.syl_display(i),
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn visual_tags_and_karaoke() {
        let mut s = session(ASS);
        s.set_tags(&[0], "\\pos(100,200)", "");
        assert_eq!(s.row(0).text, "{\\pos(100,200)}one");
        let v = s.visual(0);
        assert!(v.has_pos && v.x == 100.0 && v.y == 200.0 && v.align == 2);
        let v = s.visual(1);
        assert_eq!((v.x, v.y), (960.0, 1070.0));
        let mut r = s.row(2);
        r.text = "{\\k50}ka{\\k50}ra".into();
        s.set_row(2, &r, "");
        let syl = s.syllables(2);
        assert_eq!(syl.len(), 2);
        assert_eq!((syl[1].start, syl[1].end), (5500, 6000));
        s.kara_set_boundary(2, 1, 5300, false, "");
        assert_eq!(s.syllables(2)[0].end, 5300);
        r.text = "{\\k20}a{\\k20}b{\\k60}c".into();
        s.set_row(2, &r, "");
        // shifting the rest keeps b's length (it used to move the wrong boundaries)
        s.kara_set_boundary(2, 1, 5300, true, "");
        assert_eq!(s.row(2).text, "{\\k30}a{\\k20}b{\\k50}c");
        // one drag, one undo step
        s.kara_set_boundary(2, 1, 5400, false, "drag");
        s.kara_set_boundary(2, 1, 5450, false, "drag");
        assert!(s.undo());
        assert_eq!(s.row(2).text, "{\\k30}a{\\k20}b{\\k50}c");
        // the line's times move only the outer syllables: b stays at 5300..5500
        s.kara_set_times(2, 4800, 6200, "");
        assert_eq!(s.row(2).text, "{\\k50}a{\\k20}b{\\k70}c");
        assert_eq!((s.row(2).start, s.row(2).end), (4800, 6200));
        // the tag of every syllable, one undo step; the same tag again changes nothing
        assert_eq!(s.kara_kind(2), "k");
        s.kara_set_kind(2, "kf");
        assert_eq!(s.row(2).text, "{\\kf50}a{\\kf20}b{\\kf70}c");
        s.kara_set_kind(2, "kf");
        assert!(s.undo());
        assert_eq!(s.kara_kind(2), "k");
        // a line without karaoke keeps its text
        s.kara_set_times(1, 2900, 4000, "");
        assert_eq!((s.row(1).text.as_str(), s.row(1).start), ("two", 2900));
        // a proposed split, not yet written
        let p = syllables_of("{\\k30}ka{\\k70}ra", 1000, 2000);
        assert_eq!(
            (p[1].start, p[1].end, p[1].text.as_str()),
            (1300, 2000, "ra")
        );
    }
}
