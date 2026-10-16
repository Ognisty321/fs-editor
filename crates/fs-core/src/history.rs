//! Undo/redo.
//!
//! The history stores complete document snapshots. Because sections and
//! individual styles/events are reference-counted (`Arc`) and copied on
//! write, a snapshot shares every line an edit did not touch: it costs one
//! pointer per event plus the old versions of the lines that changed. The
//! history is limited by the memory those take (a "shift all" on 50k lines
//! keeps 50k old lines), with Aegisub's default of 20 steps always kept. The
//! same model works for edits coming from the GUI, the CLI and Lua
//! automation (which may change anything).

use std::collections::HashSet;
use std::sync::Arc;

use crate::ass::{Document, EventEntry, EventId, Section, SectionBody};

/// Grid selection to restore after undo/redo.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Selection {
    pub active: Option<EventId>,
    pub selected: Vec<EventId>,
}

#[derive(Debug, Clone)]
struct State {
    description: String,
    doc: Document,
    selection: Selection,
    /// Edits with the same amend key are merged into one undo step (typing
    /// in the edit box, dragging a time in the grid).
    amend_key: Option<String>,
    /// Memory only this snapshot keeps alive (`snapshot_bytes`).
    bytes: usize,
}

/// How rows changed between two document states (for incremental GUI
/// refresh).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EventDiff {
    None,
    /// Same rows in the same order; only these indices changed.
    Rows(Vec<usize>),
    /// Rows were inserted, removed or reordered.
    Reset,
}

pub fn diff_events(old: &Document, new: &Document) -> EventDiff {
    let (a, b) = (old.events(), new.events());
    if a.len() != b.len() || a.iter().zip(b).any(|(x, y)| x.id != y.id) {
        return EventDiff::Reset;
    }
    let rows: Vec<usize> = a
        .iter()
        .zip(b)
        .enumerate()
        .filter(|(_, (x, y))| !Arc::ptr_eq(x, y) && x != y)
        .map(|(i, _)| i)
        .collect();
    if rows.is_empty() {
        EventDiff::None
    } else {
        EventDiff::Rows(rows)
    }
}

/// Undo steps kept whatever memory they take (Aegisub's default undo levels).
const MIN_STEPS: usize = 20;

fn entry_bytes(e: &EventEntry, shares_orig: bool) -> usize {
    let ev = &e.event;
    let strings = ev.text.capacity() + ev.style.capacity() + ev.actor.capacity();
    let extra: usize = ev
        .extra
        .iter()
        .map(|(k, v)| k.capacity() + v.capacity() + 48)
        .sum();
    let leading: usize = e.leading.iter().map(|l| l.text.capacity() + 32).sum();
    let orig = if shares_orig {
        0
    } else {
        e.orig.as_ref().map_or(0, |o| o.len() + 16)
    };
    // the Arc's counts and the entry itself
    16 + size_of::<EventEntry>() + strings + ev.effect.capacity() + extra + leading + orig
}

/// What a section of `before` holds that `after` does not share.
fn section_bytes(s: &Section, after: &Document) -> usize {
    match &s.body {
        SectionBody::Events(sec) => {
            let new = after.events();
            // pointers of `after`, gathered only when lines moved (sort, insert, delete)
            let mut moved: Option<HashSet<*const EventEntry>> = None;
            let mut bytes = size_of::<Arc<EventEntry>>() * sec.entries.len();
            for (i, e) in sec.entries.iter().enumerate() {
                match new.get(i) {
                    Some(n) if Arc::ptr_eq(e, n) => {}
                    Some(n) if n.id == e.id => {
                        let shares = match (&e.orig, &n.orig) {
                            (Some(a), Some(b)) => Arc::ptr_eq(a, b),
                            _ => false,
                        };
                        bytes += entry_bytes(e, shares);
                    }
                    _ => {
                        let set =
                            moved.get_or_insert_with(|| new.iter().map(Arc::as_ptr).collect());
                        if !set.contains(&Arc::as_ptr(e)) {
                            bytes += entry_bytes(e, false);
                        }
                    }
                }
            }
            bytes
        }
        SectionBody::Styles(sec) => sec
            .entries
            .iter()
            .map(|e| 16 + size_of_val(&**e) + e.style.name.len() + e.style.font.len())
            .sum(),
        SectionBody::Info(sec) => (sec.entries.iter())
            .map(|e| 64 + e.key.len() + e.value.len())
            .sum(),
        SectionBody::Raw(lines) => lines.iter().map(|l| l.text.capacity() + 32).sum(),
    }
}

