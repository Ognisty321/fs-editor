//! Joining, splitting and recombining rows; translator pairs join and split half by half.

use fs_core::ops::{self, JoinMode};
use fs_core::tlmode::{self, TlRow};
use fs_core::{Document, Event, Time};

use super::Session;

impl Session {
    /// Aegisub "Recombine Lines"; returns the rows left of the selection.
    pub fn recombine_rows(&mut self, rows: &[usize]) -> Vec<usize> {
        // not in translator mode: it makes and drops lines by the texts' overlaps, which the
        // original and the translation do not share (Aegisub has no such mode)
        if self.tl {
            return rows.to_vec();
        }
        let evs = self.main_events(rows);
        let left = self.edit("Recombine lines", |d| ops::recombine(d, &evs));
        self.rows_of_events(&left)
    }

    /// Aegisub "Split Lines (by karaoke)"; returns the new rows.
    pub fn split_by_karaoke(&mut self, rows: &[usize]) -> Vec<usize> {
        if self.tl {
            return Vec::new();
        }
        let evs = self.main_events(rows);
        let new = self.edit("Split by karaoke", |d| ops::split_by_karaoke(d, &evs));
        self.rows_of_events(&new)
    }

    /// 0 join, 1 keep first, 2 keep last, 3 as karaoke. In translator mode the originals join
    /// as the translations do, into the first row's pair.
    pub fn join_rows(&mut self, rows: &[usize], mode: u8) -> i64 {
        if rows.len() < 2 {
            return -1;
        }
        if self.tl {
            return self.join_pairs(rows, mode);
        }
        let evs = self.main_events(rows);
        let last_text = evs
            .iter()
            .max()
            .and_then(|&e| self.ed.doc.event(e))
            .map(|e| e.text.clone())
            .unwrap_or_default();
        let jm = match mode {
            1 | 2 => JoinMode::KeepFirst,
            3 => JoinMode::AsKaraoke,
            _ => JoinMode::Concatenate { newline: false },
        };
        let r = self.edit("Join lines", |d| {
            let r = ops::join(d, &evs, jm);
            if mode == 2
                && let Some(i) = r
            {
                d.event_mut(i).unwrap().text = last_text;
            }
            r
        });
        r.and_then(|e| self.row_of_event(e))
            .map_or(-1, |r| r as i64)
    }

    /// The rows joined pair by pair: originals with originals, translations with translations
    /// (an untranslated row's empty), timed by the translations.
    fn join_pairs(&mut self, rows: &[usize], mode: u8) -> i64 {
        let mut sel: Vec<TlRow> = rows.iter().filter_map(|&r| self.tl_row(r)).collect();
        sel.sort_by_key(|r| r.original);
        sel.dedup();
        if sel.len() < 2 {
            return -1;
        }
        let doc = &self.ed.doc;
        let main = |r: &TlRow| doc.event(r.main()).unwrap();
        let originals: Vec<Event> = (sel.iter())
            .map(|r| Event {
                start: main(r).start,
                end: main(r).end,
                ..doc.event(r.original).unwrap().clone()
            })
            .collect();
        let translations: Vec<Event> = (sel.iter())
            .map(|r| match r.translation {
                Some(_) => main(r).clone(),
                None => Event {
                    text: String::new(),
                    ..main(r).clone()
                },
            })
            .collect();
        let translated = sel.iter().any(|r| r.translation.is_some());
        let mut o = join_apart(originals, mode);
        let t = translated.then(|| join_apart(translations, mode));
        let style = tlmode::tl_style(doc).unwrap_or_else(|| tlmode::DEFAULT_TL_STYLE.into());
        let first = sel[0];
        let gone: Vec<usize> = (sel[1..].iter())
            .flat_map(|r| std::iter::once(r.original).chain(r.translation))
            .collect();
        self.edit("Join lines", move |d| {
            d.remove_events(&gone); // all after the first pair
            let Some(t) = t else {
                *d.event_mut(first.original).unwrap() = o;
                return;
            };
            o.style = style;
            o.effect = tlmode::strip_doubtful(&o.effect).to_string();
            match first.translation {
                Some(ti) => {
                    *d.event_mut(first.original).unwrap() = o;
                    *d.event_mut(ti).unwrap() = t;
                }
                None => {
                    // the first row was untranslated: it becomes a pair
                    *d.event_mut(first.original).unwrap() = t;
                    d.insert_event(first.original, o);
                }
            }
        });
        self.row_of_event(first.original).map_or(-1, |r| r as i64)
    }

