//! Karaoke timing (Aegisub's karaoke mode in the audio box): a line is a
//! list of syllables whose boundaries are dragged on the audio display,
//! split at a character or joined, then written back as `\k` tags.
//!
//! Durations are kept in milliseconds but ASS stores centiseconds, so every
//! edit rounds boundaries to 10 ms like Aegisub does.

use crate::model::Event;
use crate::tags::{Syllable, join_syllables, karaoke_syllables};
use crate::time::Time;

#[derive(Debug, Clone, PartialEq)]
pub struct Karaoke {
    /// Absolute line start.
    pub line_start: Time,
    pub line_end: Time,
    pub syls: Vec<Syllable>,
}

fn round_cs(ms: i64) -> i64 {
    (ms + 5).div_euclid(10) * 10
}

impl Karaoke {
    /// Parse an event. A line without karaoke tags becomes one syllable
    /// spanning the whole line.
    pub fn from_event(e: &Event) -> Self {
        let mut syls = karaoke_syllables(&e.text);
        let dur = (e.end - e.start).ms().max(0);
        let has_k = crate::tags::Line::parse(&e.text)
            .tags()
            .any(|t| matches!(t.name.as_str(), "k" | "K" | "kf" | "ko"));
        if !has_k {
            syls = vec![Syllable {
                start_ms: 0,
                duration_ms: dur,
                kind: "k".into(),
                text: e.text.clone(),
            }];
        }
        Self {
            line_start: e.start,
            line_end: e.end,
            syls,
        }
    }

    pub fn text(&self) -> String {
        join_syllables(&self.syls)
    }

    /// Write the syllables back into the event (text only; line times are
    /// left to the caller, see [`Karaoke::set_line_times`]).
    pub fn apply(&self, e: &mut Event) {
        e.text = self.text();
        e.start = self.line_start;
        e.end = self.line_end;
    }

    /// Absolute start of syllable `i`.
    pub fn syl_start(&self, i: usize) -> Time {
        self.line_start + Time(self.syls[i].start_ms)
    }

    pub fn syl_end(&self, i: usize) -> Time {
        self.syl_start(i) + Time(self.syls[i].duration_ms)
    }

    /// Boundaries between syllables (absolute), i.e. the draggable markers.
    pub fn markers(&self) -> Vec<Time> {
        (1..self.syls.len()).map(|i| self.syl_start(i)).collect()
    }

    fn recompute_starts(&mut self) {
        let mut pos = self.syls.first().map(|s| s.start_ms).unwrap_or(0);
        for s in &mut self.syls {
            s.start_ms = pos;
            pos += s.duration_ms;
        }
    }

    /// Visible text of syllable `i` (tags removed) — what the karaoke bar shows.
    pub fn syl_display(&self, i: usize) -> String {
        crate::tags::strip_tags(&self.syls[i].text)
    }

    /// Split syllable `i` at byte offset `pos` of its text. The new syllable
    /// takes a share of the duration proportional to its text length
    /// (Aegisub `AssKaraoke::AddSplit`).
    pub fn split(&mut self, i: usize, pos: usize) {
        // the shares go by visible characters, tags left out (Aegisub keeps tags apart)
        let visible = |t: &str| crate::tags::strip_tags(t).chars().count() as i64;
        let syl = &mut self.syls[i];
        let total_len = visible(&syl.text);
        let mut new = Syllable {
            kind: syl.kind.clone(),
            ..Default::default()
        };
        if pos < syl.text.len() && syl.text.is_char_boundary(pos) {
            new.text = syl.text.split_off(pos);
        }
        if new.text.is_empty() {
            new.duration_ms = 0;
        } else if syl.text.is_empty() {
            new.duration_ms = syl.duration_ms;
            syl.duration_ms = 0;
        } else {
            new.duration_ms = round_cs(syl.duration_ms * visible(&new.text) / total_len.max(1));
            syl.duration_ms -= new.duration_ms;
        }
        // tags at the split point belong to the new syllable's start
        self.syls.insert(i + 1, new);
        self.recompute_starts();
    }

