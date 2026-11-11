//! Kainote's trees: runs of lines folded under a comment line that describes them, the state
//! kept in the lines' actor field.

use std::sync::Arc;

use fs_core::tlmode;
use fs_core::{Event, EventEntry, Time};

use super::Session;

// Kainote's trees: a comment line marked `[tree_description]` (its text describes the tree) folds
// the lines after it marked `[tree_closed]` or `[tree_opened]`, up to the first one without.
const TREE_DESCRIPTION: &str = "[tree_description]";

const TREE_OPENED: &str = "[tree_opened]";

const TREE_CLOSED: &str = "[tree_closed]";

impl Session {
    /// The tree mark of the row's line; in translator mode both halves of a pair carry it, so it
    /// stays when the mode is turned off (the translations are kept).
    fn row_tree(&self, row: usize) -> Option<&'static str> {
        let r = self.tl_row(row)?;
        tree_state(&self.ed.doc.event(r.main())?.actor)
    }

    /// Rows that closed trees fold away. A line marked closed with no description above it (the
    /// description was deleted) stays in view.
    pub fn tree_hidden(&self) -> Vec<usize> {
        let mut inside = false;
        let mut out = Vec::new();
        for r in 0..self.rows.len() {
            match self.row_tree(r) {
                Some(TREE_DESCRIPTION) => inside = true,
                Some(TREE_CLOSED) if inside => out.push(r),
                Some(TREE_OPENED | TREE_CLOSED) => {}
                _ => inside = false,
            }
        }
        out
    }

    /// The rows of the tree `row` describes; empty when it describes none.
    pub fn tree_members(&self, row: usize) -> Vec<usize> {
        if self.row_tree(row) != Some(TREE_DESCRIPTION) {
            return Vec::new();
        }
        (row + 1..self.rows.len())
            .take_while(|&r| matches!(self.row_tree(r), Some(TREE_OPENED | TREE_CLOSED)))
            .collect()
    }

    /// Kainote's "Make tree": each run of the rows (those in no tree yet) gets a comment line
    /// before it holding `description`, and folds closed. Returns the description rows.
    pub fn make_tree(&mut self, rows: &[usize], description: &str) -> Vec<usize> {
        let mut rows: Vec<usize> = rows
            .iter()
            .copied()
            .filter(|&r| r < self.rows.len() && self.row_tree(r).is_none())
            .collect();
        rows.sort_unstable();
        rows.dedup();
        let doc = &self.ed.doc;
        // before the run's first row: ahead of the original of a translator's pair
        let heads: Vec<(usize, Event)> = rows
            .chunk_by(|a, b| b - a == 1)
            .map(|run| {
                let r = self.rows[run[0]];
                let style = &doc.event(r.main()).unwrap().style;
                let mut ev = Event::dialogue(Time(0), Time(0), style, description);
                ev.kind = fs_core::model::EventKind::Comment;
                ev.actor = TREE_DESCRIPTION.into();
                (r.original, ev)
            })
            .collect();
        let evs = self.all_events(&rows);
        if heads.is_empty() {
            return Vec::new();
        }
        let made = self.edit("Make tree", move |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.actor = with_tree_state(&ev.actor, Some(TREE_CLOSED));
            }
            d.insert_events(heads)
        });
        self.rows_of_events(&made)
    }

    /// Kainote's tree "Add lines": the rows in no tree join the tree `head` describes, those above
    /// it at its start and those below at its end, in order, folded as it is. Returns the head's
    /// new row, -1 when no row joined.
    pub fn tree_add_lines(&mut self, head: usize, rows: &[usize]) -> i64 {
        if self.row_tree(head) != Some(TREE_DESCRIPTION) {
            return -1;
        }
        let members = self.tree_members(head);
        let state = members
            .first()
            .and_then(|&m| self.row_tree(m))
            .unwrap_or(TREE_OPENED);
        let mut adding: Vec<usize> = rows
            .iter()
            .copied()
            .filter(|&r| r < self.rows.len() && self.row_tree(r).is_none())
            .collect();
        adding.sort_unstable();
        adding.dedup();
        if adding.is_empty() {
            return -1;
        }
        let end = head + 1 + members.len(); // the first row after the tree
        let mut groups: Vec<Option<Vec<Arc<EventEntry>>>> =
            self.groups().into_iter().map(Some).collect();
        let (mut above, mut below) = (Vec::new(), Vec::new());
        for &r in &adding {
            let mut g = groups[r].take().unwrap();
            for e in &mut g {
                let ev = &mut Arc::make_mut(e).event;
                ev.actor = with_tree_state(&ev.actor, Some(state));
            }
            if r < head {
                above.push(g)
            } else {
                below.push(g)
            }
        }
        let new_head = head - above.len();
        let mut out = Vec::with_capacity(groups.len());
        for (r, g) in groups.into_iter().enumerate() {
            if r == end {
                out.append(&mut below);
            }
            out.extend(g);
            if r == head {
                out.append(&mut above);
            }
        }
        out.append(&mut below); // a tree that ends the file
        self.set_groups("Add lines to tree", out);
        new_head as i64
    }

    /// Open a closed tree, or close an open one (a click on its description).
    pub fn toggle_tree(&mut self, row: usize) {
        let members = self.tree_members(row);
        let Some(&first) = members.first() else {
            return;
        };
        let closed = self.row_tree(first) == Some(TREE_CLOSED);
        let evs = self.all_events(&members);
        let (to, desc) = if closed {
            (TREE_OPENED, "Open tree")
        } else {
            (TREE_CLOSED, "Close tree")
        };
        self.edit(desc, move |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.actor = with_tree_state(&ev.actor, Some(to));
            }
        });
    }

    /// Undo a tree: its lines lose the mark and the description line goes.
    pub fn remove_tree(&mut self, row: usize) {
        if self.row_tree(row) != Some(TREE_DESCRIPTION) {
            return;
        }
        let evs = self.all_events(&self.tree_members(row));
        let head = self.all_events(&[row]);
        self.edit("Remove tree", move |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.actor = with_tree_state(&ev.actor, None);
            }
            d.remove_events(&head);
        });
    }
}