    /// Split at a byte position of the text (of the original when `original`, in translator
    /// mode); `at_ms` -1 estimates the time, -2 keeps both parts' times. A translator's pair
    /// splits the half at the cursor; the other half stays whole on the first part, as typed.
    pub fn split_row(&mut self, row: usize, byte_pos: usize, at_ms: i64, original: bool) -> i64 {
        let Some(r) = self.tl_row(row) else { return -1 };
        let at = (at_ms >= 0).then_some(Time(at_ms));
        if self.tl {
            match r.translation {
                Some(t) => return self.split_pair(r.original, t, byte_pos, at_ms, original),
                // an untranslated row has no translation to split
                None if !original => return -1,
                None => {}
            }
        }
        let n = self.edit("Split line", |d| {
            let (start, end) = d.event(r.main()).map(|e| (e.start, e.end))?;
            let n = ops::split_at(d, r.main(), byte_pos, at)?;
            if at_ms == -2 {
                // Aegisub's "preserve times": both parts keep the line's times
                d.event_mut(r.main())?.end = end;
                d.event_mut(n)?.start = start;
            }
            Some(n)
        });
        n.and_then(|e| self.row_of_event(e))
            .map_or(-1, |r| r as i64)
    }

    fn split_pair(
        &mut self,
        o: usize,
        t: usize,
        byte_pos: usize,
        at_ms: i64,
        original: bool,
    ) -> i64 {
        let doc = &self.ed.doc;
        let (oe, te) = (doc.event(o).unwrap(), doc.event(t).unwrap());
        let mut d = Document::empty();
        d.push_event(if original { oe } else { te }.clone());
        if ops::split_at(&mut d, 0, byte_pos, (at_ms >= 0).then_some(Time(at_ms))).is_none() {
            return -1;
        }
        let (a, b) = (d.event(0).unwrap().clone(), d.event(1).unwrap().clone());
        let whole = |ev: &Event| {
            let first = Event {
                start: a.start,
                end: a.end,
                ..ev.clone()
            };
            let second = Event {
                start: b.start,
                end: b.end,
                text: String::new(),
                ..ev.clone()
            };
            (first, second)
        };
        let ((o1, o2), (t1, t2)) = if original {
            ((a.clone(), b.clone()), whole(te))
        } else {
            (whole(oe), (a.clone(), b.clone()))
        };
        let times = (te.start, te.end);
        let mut parts = [o1, t1, o2, t2];
        if at_ms == -2 {
            for e in &mut parts {
                (e.start, e.end) = times;
            }
        }
        let [o1, t1, o2, t2] = parts;
        self.edit("Split line", move |d| {
            *d.event_mut(o).unwrap() = o1;
            *d.event_mut(t).unwrap() = t1;
            d.insert_events(vec![(t + 1, o2), (t + 1, t2)]);
        });
        self.row_of_event(t + 1).map_or(-1, |r| r as i64)
    }

