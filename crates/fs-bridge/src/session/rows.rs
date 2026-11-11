//! The grid's rows: reading and writing their fields, inserting, deleting, moving, sorting,
//! the clipboard, comparing two files and the lookups by time.

use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use fs_core::model::{AssVersion, EventField};
use fs_core::ops;
use fs_core::tags;
use fs_core::tlmode;
use fs_core::{Document, Event, EventEntry, EventId, Time};

use super::{Session, event_hash, sync_originals};
use crate::ffi::{ColumnInfo, ComparePair, Row, RowSpan};

impl Session {
    fn style_or_default(&self, row: usize) -> String {
        self.tl_row(row)
            .or_else(|| self.rows.last().copied())
            .and_then(|r| self.ed.doc.event(r.main()))
            .map(|e| e.style.clone())
            .or_else(|| self.ed.doc.styles().next().map(|s| s.name.clone()))
            .unwrap_or_else(|| "Default".into())
    }

    /// Rows whose start and end match non-comment lines of `other` none of
    /// which has the same text (comments are not compared).
    /// Kainote's comparison: walks `rows` and `other_rows` in order and pairs each line with
    /// the next one on the other side, which must have the same times and/or style when
    /// asked (comments are skipped); returns the pairs whose texts differ.
    pub fn compare<'a>(
        &'a self,
        other: &'a Session,
        rows: &[usize],
        other_rows: &[usize],
        by_times: bool,
        by_styles: bool,
    ) -> Vec<ComparePair> {
        // Kainote walks the other rows from the last pair on, which rescans the rest for every
        // line without a partner; the positions by pairing key find the same partner in log time.
        let key = |c: &(Time, Time, &'a str, &'a str)| {
            (by_times.then_some((c.0, c.1)), by_styles.then_some(c.2))
        };
        let mut at: HashMap<_, Vec<usize>> = HashMap::new();
        for (k, &j) in other_rows.iter().enumerate() {
            if let Some(b) = other.compared(j) {
                at.entry(key(&b)).or_default().push(k);
            }
        }
        let mut out = Vec::new();
        let mut next = 0;
        for &i in rows {
            let Some(a) = self.compared(i) else { continue };
            let Some(ks) = at.get(&key(&a)) else { continue };
            let Some(&k) = ks.get(ks.partition_point(|&k| k < next)) else {
                continue;
            };
            let j = other_rows[k];
            let b = other.compared(j).expect("indexed above");
            next = k + 1;
            if a.3 != b.3 {
                let (spans, other_spans) = diff_spans(a.3, b.3);
                out.push(ComparePair {
                    row: i,
                    other: j,
                    spans,
                    other_spans,
                });
            }
        }
        out
    }

    /// Times, style and shown text of a non-comment row.
    fn compared(&self, i: usize) -> Option<(Time, Time, &str, &str)> {
        let r = *self.rows.get(i)?;
        let ev = &self.ed.doc.events()[r.main()].event;
        let text = if self.tl {
            tlmode::translation_text(&self.ed.doc, r)
        } else {
            &ev.text
        };
        (!ev.is_comment()).then_some((ev.start, ev.end, ev.style.as_str(), text))
    }

    pub fn column_info(&mut self) -> ColumnInfo {
        let ci = self.col_info.take().unwrap_or_else(|| self.scan_columns());
        self.col_info = Some(ci.clone());
        ci
    }

    fn scan_columns(&self) -> ColumnInfo {
        let mut ci = ColumnInfo::default();
        let (mut styles, mut actors, mut effects) =
            (HashSet::new(), HashSet::new(), HashSet::new());
        let events = self.ed.doc.events();
        let mut last = ("", "", "");
        for r in &self.rows {
            let (layer, ml, mr, mv, style, actor, effect) = column_key(&events[r.main()].event);
            ci.any_layer |= layer;
            ci.any_margin_l |= ml;
            ci.any_margin_r |= mr;
            ci.any_margin_v |= mv;
            let cur = (style, actor, effect);
            // neighbouring lines mostly repeat these: hash only on a change
            if cur.0 != last.0 || styles.is_empty() {
                styles.insert(cur.0);
            }
            if cur.1 != last.1 {
                actors.insert(cur.1);
            }
            if cur.2 != last.2 {
                effects.insert(cur.2);
            }
            last = cur;
        }
        let owned = |set: HashSet<&str>| {
            set.into_iter()
                .filter(|s| !s.is_empty())
                .map(String::from)
                .collect()
        };
        ci.styles = styles.into_iter().map(String::from).collect();
        (ci.actors, ci.effects) = (owned(actors), owned(effects));
        ci
    }

    pub fn row_spans(&self) -> Vec<RowSpan> {
        let events = self.ed.doc.events();
        self.rows
            .iter()
            .map(|r| {
                let ev = &events[r.main()].event;
                RowSpan {
                    start: ev.start.ms(),
                    end: ev.end.ms(),
                    comment: ev.is_comment(),
                }
            })
            .collect()
    }

    pub fn row(&self, i: usize) -> Row {
        let Some(r) = self.tl_row(i) else {
            return Row::default();
        };
        let doc = &self.ed.doc;
        let entry: &Arc<EventEntry> = &doc.events()[r.main()];
        let ev = &entry.event;
        let (markers, actor) = tlmode::split_actor(&ev.actor);
        let hash = event_hash(ev);
        let same = |m: &HashMap<EventId, u64>| m.get(&entry.id) == Some(&hash);
        let state = if same(&self.loaded) {
            0
        } else if same(&self.saved) {
            2
        } else {
            1
        };
        let (text, original) = if self.tl {
            (
                tlmode::translation_text(doc, r).to_string(),
                tlmode::original_text(doc, r).to_string(),
            )
        } else {
            (ev.text.clone(), String::new())
        };
        let shown = if self.tl && r.translation.is_none() {
            &original
        } else {
            &text
        };
        Row {
            start: ev.start.ms(),
            end: ev.end.ms(),
            layer: ev.layer,
            style: ev.style.clone(),
            actor: actor.to_string(),
            effect: tlmode::strip_doubtful(&ev.effect).to_string(),
            margin_l: ev.margin_l,
            margin_r: ev.margin_r,
            margin_v: ev.margin_v,
            cps: tags::cps(shown, ev.duration().ms()),
            wraps: wraps(shown),
            text,
            original,
            comment: ev.is_comment(),
            doubtful: tlmode::is_doubtful(&ev.effect),
            translated: r.translation.is_some(),
            state,
            markers: markers.concat(),
        }
    }

    /// Write every field of a row back (edit box commit).
    pub fn set_row(&mut self, i: usize, row: &Row, amend: &str) {
        let Some(r) = self.tl_row(i) else { return };
        // typing leaves the columns alone: keep their info (a rescan of 100k lines per key)
        let old = self.ed.doc.event(r.main()).cloned();
        let (n, ci) = (self.rows.len(), self.col_info.take());
        let tl = self.tl;
        let row = row.clone();
        self.edit_amend(amend, "Edit line", move |d| {
            let main = r.main();
            let r = if tl && r.translation.is_none() && !row.text.is_empty() {
                tlmode::set_translation(d, r, &row.text)
            } else {
                r
            };
            let mut ev = d.event(r.main()).unwrap().clone();
            ev.start = Time(row.start);
            ev.end = Time(row.end);
            ev.layer = row.layer;
            ev.style = row.style.clone();
            let prefixes: Vec<&str> = tlmode::split_actor(&row.markers).0;
            ev.actor = tlmode::join_actor(&prefixes, &row.actor);
            ev.margin_l = row.margin_l;
            ev.margin_r = row.margin_r;
            ev.margin_v = row.margin_v;
            ev.effect = row.effect.clone();
            if row.doubtful && r.translation.is_some() {
                ev.effect.push_str(tlmode::DOUBTFUL_MARK);
            }
            ev.kind = if row.comment {
                fs_core::model::EventKind::Comment
            } else {
                fs_core::model::EventKind::Dialogue
            };
            if !tl || r.translation.is_some() {
                ev.text = row.text.clone();
            }
            if *d.event(r.main()).unwrap() != ev {
                *d.event_mut(r.main()).unwrap() = ev.clone();
            }
            if tl {
                // the original mirrors the line's fields but keeps the TL style
                let o = r.original;
                let mut orig = d.event(o).unwrap().clone();
                if r.translation.is_some() {
                    let style = orig.style.clone();
                    orig = Event {
                        text: row.original.clone(),
                        style,
                        effect: tlmode::strip_doubtful(&ev.effect).to_string(),
                        ..ev.clone()
                    };
                } else if o == main {
                    orig.text = row.original.clone();
                }
                if *d.event(o).unwrap() != orig {
                    *d.event_mut(o).unwrap() = orig;
                }
            }
        });
        let new = self.tl_row(i).and_then(|r| self.ed.doc.event(r.main()));
        if self.rows.len() == n && old.as_ref().map(column_key) == new.map(column_key) {
            self.col_info = ci;
        }
    }

    pub fn set_texts(&mut self, rows: &[usize], texts: &[String], description: &str) {
        let evs: Vec<(usize, String)> = rows
            .iter()
            .zip(texts)
            .filter_map(|(&r, t)| Some((*self.main_events(&[r]).first()?, t.clone())))
            .collect();
        if evs.is_empty() {
            return;
        }
        self.edit(description, move |d| {
            for (e, t) in evs {
                d.event_mut(e).unwrap().text = t;
            }
        });
    }

    pub fn set_rows_field(&mut self, rows: &[usize], field: &str, value: &str) {
        self.set_rows_values(rows, field, &vec![value.to_string(); rows.len()], "");
    }

    /// `field` of each of `rows` set to its value in `values` (the edit box's change to every
    /// selected line, Aegisub's SetSelectedRows); edits with the same non-empty `amend` make one
    /// undo step.
    pub fn set_rows_values(&mut self, rows: &[usize], field: &str, values: &[String], amend: &str) {
        let main = matches!(field, "style" | "effect" | "text");
        let mut evs = Vec::with_capacity(rows.len());
        for (k, r) in rows.iter().enumerate().take(values.len()) {
            let Some(r) = self.tl_row(*r) else { continue };
            if main {
                evs.push((r.main(), k));
            } else {
                evs.push((r.original, k));
                evs.extend(r.translation.map(|t| (t, k)));
            }
        }
        let (field, values) = (field.to_string(), values.to_vec());
        self.edit_amend(amend, &format!("Set {field}"), move |d| {
            for (e, k) in evs {
                let (ev, value) = (d.event_mut(e).unwrap(), &values[k]);
                let num = || value.trim().parse::<i32>().unwrap_or(0);
                match field.as_str() {
                    "text" => ev.text = value.clone(),
                    "style" => ev.style = value.clone(),
                    "actor" => {
                        let (p, _) = tlmode::split_actor(&ev.actor);
                        ev.actor = tlmode::join_actor(&p, value);
                    }
                    "effect" => ev.effect = value.clone(),
                    "layer" => ev.layer = num(),
                    "margin_l" => ev.margin_l = num(),
                    "margin_r" => ev.margin_r = num(),
                    "margin_v" => ev.margin_v = num(),
                    "comment" => {
                        ev.kind = if value == "1" {
                            fs_core::model::EventKind::Comment
                        } else {
                            fs_core::model::EventKind::Dialogue
                        }
                    }
                    "start" => ev.start = Time(value.parse().unwrap_or(0)),
                    "end" => ev.end = Time(value.parse().unwrap_or(0)),
                    // "start,end" in one undo step (snap to scene)
                    "times" => {
                        if let Some((s, e)) = value.split_once(',') {
                            ev.start = Time(s.trim().parse().unwrap_or(0));
                            ev.end = Time(e.trim().parse().unwrap_or(0));
                        }
                    }
                    "markers" => {
                        let (_, a) = tlmode::split_actor(&ev.actor);
                        let a = a.to_string();
                        let p: Vec<&str> = tlmode::split_actor(value).0;
                        ev.actor = tlmode::join_actor(&p, &a);
                    }
                    _ => {}
                }
            }
        });
    }

    /// Insert an empty line before row `at` (`at == row_count` appends).
    pub fn insert_row(&mut self, at: usize, start: i64, end: i64) -> usize {
        let style = self.style_or_default(at.saturating_sub(1));
        self.insert_styled(at, start, end, &style)
    }

    /// A new empty line before row `row` (after it with `after`) in its style (Aegisub's
    /// subtitle/insert), timed `start`..`end`; `fit` cuts that to the time the other lines leave.
    pub fn insert_beside(
        &mut self,
        row: usize,
        after: bool,
        start: i64,
        end: i64,
        fit: bool,
    ) -> usize {
        let (mut start, mut end) = (start.max(0), end.max(0));
        if fit {
            for e in self.ed.doc.events().iter().map(|e| &e.event) {
                if after && e.start.0 >= start {
                    end = end.min(e.start.0);
                }
                if !after && e.end.0 <= end {
                    start = start.max(e.end.0);
                }
            }
        }
        let style = self.style_or_default(row);
        self.insert_styled(row + usize::from(after), start, end, &style)
    }

    fn insert_styled(&mut self, at: usize, start: i64, end: i64, style: &str) -> usize {
        let ev_at = self
            .tl_row(at)
            .map(|r| r.original)
            .unwrap_or(self.ed.doc.event_count());
        self.edit("Insert line", |d| {
            d.insert_event(ev_at, Event::dialogue(Time(start), Time(end), style, ""));
        });
        self.row_of_event(ev_at).unwrap_or(0)
    }

    pub fn duplicate_rows(&mut self, rows: &[usize]) -> Vec<usize> {
        let evs = self.all_events(rows);
        let new = self.edit("Duplicate lines", |d| ops::duplicate(d, &evs));
        self.rows_of_events(&new)
    }

    pub fn delete_rows(&mut self, rows: &[usize]) {
        let evs = self.all_events(rows);
        self.edit("Delete lines", |d| {
            ops::delete_rows(d, &evs);
            // none left: a new empty one to type into (Aegisub)
            if d.event_count() == 0 {
                d.push_event(Event::dialogue(Time(0), Time(5000), "Default", ""));
            }
        });
    }

    pub fn clear_text(&mut self, rows: &[usize]) {
        let evs = self.main_events(rows);
        self.edit("Delete text", |d| {
            for e in evs {
                d.event_mut(e).unwrap().text.clear();
            }
        });
    }

    /// Aegisub's "Clear text, keep override tags": only the `{\...}` blocks stay; comments
    /// and drawings go with the text.
    pub fn clear_text_keep_tags(&mut self, rows: &[usize]) {
        let evs = self.main_events(rows);
        self.edit("Delete text", |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.text = override_blocks(&ev.text);
            }
        });
    }

    pub fn swap_rows(&mut self, a: usize, b: usize) {
        if a == b || a.max(b) >= self.rows.len() {
            return;
        }
        // a row's original goes with its translation
        let mut groups = self.groups();
        groups.swap(a, b);
        self.set_groups("Swap lines", groups);
    }

    /// Move rows up (`delta < 0`) or down (Alt+Up/Down, Alt+drag).
    pub fn move_rows(&mut self, rows: &[usize], delta: i64) -> Vec<usize> {
        let mut sel: Vec<usize> = rows
            .iter()
            .copied()
            .filter(|&r| r < self.rows.len())
            .collect();
        sel.sort_unstable();
        sel.dedup();
        let n = self.rows.len() as i64;
        if sel.is_empty() || delta == 0 {
            return sel;
        }
        // the rows move as one block: it can reach the end even when they had gaps
        let delta = delta.clamp(-(sel[0] as i64), n - sel.len() as i64 - sel[0] as i64);
        let mut groups: Vec<Option<Vec<Arc<EventEntry>>>> =
            self.groups().into_iter().map(Some).collect();
        let moving: Vec<Vec<Arc<EventEntry>>> =
            sel.iter().map(|&r| groups[r].take().unwrap()).collect();
        let rest: Vec<Vec<Arc<EventEntry>>> = groups.into_iter().flatten().collect();
        let first = (sel[0] as i64 + delta) as usize;
        let mut out = rest;
        let new: Vec<usize> = (first..first + moving.len()).collect();
        out.splice(first..first, moving);
        self.set_groups("Move lines", out);
        new
    }

    /// Sort by 0 start, 1 end, 2 style, 3 actor, 4 effect, 5 layer, 6 text.
    pub fn sort_rows(&mut self, key: u8, selected_only: bool, rows: &[usize]) {
        // compared in place: a copy of both events per comparison was most of a sort's time
        fn main(g: &[Arc<EventEntry>]) -> &Event {
            &g.last().unwrap().event
        }
        let cmp = |a: &Event, b: &Event| match key {
            1 => a.end.cmp(&b.end),
            2 => a.style.cmp(&b.style),
            3 => a.actor.cmp(&b.actor),
            4 => a.effect.cmp(&b.effect),
            5 => a.layer.cmp(&b.layer),
            6 => a.text.cmp(&b.text),
            _ => a.start.cmp(&b.start),
        };
        let mut groups = self.groups();
        if selected_only {
            let mut sel: Vec<usize> = rows.iter().copied().filter(|&r| r < groups.len()).collect();
            sel.sort_unstable();
            sel.dedup();
            let mut items: Vec<_> = sel.iter().map(|&r| groups[r].clone()).collect();
            items.sort_by(|a, b| cmp(main(a), main(b)));
            for (r, g) in sel.into_iter().zip(items) {
                groups[r] = g;
            }
        } else {
            // in order already (Kainote's sort on opening a sorted file): no empty undo step
            if groups
                .windows(2)
                .all(|w| cmp(main(&w[0]), main(&w[1])).is_le())
            {
                return;
            }
            groups.sort_by(|a, b| cmp(main(a), main(b)));
        }
        self.set_groups("Sort lines", groups);
    }

    /// The rows' lasting ids (their main event's, which translating or turning translator mode
    /// off keeps), for remembering lines across edits that move them.
    pub fn row_ids(&self, rows: &[usize]) -> Vec<u64> {
        (rows.iter())
            .filter_map(|&r| self.ed.doc.event_id(self.tl_row(r)?.main()))
            .collect()
    }

    /// The rows, in order, that hold any of `ids` ([`Session::row_ids`]).
    pub fn rows_of_ids(&self, ids: &[u64]) -> Vec<usize> {
        if ids.is_empty() {
            return Vec::new();
        }
        let want: HashSet<u64> = ids.iter().copied().collect();
        let doc = &self.ed.doc;
        (0..self.rows.len())
            .filter(|&i| {
                let r = self.rows[i];
                (std::iter::once(r.original).chain(r.translation))
                    .any(|e| doc.event_id(e).is_some_and(|id| want.contains(&id)))
            })
            .collect()
    }

    /// Rows overlapping `row` in time (collision colouring).
    pub fn overlapping(&self, row: usize) -> Vec<usize> {
        let Some(r) = self.tl_row(row) else {
            return Vec::new();
        };
        let me = self.ed.doc.event(r.main()).unwrap();
        if me.is_comment() {
            return Vec::new();
        }
        let events = self.ed.doc.events();
        (0..self.rows.len())
            .filter(|&i| i != row)
            .filter(|&i| {
                let e = &events[self.rows[i].main()].event;
                !e.is_comment() && e.start < me.end && me.start < e.end
            })
            .collect()
    }

    /// Rows visible at `ms` (grid tint, F2 select from video time).
    pub fn rows_at(&self, ms: i64) -> Vec<usize> {
        let events = self.ed.doc.events();
        (0..self.rows.len())
            .filter(|&i| {
                let e = &events[self.rows[i].main()].event;
                !e.is_comment() && e.start.ms() <= ms && ms < e.end.ms()
            })
            .collect()
    }

    /// Kainote's line for a time: the first dialogue with text showing at `ms` (end included),
    /// else the one starting nearest it, the earlier on a tie; -1 when there is none.
    pub fn row_near(&self, ms: i64) -> i64 {
        let events = self.ed.doc.events();
        let mut best: Option<(i64, usize)> = None;
        for (i, r) in self.rows.iter().enumerate() {
            let e = &events[r.main()].event;
            if e.is_comment() || e.text.is_empty() {
                continue;
            }
            if e.start.ms() <= ms && ms <= e.end.ms() {
                return i as i64;
            }
            let d = (e.start.ms() - ms).abs();
            if best.is_none_or(|(bd, _)| d < bd) {
                best = Some((d, i));
            }
        }
        best.map_or(-1, |(_, i)| i as i64)
    }

    /// Selected events as `Dialogue:` lines (clipboard).
    pub fn copy_rows(&self, rows: &[usize]) -> String {
        self.all_events(rows)
            .iter()
            .filter_map(|&e| self.ed.doc.event(e))
            .map(event_line)
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// Paste `Dialogue:` lines (or plain text, one line per row) before row `at`.
    pub fn paste_rows(&mut self, at: usize, text: &str) -> Vec<usize> {
        let style = self.style_or_default(at.saturating_sub(1));
        let evs: Vec<Event> = pasted_lines(text)
            .into_iter()
            .map(|p| p.unwrap_or_else(|l| Event::dialogue(Time(0), Time(5000), &style, l)))
            .collect();
        let ev_at = self
            .tl_row(at)
            .map(|r| r.original)
            .unwrap_or(self.ed.doc.event_count());
        let made = self.edit("Paste lines", |d| {
            d.insert_events(evs.into_iter().map(|ev| (ev_at, ev)).collect())
        });
        self.rows_of_events(&made)
    }

    /// Aegisub's "Paste lines over": clipboard line k goes onto `rows[k]`,
    /// copying only the `fields` bits (Aegisub's order: 1 comment, 2 layer,
    /// 4 start, 8 end, 16 style, 32 actor, 64/128/256 margins L/R/V,
    /// 512 effect, 1024 text). Returns the rows changed.
    pub fn paste_over(&mut self, rows: &[usize], text: &str, fields: u16) -> Vec<usize> {
        // plain text has End 0 here (Aegisub)
        let pasted: Vec<Event> = pasted_lines(text)
            .into_iter()
            .map(|p| p.unwrap_or_else(|l| Event::dialogue(Time(0), Time(0), "", l)))
            .collect();
        let n = pasted.len().min(rows.len());
        let (evs, tl) = (self.main_events(&rows[..n]), self.tl_pairs(&rows[..n]));
        self.edit("Paste lines over", |d| {
            for (src, &e) in pasted.iter().zip(&evs) {
                let ev = d.event_mut(e).unwrap();
                let on = |bit: u16| fields & bit != 0;
                if on(1) {
                    ev.kind = src.kind;
                }
                if on(2) {
                    ev.layer = src.layer;
                }
                if on(4) {
                    ev.start = src.start;
                }
                if on(8) {
                    ev.end = src.end;
                }
                if on(16) {
                    ev.style.clone_from(&src.style);
                }
                if on(32) {
                    ev.actor.clone_from(&src.actor);
                }
                if on(64) {
                    ev.margin_l = src.margin_l;
                }
                if on(128) {
                    ev.margin_r = src.margin_r;
                }
                if on(256) {
                    ev.margin_v = src.margin_v;
                }
                if on(512) {
                    ev.effect.clone_from(&src.effect);
                }
                if on(1024) {
                    ev.text.clone_from(&src.text);
                }
            }
            sync_originals(d, &tl);
        });
        rows[..n].to_vec()
    }
}