    /// [`split`](Self::split) at `units` UTF-16 units into the syllable's visible text (tags
    /// skipped), where the karaoke bar's character positions point.
    pub fn split_visible(&mut self, i: usize, units: usize) {
        let raw = &self.syls[i].text;
        let (mut seen, mut in_tag, mut at) = (0, false, raw.len());
        for (b, c) in raw.char_indices() {
            if seen >= units && !in_tag {
                at = b; // before a tag that follows: it starts the new syllable
                break;
            }
            match c {
                '{' => in_tag = true,
                '}' if in_tag => in_tag = false,
                _ if !in_tag => seen += c.len_utf16(),
                _ => {}
            }
        }
        self.split(i, at);
    }

    /// Merge syllable `i` into the previous one (removes the marker before it).
    pub fn join(&mut self, i: usize) {
        if i == 0 || i >= self.syls.len() {
            return;
        }
        let s = self.syls.remove(i);
        let prev = &mut self.syls[i - 1];
        prev.duration_ms += s.duration_ms;
        prev.text.push_str(&s.text);
        self.recompute_starts();
    }

    /// Move the boundary before syllable `i` (`1..len`) to absolute time `t`,
    /// clamped between the neighbouring boundaries. Returns the actual time.
    pub fn set_marker(&mut self, i: usize, t: Time) -> Time {
        if i == 0 || i >= self.syls.len() {
            return t;
        }
        let lo = self.syls[i - 1].start_ms;
        let hi = self.syls[i].start_ms + self.syls[i].duration_ms;
        let rel = round_cs((t - self.line_start).ms()).clamp(lo, hi);
        self.syls[i - 1].duration_ms = rel - lo;
        self.syls[i].duration_ms = hi - rel;
        self.recompute_starts();
        self.line_start + Time(rel)
    }

    /// Move the boundaries before syllables `from..` by `delta`, each clamped
    /// by its neighbours; moving later starts from the far end so every
    /// boundary has room (Aegisub `ModifyLength` with shift_following).
    pub fn shift_markers(&mut self, from: usize, delta: Time) {
        let (from, n) = (from.max(1), self.syls.len());
        for k in 0..n.saturating_sub(from) {
            let i = if delta.ms() > 0 { n - 1 - k } else { from + k };
            self.set_marker(i, self.syl_start(i) + delta);
        }
    }

    /// Change the line's start/end: the first syllable absorbs the start
    /// change and the last one the end change (durations never go negative).
    pub fn set_line_times(&mut self, start: Time, end: Time) {
        let ds = (start - self.line_start).ms();
        if let Some(first) = self.syls.first_mut() {
            first.duration_ms = (first.duration_ms - ds).max(0);
        }
        self.line_start = start;
        self.line_end = end.max(start);
        self.recompute_starts();
        let total = (self.line_end - self.line_start).ms();
        let used: i64 = self.syls.iter().map(|s| s.duration_ms).sum();
        if let Some(last) = self.syls.last_mut() {
            last.duration_ms = (last.duration_ms + total - used).max(0);
        }
        self.recompute_starts();
    }

    /// Syllable under absolute time `t`.
    pub fn syl_at(&self, t: Time) -> Option<usize> {
        (0..self.syls.len()).find(|&i| t >= self.syl_start(i) && t < self.syl_end(i))
    }

    /// Change the karaoke tag of every syllable (`k`, `kf`, `ko`, `K`).
    pub fn set_kind(&mut self, kind: &str) {
        for s in &mut self.syls {
            s.kind = kind.to_string();
        }
    }

