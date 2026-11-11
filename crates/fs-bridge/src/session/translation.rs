//! Translator mode: original and translation as one row.

use std::path::Path;

use fs_core::tlmode;
use fs_formats::{Format, SaveOptions};

use super::Session;

impl Session {
    pub fn save_translation(&self, path: &str) -> Result<(), String> {
        let doc = tlmode::translation_only(&self.ed.doc);
        let opts = SaveOptions {
            encoding: self.encoding,
            bom: self.bom,
            ..Default::default()
        };
        fs_formats::save_file(&doc, Path::new(path), Format::Ass, &opts)
            .map(drop)
            .map_err(|e| format!("{path}: {e}"))
    }

    pub fn tl_enable(&mut self, on: bool) {
        self.edit(
            if on {
                "Translation mode on"
            } else {
                "Translation mode off"
            },
            |d| {
                if on {
                    tlmode::enable(d)
                } else {
                    tlmode::disable(d)
                }
            },
        );
    }

    pub fn set_doubtful(&mut self, rows: &[usize], on: bool) {
        let rows: Vec<usize> = rows.to_vec();
        let tl_rows = self.rows.clone();
        self.edit("Not confirmed", move |d| {
            // setting doubtful may insert an original event, so walk backwards
            for &r in rows.iter().rev() {
                if let Some(&row) = tl_rows.get(r) {
                    tlmode::set_doubtful(d, row, on);
                }
            }
        });
    }

    pub fn next_doubtful(&self, from: usize) -> i64 {
        tlmode::next_doubtful(&self.ed.doc, from).map_or(-1, |r| r as i64)
    }

    pub fn next_untranslated(&self, from: usize) -> i64 {
        tlmode::next_untranslated(&self.ed.doc, from).map_or(-1, |r| r as i64)
    }

    pub fn paste_translation(&mut self, first: usize, text: &str) -> usize {
        let lines: Vec<String> = text.lines().map(str::to_string).collect();
        self.edit("Paste translation", |d| {
            let refs: Vec<&str> = lines.iter().map(String::as_str).collect();
            tlmode::paste_translation(d, first, &refs)
        })
    }

    pub fn tl_shift(&mut self, first: usize, count: usize, op: u8) {
        use tlmode::Shift::*;
        let ops = [
            DeleteTranslation,
            InsertTranslation,
            JoinTranslation,
            DeleteOriginal,
            InsertOriginal,
            JoinOriginal,
        ];
        let Some(&op) = ops.get(op as usize) else {
            return;
        };
        self.edit("Move translation", |d| tlmode::shift(d, first, count, op));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::{FilterSpec, ShiftSpec};
    use crate::session::fixtures::{ASS, session};

    /// Translator mode: pairs join and split half by half, trees fold whole pairs.
    #[test]
    fn translator_pairs_join_split_and_fold() {
        let mut s = session(ASS);
        s.tl_enable(true);
        s.paste_translation(0, "jeden");
        s.paste_translation(2, "trzy");
        let view = |s: &Session| -> Vec<(String, String, i64, i64)> {
            (0..s.row_count())
                .map(|i| {
                    let r = s.row(i);
                    (r.original, r.text, r.start, r.end)
                })
                .collect()
        };
        let own = |o: &str, t: &str, a: i64, b: i64| (o.to_string(), t.to_string(), a, b);
        let words = |w: &[&str]| -> Vec<String> { w.iter().map(|s| s.to_string()).collect() };
        // an untranslated row in the middle: its translation is empty
        assert_eq!(s.join_rows(&[2, 0, 1], 0), 0);
        assert_eq!(view(&s), [own("one two three", "jeden trzy", 1000, 6000)]);
        assert_eq!(s.row_count(), 1);
        s.undo();
        // swapped rows keep their originals
        s.swap_rows(2, 0);
        assert_eq!(view(&s)[0], own("three", "trzy", 5000, 6000));
        assert_eq!(view(&s)[2], own("one", "jeden", 1000, 2000));
        s.swap_rows(1, 7);
        s.undo();
        // keep last; an untranslated first row becomes a pair
        assert_eq!(s.join_rows(&[1, 2], 2), 1);
        assert_eq!(view(&s)[1], own("three", "trzy", 3000, 6000));
        assert!(s.row(1).translated && s.row_count() == 2);
        s.undo();
        // split the translation at the cursor: the original stays whole on the first part
        assert_eq!(s.split_row(2, 3, 5500, false), 3);
        assert_eq!(
            view(&s)[2..],
            [own("three", "trz", 5000, 5500), own("", "y", 5500, 6000)]
        );
        s.undo();
        // or the original, keeping the times; an untranslated row splits its original only
        assert_eq!(s.split_row(2, 2, -2, true), 3);
        assert_eq!(
            view(&s)[2..],
            [own("th", "trzy", 5000, 6000), own("ree", "", 5000, 6000)]
        );
        assert_eq!(s.split_row(1, 1, -1, false), -1);
        s.undo();
        // words: the original stays with the first piece; frames: with every copy
        let made = s.explode_rows(
            &[2, 1],
            &[2, 2],
            &words(&["tr", "zy", "t", "wo"]),
            &[5000, 5500, 3000, 3500],
            &[5500, 6000, 3500, 4000],
        );
        assert_eq!(made, [1, 2, 3, 4]);
        assert_eq!(
            view(&s)[1..],
            [
                own("t", "", 3000, 3500),
                own("wo", "", 3500, 4000),
                own("three", "tr", 5000, 5500),
                own("", "zy", 5500, 6000)
            ]
        );
        s.undo();
        s.explode_rows(
            &[2],
            &[2],
            &words(&["trzy", "trzy"]),
            &[5000, 5500],
            &[5500, 6000],
        );
        assert_eq!(
            view(&s)[2..],
            [
                own("three", "trzy", 5000, 5500),
                own("three", "trzy", 5500, 6000)
            ]
        );
        s.undo();
        // a tree before the pair's original; both halves carry the mark
        assert_eq!(s.make_tree(&[2], "end"), [2]);
        assert_eq!(s.tree_hidden(), [3]);
        assert_eq!(
            (s.row(3).original.as_str(), s.row(3).text.as_str()),
            ("three", "trzy")
        );
        s.toggle_tree(2);
        assert!(s.tree_hidden().is_empty());
        s.toggle_tree(2);
        s.tl_enable(false);
        assert_eq!(s.tree_hidden(), [3]); // the translation kept the mark
        s.undo();
        s.remove_tree(2);
        assert_eq!(s.row_count(), 3);
        assert_eq!(s.row(2).markers, "");
    }

    #[test]
    fn translation_rows_and_shift() {
        let mut s = session(ASS);
        s.tl_enable(true);
        let mut r = s.row(0);
        r.text = "jeden".into();
        s.set_row(0, &r, "");
        assert_eq!(s.row_count(), 3);
        let r = s.row(0);
        assert_eq!(
            (r.original.as_str(), r.text.as_str(), r.translated),
            ("one", "jeden", true)
        );
        let spec = ShiftSpec {
            amount: 500,
            lines: 0,
            ..Default::default()
        };
        s.shift(&spec, &[]);
        assert_eq!((s.row(0).start, s.row(2).start), (1500, 5500));
        // both halves of the pair moved
        assert!(
            s.ed.doc
                .events()
                .iter()
                .all(|e| e.event.start.ms() % 1000 == 500)
        );
        s.set_doubtful(&[1], true);
        assert!(s.row(1).doubtful);
        assert_eq!(
            s.filter(&FilterSpec {
                only_doubtful: true,
                ..Default::default()
            }),
            vec![1]
        );
    }
}