fn wraps(text: &str) -> String {
    tags::visible_text(text)
        .split("\\N")
        .flat_map(|l| l.split("\\n"))
        .map(|l| tags::wrap_len(&l.replace("\\h", " ")).to_string())
        .collect::<Vec<_>>()
        .join("/")
}

/// The fields of a row's main event that [`Session::column_info`] reads.
fn column_key(ev: &Event) -> (bool, bool, bool, bool, &str, &str, &str) {
    (
        ev.layer != 0,
        ev.margin_l != 0,
        ev.margin_r != 0,
        ev.margin_v != 0,
        ev.style.as_str(),
        tlmode::split_actor(&ev.actor).1,
        tlmode::strip_doubtful(&ev.effect),
    )
}

/// `Dialogue: …` line of an event in the default v4+ field order.
fn event_line(ev: &Event) -> String {
    let fields: Vec<String> = EventField::default_format(AssVersion::Ass)
        .iter()
        .map(|f| ev.get_field(f))
        .collect();
    format!("{}: {}", ev.kind.key(), fields.join(","))
}

fn override_blocks(text: &str) -> String {
    let mut out = String::new();
    let mut rest = text;
    while let Some(a) = rest.find('{') {
        let Some(b) = rest[a..].find('}') else { break };
        let block = &rest[a..=a + b];
        if block.contains('\\') {
            out.push_str(block);
        }
        rest = &rest[a + b + 1..];
    }
    out
}

