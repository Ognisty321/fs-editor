//! Undo/redo.
//!
//! The history stores complete document snapshots. Because sections and
//! individual styles/events are reference-counted (`Arc`) and copied on
//! write, a snapshot only costs one pointer per event of the section that
//! changed, so even scripts with tens of thousands of lines can keep a deep
//! history. The same model works for edits coming from the GUI, the CLI and
//! Lua automation (which may change anything).

use std::sync::Arc;

use crate::ass::{Document, EventId};

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

/// A document together with its undo history — one per editor tab.
#[derive(Debug, Clone)]
pub struct Editor {
    pub doc: Document,
    pub selection: Selection,
    undo: Vec<State>,
    redo: Vec<State>,
    /// Maximum number of undo steps kept.
    pub limit: usize,
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
        self.undo.push(State {
            description: description.to_string(),
            doc: before,
            selection: self.selection.clone(),
            amend_key: amend.map(str::to_string),
        });
        self.undo_gen.push(self.generation);
        if self.undo.len() > self.limit {
            self.undo.remove(0);
            self.undo_gen.remove(0);
        }
        self.generation = self.next_generation;
        self.next_generation += 1;
        r
    }

    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }

    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
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
        let st = self.undo.pop()?;
        let g = self.undo_gen.pop().unwrap();
        let diff = diff_events(&self.doc, &st.doc);
        let cur_doc = std::mem::replace(&mut self.doc, st.doc);
        let cur_sel = std::mem::replace(&mut self.selection, st.selection);
        self.redo.push(State {
            description: st.description,
            doc: cur_doc,
            selection: cur_sel,
            amend_key: None,
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
        self.undo.push(State {
            description: st.description,
            doc: cur_doc,
            selection: cur_sel,
            amend_key: None,
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