/// Memory the snapshot `before` keeps alive on its own once the document is `after`.
fn snapshot_bytes(before: &Document, after: &Document) -> usize {
    (before.sections.iter())
        .filter(|s| !after.sections.iter().any(|t| Arc::ptr_eq(s, t)))
        .map(|s| section_bytes(s, after))
        .sum()
}

/// A document together with its undo history — one per editor tab.
#[derive(Debug, Clone)]
pub struct Editor {
    pub doc: Document,
    pub selection: Selection,
    undo: Vec<State>,
    redo: Vec<State>,
    /// Maximum number of undo steps kept.
    pub limit: usize,
    /// Memory the undo steps may keep beyond the document, in bytes (`MIN_STEPS` are kept anyway).
    pub budget: usize,
    /// Memory the undo steps keep now.
    undo_bytes: usize,
    generation: u64,
    saved_generation: u64,
    /// Generation number for each undo state, parallel to `undo`.
    undo_gen: Vec<u64>,
    redo_gen: Vec<u64>,
    next_generation: u64,
}

impl Editor {
    pub fn new(doc: Document) -> Self {
        Editor {
            doc,
            selection: Selection::default(),
            undo: Vec::new(),
            redo: Vec::new(),
            limit: 500,
            budget: 512 << 20,
            undo_bytes: 0,
            generation: 0,
            saved_generation: 0,
            undo_gen: Vec::new(),
            redo_gen: Vec::new(),
            next_generation: 1,
        }
    }

    /// Run an edit as one undoable step. Returns the closure's result.
    /// If the document did not change, no undo step is recorded.
    pub fn edit<R>(&mut self, description: &str, f: impl FnOnce(&mut Document) -> R) -> R {
        self.edit_inner(description, None, f)
    }

    /// Like [`edit`](Self::edit) but merged with the previous step if that
    /// step had the same `amend_key` and nothing was undone in between.
    pub fn edit_amend<R>(
        &mut self,
        description: &str,
        amend_key: &str,
        f: impl FnOnce(&mut Document) -> R,
    ) -> R {
        self.edit_inner(description, Some(amend_key), f)
    }

    fn edit_inner<R>(
        &mut self,
        description: &str,
        amend: Option<&str>,
        f: impl FnOnce(&mut Document) -> R,
    ) -> R {
        let merge = amend.is_some()
            && self.redo.is_empty()
            && self
                .undo
                .last()
                .is_some_and(|s| s.amend_key.as_deref() == amend)
            && self.saved_generation != self.generation;
        if merge {
            // The step already holds the state before the first merged edit, so no
            // snapshot: the sections stay unshared and typing edits them in place.
            // The document is already modified, so a no-op here changes nothing visible.
            let r = f(&mut self.doc);
            self.generation = self.next_generation;
            self.next_generation += 1;
            return r;
        }
        let before = self.doc.clone();
        let r = f(&mut self.doc);
        if self.doc == before {
            return r;
        }
        self.redo.clear();
        self.redo_gen.clear();
        let bytes = snapshot_bytes(&before, &self.doc);
        self.push_undo(State {
            description: description.to_string(),
            doc: before,
            selection: self.selection.clone(),
            amend_key: amend.map(str::to_string),
            bytes,
        });
        self.undo_gen.push(self.generation);
        self.trim();
        self.generation = self.next_generation;
        self.next_generation += 1;
        r
    }

