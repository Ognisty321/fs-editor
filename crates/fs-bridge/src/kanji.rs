//! The kanji timer dialog's line pair (fs_core::kanji_timer) for the GUI.

use fs_core::kanji_timer::KanjiMatcher;

use crate::Session;
use crate::ffi::KanjiState;

#[derive(Default)]
pub struct KanjiTimer(KanjiMatcher);

pub fn new_kanji_timer() -> Box<KanjiTimer> {
    Box::default()
}

impl KanjiTimer {
    /// Start on the karaoke of `src_row` and the plain text of `dst_row`.
    pub fn set_lines(&mut self, session: &Session, src_row: usize, dst_row: usize) {
        let syls = session
            .syllables(src_row)
            .into_iter()
            .map(|s| (s.end - s.start, s.text))
            .collect();
        let dst = fs_core::tags::strip_tags(&session.row(dst_row).text);
        self.0 = KanjiMatcher::new(syls, &dst);
    }

    pub fn auto_match(&mut self) {
        self.0.auto_match();
    }
    pub fn accept(&mut self) -> bool {
        self.0.accept()
    }
    pub fn undo(&mut self) -> bool {
        self.0.undo()
    }
    /// `what`: 0 source, 1 destination; `grow` lengthens the selection.
    pub fn adjust(&mut self, what: u8, grow: bool) -> bool {
        match (what, grow) {
            (0, true) => self.0.increase_source(),
            (0, false) => self.0.decrease_source(),
            (_, true) => self.0.increase_destination(),
            (_, false) => self.0.decrease_destination(),
        }
    }
    pub fn output(&self) -> String {
        self.0.output()
    }
    pub fn state(&self) -> KanjiState {
        let groups = self.0.groups();
        let (remaining_src, remaining_dst) = self.0.remaining();
        KanjiState {
            group_src: groups.iter().map(|g| g.0.concat()).collect(),
            group_dst: groups.into_iter().map(|g| g.1).collect(),
            sel_src: self.0.source_selection(),
            rest_src: self.0.unmatched_source(),
            sel_dst: self.0.destination_selection(),
            rest_dst: self.0.unmatched_destination(),
            remaining_src,
            remaining_dst,
        }
    }
}