    /// Split every syllable into one syllable per character (spaces stay
    /// attached to the preceding character), dividing durations evenly.
    pub fn split_characters(&mut self) {
        let mut out = Vec::new();
        for s in &self.syls {
            let line = crate::tags::Line::parse(&s.text);
            // keep syllables containing tags or drawings intact
            if line
                .blocks
                .iter()
                .any(|b| !matches!(b, crate::tags::Block::Text(_)))
            {
                out.push(s.clone());
                continue;
            }
            let mut pieces: Vec<String> = Vec::new();
            for c in s.text.chars() {
                match pieces.last_mut() {
                    Some(p) if c.is_whitespace() => p.push(c),
                    _ => pieces.push(c.to_string()),
                }
            }
            if pieces.len() <= 1 {
                out.push(s.clone());
                continue;
            }
            let n = pieces.len() as i64;
            let mut acc = 0;
            for (k, p) in pieces.into_iter().enumerate() {
                let end = round_cs(s.duration_ms * (k as i64 + 1) / n);
                out.push(Syllable {
                    start_ms: 0,
                    duration_ms: end - acc,
                    kind: s.kind.clone(),
                    text: p,
                });
                acc = end;
            }
        }
        self.syls = out;
        self.recompute_starts();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(text: &str, start: i64, end: i64) -> Event {
        Event {
            text: text.into(),
            start: Time(start),
            end: Time(end),
            ..Default::default()
        }
    }

    #[test]
    fn split_at_a_visible_position() {
        // a tag before the text and a letter of two bytes
        let mut k = Karaoke::from_event(&ev(r"{\k100}{\i1}ąbc", 0, 1000));
        k.split_visible(0, 1);
        assert_eq!(k.text(), r"{\k33}{\i1}ą{\k67}bc");
        // a tag at the split point goes with the new syllable
        let mut k = Karaoke::from_event(&ev(r"{\k100}ab{\b1}c", 0, 1000));
        k.split_visible(0, 2);
        assert_eq!(k.text(), r"{\k67}ab{\k33}{\b1}c");
    }

    #[test]
    fn parse_markers_and_write_back() {
        let k = Karaoke::from_event(&ev(r"{\k20}ka{\kf30}ra{\k50}o{\b1}ke", 1000, 2000));
        assert_eq!(k.syls.len(), 3);
        assert_eq!(k.markers(), vec![Time(1200), Time(1500)]);
        assert_eq!(k.text(), r"{\k20}ka{\kf30}ra{\k50}o{\b1}ke");
        assert_eq!(k.syl_at(Time(1499)), Some(1));
        assert_eq!(k.syl_display(2), "oke");
    }

    #[test]
    fn plain_line_is_one_syllable() {
        let k = Karaoke::from_event(&ev("hello", 0, 1230));
        assert_eq!(k.syls.len(), 1);
        assert_eq!(k.text(), r"{\k123}hello");
    }

    #[test]
    fn split_join_and_markers() {
        let mut k = Karaoke::from_event(&ev(r"{\k100}abcd", 0, 1000));
        k.split(0, 1); // a | bcd
        assert_eq!(k.text(), r"{\k25}a{\k75}bcd");
        k.split(1, 1); // a | b | cd
        assert_eq!(k.syls.iter().map(|s| s.duration_ms).sum::<i64>(), 1000);
        // drag the first marker past the next one: clamped
        assert_eq!(k.set_marker(1, Time(5000)), k.syl_start(2));
        assert_eq!(k.syls[1].duration_ms, 0);
        k.set_marker(1, Time(104));
        assert_eq!(k.syls[0].duration_ms, 100);
        k.join(1);
        k.join(1);
        assert_eq!(k.text(), r"{\k100}abcd");
        // split past the end gives an empty zero-length syllable
        k.split(0, 99);
        assert_eq!(k.text(), r"{\k100}abcd{\k0}");
    }

    #[test]
    fn shift_markers_moves_the_following_boundaries() {
        let mut k = Karaoke::from_event(&ev(r"{\k10}a{\k10}b{\k10}c{\k70}d", 0, 1000));
        // later: from the far end, so b and c keep their lengths
        k.shift_markers(1, Time(150));
        assert_eq!(k.text(), r"{\k25}a{\k10}b{\k10}c{\k55}d");
        // earlier: the first boundary is clamped at the line start, the rest move fully
        k.shift_markers(1, Time(-300));
        assert_eq!(k.text(), r"{\k0}a{\k5}b{\k10}c{\k85}d");
        let mut k = Karaoke::from_event(&ev(r"{\k10}a{\k10}b", 0, 200));
        k.shift_markers(0, Time(500)); // the last boundary stops at the line end
        assert_eq!(k.text(), r"{\k20}a{\k0}b");
    }

    #[test]
    fn line_times_and_characters() {
        let mut k = Karaoke::from_event(&ev(r"{\k50}ab{\k50}c", 1000, 2000));
        k.set_line_times(Time(1200), Time(2500));
        assert_eq!(k.text(), r"{\k30}ab{\k100}c");
        let mut e = ev("", 0, 0);
        k.apply(&mut e);
        assert_eq!((e.start, e.end), (Time(1200), Time(2500)));
        k.split_characters();
        assert_eq!(k.text(), r"{\k15}a{\k15}b{\k100}c");
        k.set_kind("kf");
        assert!(k.text().starts_with(r"{\kf15}"));
    }
}