/// The clipboard's lines as Aegisub pastes them: a `Dialogue:`/`Comment:` line is an event,
/// any other line plain text for the caller to wrap.
fn pasted_lines(text: &str) -> Vec<Result<Event, &str>> {
    let fmt = EventField::default_format(AssVersion::Ass)
        .iter()
        .map(|f| f.name().to_string())
        .collect::<Vec<_>>()
        .join(", ");
    text.lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .map(|l| {
            let d = Document::parse(&format!("[Events]\nFormat: {fmt}\n{l}\n"));
            d.event(0).cloned().ok_or(l)
        })
        .collect()
}

/// The parts of `a` and `b` outside their longest common subsequence of characters, as
/// (start, length) pairs in UTF-16 units (Kainote's comparison).
fn diff_spans(a: &str, b: &str) -> (Vec<usize>, Vec<usize>) {
    let (a, b): (Vec<char>, Vec<char>) = (a.chars().collect(), b.chars().collect());
    // edits are usually local: the common ends keep the table small
    let pre = a.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let suf = a[pre..]
        .iter()
        .rev()
        .zip(b[pre..].iter().rev())
        .take_while(|(x, y)| x == y)
        .count();
    let (x, y) = (&a[pre..a.len() - suf], &b[pre..b.len() - suf]);
    let (mut in_x, mut in_y) = (vec![false; x.len()], vec![false; y.len()]);
    // shortcut: longer middles (whole drawings) are marked whole; diff them in pieces if needed
    if x.len() * y.len() <= 1_000_000 {
        // t[i][j]: LCS length of x[i..] and y[j..]
        let w = y.len() + 1;
        let mut t = vec![0u32; (x.len() + 1) * w];
        for i in (0..x.len()).rev() {
            for j in (0..y.len()).rev() {
                t[i * w + j] = if x[i] == y[j] {
                    t[(i + 1) * w + j + 1] + 1
                } else {
                    t[(i + 1) * w + j].max(t[i * w + j + 1])
                };
            }
        }
        let (mut i, mut j) = (0, 0);
        while i < x.len() && j < y.len() {
            if x[i] == y[j] {
                (in_x[i], in_y[j]) = (true, true);
                (i, j) = (i + 1, j + 1);
            } else if t[(i + 1) * w + j] >= t[i * w + j + 1] {
                i += 1;
            } else {
                j += 1;
            }
        }
    }
    let spans = |s: &[char], common: &[bool]| {
        let mut out: Vec<usize> = Vec::new();
        let mut at: usize = s[..pre].iter().map(|c| c.len_utf16()).sum();
        for (c, &same) in s[pre..].iter().zip(common) {
            let n = c.len_utf16();
            if !same {
                match out.len() {
                    l if l >= 2 && out[l - 2] + out[l - 1] == at => out[l - 1] += n,
                    _ => out.extend([at, n]),
                }
            }
            at += n;
        }
        out
    };
    (spans(&a, &in_x), spans(&b, &in_y))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn sorting_lines_in_order_adds_no_step() {
        let mut s = session(ASS);
        s.sort_rows(0, false, &[]);
        assert!(!s.can_undo());
        s.sort_rows(6, false, &[]); // one, three, two
        assert!(s.can_undo());
        assert_eq!(s.row(1).text, "three");
    }

    #[test]
    fn row_near_a_time() {
        let s = session(ASS);
        assert_eq!(s.row_near(1500), 0);
        assert_eq!(s.row_near(4000), 1); // the end counts
        // between lines, the nearest start: two's at 3 s against three's at 5 s
        assert_eq!(s.row_near(3900), 1);
        assert_eq!(s.row_near(4400), 2);
        assert_eq!(s.row_near(0), 0);
        assert_eq!(s.row_near(99_000), 2);
        assert_eq!(session(&ASS.replace(",one", ",")).row_near(0), 1); // empty lines are skipped
    }

    #[test]
    fn edit_box_changes_reach_the_selection_in_one_step() {
        let mut s = session(ASS);
        let mut r = s.row(0);
        for t in ["a", "ab"] {
            r.text = t.into();
            s.set_row(0, &r, "edit:0:text:0");
            let v = [t.to_string(), t.to_string()];
            s.set_rows_values(&[1, 2], "text", &v, "edit:0:text:0");
        }
        s.set_rows_values(&[1, 2], "times", &["0,500".into(), "7000,8000".into()], "k");
        let row = |s: &Session, i| (s.row(i).text.to_string(), s.row(i).start, s.row(i).end);
        assert_eq!(row(&s, 1), ("ab".into(), 0, 500));
        assert_eq!(row(&s, 2), ("ab".into(), 7000, 8000));
        s.undo();
        s.undo();
        assert_eq!(row(&s, 0), ("one".into(), 1000, 2000));
        assert_eq!(row(&s, 2), ("three".into(), 5000, 6000));
    }

    #[test]
    fn inserted_lines_fit_between_their_neighbours() {
        let mut s = session(ASS);
        // after "one" (1-2 s), cut at "two" (3 s); before "two", from where "one" ends
        let r = s.insert_beside(0, true, 2000, 5000, true);
        assert_eq!((r, s.row(1).start, s.row(1).end), (1, 2000, 3000));
        let r = s.insert_beside(3, false, 0, 5000, true);
        assert_eq!((r, s.row(3).start, s.row(3).end), (3, 4000, 5000));
        s.delete_rows(&(0..s.row_count()).collect::<Vec<_>>());
        assert_eq!((s.row_count(), s.row(0).text.as_str()), (1, ""));
    }

    /// Lines remembered by id stay themselves when lines are added before them or translated.
    #[test]
    fn row_ids_follow_their_lines() {
        let mut s = session(ASS);
        let ids = s.row_ids(&[1, 2]);
        s.insert_row(0, 0, 500);
        assert_eq!(s.rows_of_ids(&ids), [2, 3]);
        s.tl_enable(true);
        s.paste_translation(2, "dwa");
        assert_eq!(s.rows_of_ids(&ids), [2, 3]);
        s.delete_rows(&[2]);
        assert_eq!(s.rows_of_ids(&ids), [2]);
        assert!(s.rows_of_ids(&[]).is_empty());
    }

    #[test]
    fn compare_pairs_lines_in_order() {
        let a = session(ASS);
        let mut b = session(ASS);
        let old = a.row(1).text;
        b.set_texts(&[1], &[format!("{old}!")], "edit");
        b.set_rows_field(&[2], "start", "5100");
        let all = [0, 1, 2];
        let pairs = |by_times| {
            a.compare(&b, &all, &all, by_times, false)
                .iter()
                .map(|p| (p.row, p.other, p.spans.clone(), p.other_spans.clone()))
                .collect::<Vec<_>>()
        };
        let one = (1, 1, vec![], vec![old.encode_utf16().count(), 1]);
        assert_eq!(pairs(true), std::slice::from_ref(&one));
        // without times the third lines pair up too; their texts are the same
        assert_eq!(pairs(false), [one]);
        // only the given rows: line 0 against line 1
        let p = a.compare(&b, &[0], &[1], false, false);
        assert_eq!((p.len(), p[0].row, p[0].other), (1, 0, 1));
        // a line without a partner is skipped and the next ones still pair; a partner before
        // the last pair is not taken, the order holds
        let mut c = session(ASS);
        c.set_rows_field(&[0], "start", "7000");
        c.set_texts(&[1, 2], &["x".into(), "y".into()], "edit");
        let rows = |p: Vec<ComparePair>| p.iter().map(|p| (p.row, p.other)).collect::<Vec<_>>();
        assert_eq!(
            rows(a.compare(&c, &all, &all, true, false)),
            [(1, 1), (2, 2)]
        );
        assert_eq!(rows(a.compare(&c, &[2, 1], &all, true, false)), [(2, 2)]);
    }

    #[test]
    fn pasted_lines_go_before_the_row_in_order() {
        let mut s = session(ASS);
        let pasted = "Dialogue: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,p1\nplain p2";
        // a line that is not a subtitle line comes in as text (Aegisub)
        assert_eq!(s.paste_rows(1, pasted), [1, 2]);
        assert_eq!(s.paste_rows(9, "x\ny"), [5, 6]);
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["one", "p1", "plain p2", "two", "three", "x", "y"]);
        assert_eq!((s.row(1).start, s.row(2).end), (7000, 5000));
    }

    #[test]
    fn override_blocks_stay() {
        assert_eq!(
            override_blocks(r"{\an8}Hi {note}there{\i1}!{\p1}m 0 0 l 1 1{\p0}"),
            r"{\an8}{\i1}{\p1}{\p0}"
        );
        assert_eq!(override_blocks("plain {open"), "");
    }

    #[test]
    fn diff_spans_outside_the_common_part() {
        assert_eq!(diff_spans("abc", "abc"), (vec![], vec![]));
        assert_eq!(
            diff_spans("kot ma", "kat mam"),
            (vec![1, 1], vec![1, 1, 6, 1])
        );
        // UTF-16 units: the emoji counts 2
        assert_eq!(diff_spans("😀a", "😀b"), (vec![2, 1], vec![2, 1]));
        assert_eq!(diff_spans("", "xy"), (vec![], vec![0, 2]));
    }

    #[test]
    fn column_info_lists_distinct_values() {
        let mut s = session(ASS);
        s.set_rows_field(&[1], "actor", "Ann");
        let ci = s.column_info();
        assert_eq!(
            (ci.styles, ci.actors, ci.effects),
            (vec!["Default".to_string()], vec!["Ann".to_string()], vec![])
        );
        assert!(!ci.any_layer && !ci.any_margin_v);
        // typing keeps the cached info, a column field drops it
        let mut r = s.row(1);
        r.text += "x";
        s.set_row(1, &r, "typing");
        assert_eq!(s.column_info().actors, ["Ann"]);
        r.actor = "Bob".into();
        s.set_row(1, &r, "typing");
        assert_eq!(s.column_info().actors, ["Bob"]);
    }

    #[test]
    fn rows_edit_undo_and_state() {
        let mut s = session(ASS);
        assert_eq!(s.row_count(), 3);
        let mut r = s.row(1);
        assert_eq!((r.text.as_str(), r.state), ("two", 0));
        r.text = "TWO".into();
        s.set_row(1, &r, "");
        assert_eq!((s.row(1).text.as_str(), s.row(1).state), ("TWO", 1));
        let path = std::env::temp_dir().join(format!("fsb-state-{}.ass", std::process::id()));
        s.save(path.to_str().unwrap()).unwrap();
        std::fs::remove_file(&path).ok();
        assert_eq!(s.row(1).state, 2);
        // back to the loaded text in a new entry: unchanged by value
        r.text = "two".into();
        s.set_row(1, &r, "");
        assert_eq!(s.row(1).state, 0);
        assert!(s.undo());
        assert_eq!(s.row(1).text, "TWO");
        assert!(s.undo());
        assert_eq!(s.row(1).text, "two");
        assert_eq!(s.move_rows(&[2], -2), vec![0]);
        assert_eq!(s.row(0).text, "three");
        // "three" and "two" with a gap, to the end
        assert_eq!(s.move_rows(&[0, 2], 9), vec![1, 2]);
        assert_eq!(s.row(0).text, "one");
    }

    #[test]
    fn paste_over_copies_only_chosen_fields() {
        let mut s = session(ASS);
        let before = s.row(1);
        let clip = "Dialogue: 5,0:00:09.00,0:00:10.00,Other,Bob,0,0,0,,new text\nplain";
        let rows = s.paste_over(&[1, 0], clip, 4 | 1024);
        assert_eq!(rows, [1, 0]);
        let r = s.row(1);
        assert_eq!(
            (r.start, r.end, r.text.as_str()),
            (9000, before.end, "new text")
        );
        assert_eq!(
            (r.layer, r.style.as_str()),
            (before.layer, before.style.as_str())
        );
        assert_eq!((s.row(0).start, s.row(0).text.as_str()), (0, "plain"));
    }
}