fn tree_state(actor: &str) -> Option<&'static str> {
    if !actor.starts_with('[') {
        return None; // nearly every line: no need to look further
    }
    tlmode::split_actor(actor)
        .0
        .into_iter()
        .find(|p| p.starts_with("[tree_"))
}

/// `actor` with its tree mark set to `state` (first, where Kainote writes it) or taken off.
fn with_tree_state(actor: &str, state: Option<&'static str>) -> String {
    let (marks, name) = tlmode::split_actor(actor);
    let mut out: Vec<&str> = state.into_iter().collect();
    out.extend(marks.into_iter().filter(|p| !p.starts_with("[tree_")));
    tlmode::join_actor(&out, name)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn trees_as_kainote() {
        let mut s = session(ASS);
        // rows 0 and 2 are two runs: two trees, each with its description before it
        let heads = s.make_tree(&[2, 0], "intro");
        assert_eq!(heads, [0, 3]);
        assert_eq!(s.row_count(), 5);
        assert_eq!((s.row(0).text.as_str(), s.row(0).comment), ("intro", true));
        assert_eq!(s.row(1).markers, TREE_CLOSED);
        assert_eq!(s.tree_hidden(), [1, 4]);
        assert_eq!(s.tree_members(0), [1]);
        // a line already in a tree makes no new one
        assert!(s.make_tree(&[1], "x").is_empty());
        s.toggle_tree(3);
        assert_eq!(s.tree_hidden(), [1]);
        assert_eq!(s.row(4).markers, TREE_OPENED);
        // the actor stays the actor, other marks stay too
        let mut r = s.row(4);
        r.actor = "Ann".into();
        s.set_row(4, &r, "");
        s.remove_tree(3);
        assert_eq!(s.row_count(), 4);
        assert_eq!(
            (s.row(3).actor.as_str(), s.row(3).markers.as_str()),
            ("Ann", "")
        );
        // a closed line whose description went shows again
        s.delete_rows(&[0]);
        assert!(s.tree_hidden().is_empty());
        // lines join a tree: those above at its start, those below at its end, folded as it is
        let mut s = session(ASS);
        assert_eq!(s.make_tree(&[1], "mid"), [1]);
        assert_eq!(s.tree_add_lines(1, &[2, 0, 3]), 0); // row 2 is in the tree already
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["mid", "one", "two", "three"]);
        assert_eq!(s.tree_hidden(), [1, 2, 3]);
        assert_eq!(s.tree_add_lines(0, &[1]), -1);
        s.toggle_tree(0);
        assert!(s.tree_hidden().is_empty());
        // Kainote writes the tree mark before the bookmark
        assert_eq!(
            with_tree_state("[bookmark]Bob", Some(TREE_CLOSED)),
            "[tree_closed][bookmark]Bob"
        );
    }
}