    fn push_undo(&mut self, st: State) {
        self.undo_bytes += st.bytes;
        self.undo.push(st);
    }

    fn pop_undo(&mut self) -> Option<State> {
        let st = self.undo.pop()?;
        self.undo_bytes -= st.bytes;
        Some(st)
    }

    /// Drops the oldest steps over the step limit or the memory budget.
    fn trim(&mut self) {
        let mut n = 0;
        let mut bytes = self.undo_bytes;
        while self.undo.len() - n > self.limit
            || (self.undo.len() - n > MIN_STEPS && bytes > self.budget)
        {
            bytes -= self.undo[n].bytes;
            n += 1;
        }
        if n > 0 {
            self.undo.drain(..n);
            self.undo_gen.drain(..n);
            self.undo_bytes = bytes;
        }
    }

    /// Memory the undo steps keep alive besides the document, in bytes.
    pub fn undo_memory(&self) -> usize {
        self.undo_bytes
    }

    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
    }

    /// Changes with every step made, undone or redone.
    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn undo_description(&self) -> Option<&str> {
        self.undo.last().map(|s| s.description.as_str())
    }

    pub fn redo_description(&self) -> Option<&str> {
        self.redo.last().map(|s| s.description.as_str())
    }

    /// Undo descriptions, most recent first (for the history list).
    pub fn undo_list(&self) -> impl Iterator<Item = &str> {
        self.undo.iter().rev().map(|s| s.description.as_str())
    }

    pub fn undo(&mut self) -> Option<EventDiff> {
        let st = self.pop_undo()?;
        let g = self.undo_gen.pop().unwrap();
        let diff = diff_events(&self.doc, &st.doc);
        let cur_doc = std::mem::replace(&mut self.doc, st.doc);
        let cur_sel = std::mem::replace(&mut self.selection, st.selection);
        self.redo.push(State {
            description: st.description,
            doc: cur_doc,
            selection: cur_sel,
            amend_key: None,
            bytes: st.bytes, // the other side of the same change
        });
        self.redo_gen.push(self.generation);
        self.generation = g;
        Some(diff)
    }

    pub fn redo(&mut self) -> Option<EventDiff> {
        let st = self.redo.pop()?;
        let g = self.redo_gen.pop().unwrap();
        let diff = diff_events(&self.doc, &st.doc);
        let cur_doc = std::mem::replace(&mut self.doc, st.doc);
        let cur_sel = std::mem::replace(&mut self.selection, st.selection);
        self.push_undo(State {
            description: st.description,
            doc: cur_doc,
            selection: cur_sel,
            amend_key: None,
            bytes: st.bytes,
        });
        self.undo_gen.push(self.generation);
        self.generation = g;
        Some(diff)
    }

    /// Whether the document differs from the last saved state.
    pub fn is_modified(&self) -> bool {
        self.generation != self.saved_generation
    }

    pub fn mark_saved(&mut self) {
        self.saved_generation = self.generation;
    }

    /// Modified until saved, whatever is undone (a recovered copy).
    pub fn mark_unsaved(&mut self) {
        self.saved_generation = u64::MAX;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::Event;
    use crate::time::Time;

    fn editor() -> Editor {
        Editor::new(Document::parse(
            "[Script Info]\nScriptType: v4.00+\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,a\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,b\n",
        ))
    }

    #[test]
    fn undo_redo() {
        let mut e = editor();
        let orig = e.doc.to_ass_string();
        assert!(!e.is_modified());
        e.edit("change text", |d| d.event_mut(0).unwrap().text = "x".into());
        assert!(e.is_modified());
        e.edit("insert", |d| {
            d.insert_event(1, Event::dialogue(Time(0), Time(1), "Default", "new"));
        });
        assert_eq!(e.doc.event_count(), 3);
        assert_eq!(e.undo(), Some(EventDiff::Reset));
        assert_eq!(e.doc.event_count(), 2);
        assert_eq!(e.undo(), Some(EventDiff::Rows(vec![0])));
        assert_eq!(e.doc.to_ass_string(), orig);
        assert!(!e.is_modified());
        assert!(e.undo().is_none());
        e.redo();
        e.redo();
        assert_eq!(e.doc.event_count(), 3);
        assert_eq!(e.doc.event(0).unwrap().text, "x");
    }

    #[test]
    fn no_op_edits_are_not_recorded() {
        let mut e = editor();
        e.edit("nothing", |_| {});
        assert!(!e.can_undo());
    }

    #[test]
    fn history_is_limited_by_memory() {
        let mut text = String::from(
            "[Script Info]\nScriptType: v4.00+\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
        );
        for i in 0..2_000 {
            text += &format!(
                "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{}\n",
                "x".repeat(400 + i % 7)
            );
        }
        let mut e = Editor::new(Document::parse(&text));
        e.budget = 40 << 20;
        let shift_all = |d: &mut Document| {
            for i in 0..d.event_count() {
                d.event_mut(i).unwrap().start.0 += 10;
            }
        };
        e.edit("shift", shift_all);
        // the old lines and the pointers, the shared original text not counted twice
        let one = e.undo_memory();
        assert!((2_000 * 500..2_000 * 1_000).contains(&one), "{one}");
        for _ in 0..60 {
            e.edit("shift", shift_all);
        }
        let n = e.undo.len();
        assert!(
            e.undo_memory() <= e.budget && n > MIN_STEPS && n < 61,
            "{n}"
        );
        // a one-line edit costs about one line
        e.edit("one", |d| d.event_mut(5).unwrap().text.push('!'));
        let last = e.undo.last().unwrap().bytes;
        assert!(last < 2_000 * 8 + 2_000, "{last}");
        // Aegisub's 20 steps stay, however large
        e.budget = 0;
        e.edit("shift", shift_all);
        assert_eq!(e.undo.len(), MIN_STEPS);
        // undo and redo carry their share
        let before = e.undo_memory();
        e.undo();
        e.redo();
        assert_eq!(e.undo_memory(), before);
    }

    #[test]
    fn sorting_shares_every_line() {
        let mut e = editor();
        e.edit("reverse", |d| {
            let sec = d.ensure_events();
            sec.entries.reverse();
        });
        assert_eq!(e.undo_memory(), 2 * size_of::<Arc<EventEntry>>());
    }

    #[test]
    fn amend_merges() {
        let mut e = editor();
        for c in ["h", "he", "hel"] {
            e.edit_amend("typing", "text:0", |d| {
                d.event_mut(0).unwrap().text = c.into()
            });
        }
        assert_eq!(e.undo_list().count(), 1);
        e.undo();
        assert_eq!(e.doc.event(0).unwrap().text, "a");
    }

    #[test]
    fn save_point() {
        let mut e = editor();
        e.edit("a", |d| d.event_mut(0).unwrap().text = "1".into());
        e.mark_saved();
        e.edit("b", |d| d.event_mut(0).unwrap().text = "2".into());
        assert!(e.is_modified());
        e.undo();
        assert!(!e.is_modified());
        e.undo();
        assert!(e.is_modified());
        e.redo();
        assert!(!e.is_modified());
    }

    #[test]
    fn redo_cleared_by_new_edit() {
        let mut e = editor();
        e.edit("a", |d| d.event_mut(0).unwrap().text = "1".into());
        e.undo();
        assert!(e.can_redo());
        e.edit("b", |d| d.event_mut(1).unwrap().text = "2".into());
        assert!(!e.can_redo());
    }

    #[test]
    fn snapshots_share_unchanged_events() {
        let mut e = editor();
        let before = e.doc.clone();
        e.edit("a", |d| d.event_mut(0).unwrap().text = "1".into());
        assert!(!Arc::ptr_eq(&before.events()[0], &e.doc.events()[0]));
        assert!(Arc::ptr_eq(&before.events()[1], &e.doc.events()[1]));
    }
}