    pub fn explode_rows(
        &mut self,
        rows: &[usize],
        counts: &[usize],
        texts: &[String],
        starts: &[i64],
        ends: &[i64],
    ) -> Vec<usize> {
        let n = texts.len().min(starts.len()).min(ends.len());
        // (row, its parts), by row
        let mut jobs = Vec::new();
        let mut at = 0;
        for (&row, &count) in rows.iter().zip(counts) {
            let parts = at.min(n)..(at + count).min(n);
            at += count;
            if let Some(r) = self.tl_row(row).filter(|_| !parts.is_empty()) {
                jobs.push((r, parts));
            }
        }
        jobs.sort_by_key(|j| j.0.original);
        jobs.dedup_by_key(|j| j.0.original);
        let gone: Vec<usize> = (jobs.iter())
            .flat_map(|(r, _)| std::iter::once(r.original).chain(r.translation))
            .collect();
        // the parts go where their line was, once the lines before it are gone too
        let doc = &self.ed.doc;
        let (mut parts, mut removed) = (Vec::new(), 0);
        for (r, range) in jobs {
            let at = r.original - removed;
            removed += 1 + usize::from(r.translation.is_some());
            let base = doc.event(r.main()).unwrap();
            let original = r.translation.map(|_| doc.event(r.original).unwrap());
            for (i, p) in range.enumerate() {
                let (start, end) = (Time(starts[p]), Time(ends[p]));
                if let Some(o) = original {
                    // a translator's pair: the original goes with each copy of the whole line, and
                    // with the first piece of a split text
                    let text = if i == 0 || texts[p] == base.text {
                        o.text.clone()
                    } else {
                        String::new()
                    };
                    parts.push((
                        at,
                        Event {
                            start,
                            end,
                            text,
                            ..o.clone()
                        },
                    ));
                }
                parts.push((
                    at,
                    Event {
                        start,
                        end,
                        text: texts[p].clone(),
                        ..base.clone()
                    },
                ));
            }
        }
        if parts.is_empty() {
            return Vec::new();
        }
        let made = self.edit("Split lines", |d| {
            d.remove_events(&gone);
            d.insert_events(parts)
        });
        self.rows_of_events(&made)
    }
}

/// The originals of translation pairs take their translations' times.
/// `evs` (in order) joined as [`Session::join_rows`]'s `mode` says, apart from any file.
fn join_apart(evs: Vec<Event>, mode: u8) -> Event {
    let last = evs.last().map(|e| e.text.clone()).unwrap_or_default();
    let jm = match mode {
        1 | 2 => JoinMode::KeepFirst,
        3 => JoinMode::AsKaraoke,
        _ => JoinMode::Concatenate { newline: false },
    };
    let mut d = Document::empty();
    let all: Vec<usize> = evs.into_iter().map(|e| d.push_event(e)).collect();
    ops::join(&mut d, &all, jm);
    let mut e = d.event(0).unwrap().clone();
    if mode == 2 {
        e.text = last;
    }
    e
}

#[cfg(test)]
mod tests {
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn explode_rows_splits_the_lines_in_one_step() {
        let mut s = session(ASS);
        let parts = |v: &[&str]| v.iter().map(|t| t.to_string()).collect::<Vec<_>>();
        // rows given in any order; row 1 stays
        let made = s.explode_rows(
            &[2, 0],
            &[2, 3],
            &parts(&["c1", "c2", "a1", "a2", "a3"]),
            &[5000, 5500, 1000, 1300, 1600],
            &[5500, 6000, 1300, 1600, 2000],
        );
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["a1", "a2", "a3", "two", "c1", "c2"]);
        assert_eq!(made, [0, 1, 2, 4, 5]);
        assert_eq!((s.row(4).start, s.row(5).end), (5000, 6000));
        assert!(s.undo());
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["one", "two", "three"]);
    }

    #[test]
    fn split_row_estimates_or_keeps_times() {
        let mut s = session(ASS);
        assert_eq!(s.split_row(2, 2, -2, false), 3);
        let (a, b) = (s.row(2), s.row(3));
        assert_eq!((a.text.as_str(), a.start, a.end), ("th", 5000, 6000));
        assert_eq!((b.text.as_str(), b.start, b.end), ("ree", 5000, 6000));
        assert_eq!(s.split_row(0, 1, -1, false), 1);
        assert_eq!((s.row(0).end, s.row(1).start), (1333, 1333));
    }
}
