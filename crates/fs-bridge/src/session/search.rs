//! The grid's filter, Find and Replace, the search results' ticked replacements and the
//! multireplacer's rules.

use fs_core::ops::{self, Field, Rows, TextMatcher};
use fs_core::tlmode;

use super::Session;
use crate::ffi::{FilterSpec, FindHit, FindMatch, FindSpec};

impl Session {
    /// Rows the grid shows. Reads the events directly: building a `Row` per
    /// line (text copies, CPS) made this the slowest step of an edit.
    pub fn filter(&self, spec: &FilterSpec) -> Vec<usize> {
        let doc = &self.ed.doc;
        let mut hide_rows = spec.hide_rows.clone();
        hide_rows.sort_unstable();
        (0..self.rows.len())
            .filter(|&i| {
                let r = self.rows[i];
                let ev = &doc.events()[r.main()].event;
                let untranslated = || {
                    let text = if self.tl {
                        tlmode::translation_text(doc, r)
                    } else {
                        &ev.text
                    };
                    r.translation.is_some() && !text.trim().is_empty()
                };
                let hidden = spec.hide_styles.contains(&ev.style)
                    || hide_rows.binary_search(&i).is_ok()
                    || (spec.hide_comments && ev.is_comment())
                    || (spec.only_doubtful && !tlmode::is_doubtful(&ev.effect))
                    || (spec.only_untranslated && untranslated());
                hidden == spec.invert
            })
            .collect()
    }

    fn matcher(spec: &FindSpec) -> Result<TextMatcher, String> {
        // shortcut: with Skip text the start and end are each block's; the line's if people
        // combine the two
        let anchored = spec.at_start || spec.at_end;
        let pattern = if anchored {
            ops::anchored(&spec.pattern, spec.regex, spec.at_start, spec.at_end)
        } else {
            spec.pattern.clone()
        };
        let mut m = TextMatcher::new(
            &pattern,
            spec.regex || anchored,
            spec.match_case,
            field_of(spec.field),
        )
        .map_err(|e| e.to_string())?;
        m.strip_tags = spec.skip_tags;
        m.only_tags = spec.skip_text;
        m.is_regex = spec.regex;
        Ok(m)
    }

    /// Find next and Replace have something to look for: Aegisub does nothing for an empty
    /// search, Kainote's start / end of text find the field's ends.
    fn searchable(spec: &FindSpec) -> bool {
        !spec.pattern.is_empty() || spec.at_start || spec.at_end
    }

    /// The events row `r` is searched in, with whether each is the original: the text of both
    /// halves of a translation row, the original first (Kainote), else the line.
    fn search_events(&self, spec: &FindSpec, r: usize) -> impl Iterator<Item = (usize, bool)> {
        let row = self.rows[r];
        let both = self.tl && field_of(spec.field) == Field::Text;
        let first = if both {
            (row.original, true)
        } else {
            (row.main(), false)
        };
        std::iter::once(first).chain(row.translation.filter(|_| both).map(|t| (t, false)))
    }

    /// Whether row `r` is searched for `spec` (comments) and its field matches.
    fn row_hit(&self, m: &TextMatcher, spec: &FindSpec, r: usize) -> bool {
        let ev = |e| self.ed.doc.event(e).unwrap();
        (spec.include_comments || !ev(self.rows[r].main()).is_comment())
            && self.search_events(spec, r).any(|(e, _)| m.is_match(ev(e)))
    }

    pub fn find_next(
        &self,
        spec: &FindSpec,
        row: usize,
        original: bool,
        pos: usize,
    ) -> Result<FindHit, String> {
        let m = Self::matcher(spec)?;
        let n = self.rows.len();
        let none = FindHit {
            row: -1,
            ..Default::default()
        };
        if n == 0 || !Self::searchable(spec) {
            return Ok(none);
        }
        // the rows to search, marked once ("from selected" passes most of the file)
        let mut only = vec![spec.rows.is_empty(); n];
        for &r in spec.rows.iter().filter(|&&r| r < n) {
            only[r] = true;
        }
        // on from `pos`, through the other rows, and around to the start of `row` itself
        for k in 0..=n {
            let r = (row.min(n - 1) + k) % n;
            if !only[r]
                || !(spec.include_comments
                    || !self.ed.doc.event(self.rows[r].main()).unwrap().is_comment())
            {
                continue;
            }
            for (e, orig) in self.search_events(spec, r) {
                if k == 0 && orig && !original {
                    continue;
                }
                let ev = self.ed.doc.event(e).unwrap();
                let text = m.field_value(ev);
                let from = if k == 0 && orig == original {
                    byte_at(text, pos)
                } else {
                    0
                };
                // most rows have no match, and asking whether is the quickest
                if from == 0 && !m.is_match(ev) {
                    continue;
                }
                if let Some(at) = m.find_at(text, from) {
                    let start = utf16_len(&text[..at.start]);
                    return Ok(FindHit {
                        row: r as i64,
                        original: orig,
                        start,
                        end: start + utf16_len(&text[at]),
                    });
                }
            }
        }
        Ok(none)
    }

    pub fn replace_hit(&mut self, spec: &FindSpec, hit: &FindHit) -> Result<i64, String> {
        let m = Self::matcher(spec)?;
        if !Self::searchable(spec) || hit.row < 0 || hit.row as usize >= self.rows.len() {
            return Ok(-1);
        }
        let Some((e, _)) = self
            .search_events(spec, hit.row as usize)
            .find(|&(_, o)| o == hit.original)
        else {
            return Ok(-1);
        };
        let text = m.field_value(self.ed.doc.event(e).unwrap());
        let at = byte_at(text, hit.start)..byte_at(text, hit.end);
        let r = ops::Replace {
            matcher: m,
            replacement: spec.replacement.clone(),
            first_only: true,
            case: ops::CaseRule::Written,
        };
        let Some((new, end)) = r.replace_one(text, at) else {
            return Ok(-1);
        };
        let end = utf16_len(&new[..end]) as i64;
        self.edit("Replace", |d| {
            r.matcher.set_field(d.event_mut(e).unwrap(), new)
        });
        Ok(end)
    }

    /// Kainote's Replace in the search results: the matches ticked, each where it was found, in
    /// one undo step; a field whose text changed since the search is left as it is. Returns how
    /// many were replaced.
    pub fn replace_matches(
        &mut self,
        spec: &FindSpec,
        matches: &[FindMatch],
    ) -> Result<usize, String> {
        self.replace_ticked(std::slice::from_ref(spec), matches, &[], "Replace")
    }

    /// Kainote's multireplacer results: each match ticked replaced by its rule, `specs[rules[i]]`
    /// for `matches[i]`, in the case the rule asks for; otherwise as [`Self::replace_matches`].
    pub fn replace_rule_matches(
        &mut self,
        specs: &[FindSpec],
        matches: &[FindMatch],
        rules: &[usize],
    ) -> Result<usize, String> {
        self.replace_ticked(specs, matches, rules, "Correct minor errors")
    }

    /// `rules` empty: every match by `specs[0]`, its replacement as written.
    fn replace_ticked(
        &mut self,
        specs: &[FindSpec],
        matches: &[FindMatch],
        rules: &[usize],
        step: &str,
    ) -> Result<usize, String> {
        let reps = specs
            .iter()
            .map(|s| {
                Ok(ops::Replace {
                    matcher: Self::matcher(s)?,
                    replacement: s.replacement.clone(),
                    first_only: true,
                    case: if rules.is_empty() {
                        ops::CaseRule::Written
                    } else {
                        Self::rule_case(s)
                    },
                })
            })
            .collect::<Result<Vec<_>, String>>()?;
        // field by field, the last match first: the earlier ones keep their places
        let mut todo: Vec<(&FindMatch, usize)> = matches
            .iter()
            .enumerate()
            .map(|(i, x)| (x, rules.get(i).copied().unwrap_or(0)))
            .filter(|&(x, r)| {
                x.row < self.rows.len() && r < specs.len() && Self::searchable(&specs[r])
            })
            .collect();
        let key = |(x, r): &(&FindMatch, usize)| (x.row, x.original, specs[*r].field);
        todo.sort_by(|a, b| (key(a), b.0.start).cmp(&(key(b), a.0.start)));
        todo.dedup_by(|a, b| (key(a), a.0.start) == (key(b), b.0.start));
        let mut edits: Vec<(usize, String, usize)> = Vec::new();
        let mut count = 0;
        for field in todo.chunk_by(|a, b| key(a) == key(b)) {
            let (first, rule) = field[0];
            let Some((e, _)) = self
                .search_events(&specs[rule], first.row)
                .find(|&(_, o)| o == first.original)
            else {
                continue;
            };
            let text = reps[rule]
                .matcher
                .field_value(self.ed.doc.event(e).unwrap());
            if text != first.text {
                continue;
            }
            // each match as the search found it in the field's text (its groups, its context), the
            // last first; one overlapping a match replaced after it is left
            let mut new = text.to_string();
            let mut before = usize::MAX;
            for &(x, r) in field {
                let at = byte_at(text, x.start)..byte_at(text, x.end);
                if at.end > before {
                    continue;
                }
                if let Some((t, end)) = reps[r].replace_one(text, at.clone()) {
                    new.replace_range(at.clone(), &t[at.start..end]);
                    before = at.start;
                    count += 1;
                }
            }
            if new != text {
                edits.push((e, new, rule));
            }
        }
        if !edits.is_empty() {
            self.edit(step, |d| {
                for (e, new, rule) in edits {
                    reps[rule].matcher.set_field(d.event_mut(e).unwrap(), new);
                }
            });
        }
        Ok(count)
    }

    /// The case a multireplacer rule writes its replacement in ([`FindSpec::case_rule`]).
    fn rule_case(spec: &FindSpec) -> ops::CaseRule {
        match spec.case_rule {
            1 => ops::CaseRule::Follow,
            2 => ops::CaseRule::Written,
            3 => ops::CaseRule::Lower,
            4 => ops::CaseRule::Upper,
            _ if spec.match_case => ops::CaseRule::Written,
            _ => ops::CaseRule::Follow,
        }
    }

    /// Every row whose field matches, in order (Kainote's Find all).
    pub fn find_all(&self, spec: &FindSpec) -> Result<Vec<usize>, String> {
        let m = Self::matcher(spec)?;
        let n = self.rows.len();
        Ok(if spec.rows.is_empty() {
            (0..n).filter(|&r| self.row_hit(&m, spec, r)).collect()
        } else {
            let mut rows: Vec<usize> = spec.rows.iter().copied().filter(|&r| r < n).collect();
            rows.sort_unstable();
            rows.dedup();
            rows.retain(|&r| self.row_hit(&m, spec, r));
            rows
        })
    }

    pub fn find_matches(&self, spec: &FindSpec, max: usize) -> Result<Vec<FindMatch>, String> {
        let m = Self::matcher(spec)?;
        let mut out = Vec::new();
        for r in self.find_all(spec)? {
            for (e, original) in self.search_events(spec, r) {
                let text = m.field_value(self.ed.doc.event(e).unwrap());
                let mut from = 0;
                while let Some(at) = m.find_at(text, from) {
                    if out.len() == max {
                        return Ok(out);
                    }
                    let start = utf16_len(&text[..at.start]);
                    out.push(FindMatch {
                        row: r,
                        original,
                        start,
                        end: start + utf16_len(&text[at.clone()]),
                        text: text.into(),
                    });
                    // an empty match (^, \b) is passed by a character
                    from = match text[at.end..].chars().next() {
                        Some(c) if at.is_empty() => at.end + c.len_utf8(),
                        None if at.is_empty() => break,
                        _ => at.end,
                    };
                }
            }
        }
        Ok(out)
    }

    /// The events `spec` replaces in and how.
    fn replace_job(
        &self,
        spec: &FindSpec,
        case: ops::CaseRule,
    ) -> Result<(Rows, ops::Replace), String> {
        let m = Self::matcher(spec)?;
        let n = if Self::searchable(spec) {
            self.rows.len()
        } else {
            0
        };
        let rows: Vec<usize> = if spec.rows.is_empty() {
            (0..n).collect()
        } else {
            spec.rows.iter().copied().filter(|&r| r < n).collect()
        };
        let doc = &self.ed.doc;
        let evs: Vec<usize> = rows
            .into_iter()
            .filter(|&r| {
                spec.include_comments || !doc.event(self.rows[r].main()).unwrap().is_comment()
            })
            .flat_map(|r| self.search_events(spec, r))
            .map(|(e, _)| e)
            .collect();
        let r = ops::Replace {
            matcher: m,
            replacement: spec.replacement.clone(),
            first_only: false,
            case,
        };
        Ok((Rows::Selected(evs), r))
    }

    pub fn replace_all(&mut self, spec: &FindSpec) -> Result<usize, String> {
        let (rows, r) = self.replace_job(spec, ops::CaseRule::Written)?;
        Ok(self.edit("Replace", |d| ops::replace(d, &rows, &r)))
    }

    /// Kainote's minor error corrector: every rule in turn, one undo step. A rule writes its
    /// replacement in the case it asks for; by default one that ignores case in the case of
    /// each match ("Sie" -> "Się").
    pub fn replace_rules(&mut self, specs: &[FindSpec]) -> Result<usize, String> {
        let jobs = specs
            .iter()
            .map(|s| {
                self.replace_job(s, Self::rule_case(s))
                    .map_err(|e| format!("{}: {e}", s.pattern))
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(self.edit("Correct minor errors", |d| {
            jobs.iter().map(|(rows, r)| ops::replace(d, rows, r)).sum()
        }))
    }
}

fn field_of(code: u8) -> Field {
    match code {
        1 => Field::Style,
        2 => Field::Actor,
        3 => Field::Effect,
        _ => Field::Text,
    }
}

/// Byte offset of UTF-16 position `pos` in `s` (Qt's), its end past it.
fn byte_at(s: &str, pos: usize) -> usize {
    let mut u = 0;
    for (i, c) in s.char_indices() {
        if u >= pos {
            return i;
        }
        u += c.len_utf16();
    }
    s.len()
}

fn utf16_len(s: &str) -> usize {
    s.chars().map(char::len_utf16).sum()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn filter_matches_the_row_fields() {
        let mut s = session(ASS);
        s.set_rows_field(&[0], "comment", "1");
        s.set_rows_field(&[2], "style", "Other");
        s.tl_enable(true);
        s.paste_translation(1, "przekład");
        s.set_doubtful(&[1], true);
        // the old definition, row by row
        let oracle = |s: &Session, spec: &FilterSpec| -> Vec<usize> {
            (0..s.row_count())
                .filter(|&i| {
                    let r = s.row(i);
                    let hidden = spec.hide_styles.contains(&r.style)
                        || spec.hide_rows.contains(&i)
                        || (spec.hide_comments && r.comment)
                        || (spec.only_doubtful && !r.doubtful)
                        || (spec.only_untranslated && r.translated && !r.text.trim().is_empty());
                    hidden == spec.invert
                })
                .collect()
        };
        let specs = [
            FilterSpec {
                hide_comments: true,
                ..Default::default()
            },
            FilterSpec {
                hide_styles: vec!["Other".into()],
                ..Default::default()
            },
            FilterSpec {
                hide_rows: vec![2, 0],
                invert: true,
                ..Default::default()
            },
            FilterSpec {
                only_doubtful: true,
                ..Default::default()
            },
            FilterSpec {
                only_untranslated: true,
                ..Default::default()
            },
        ];
        for spec in &specs {
            assert_eq!(s.filter(spec), oracle(&s, spec), "{spec:?}");
        }
        assert!(specs.iter().any(|sp| s.filter(sp).len() < s.row_count()));
    }

    #[test]
    fn find_all_rows() {
        let s = session(ASS);
        let spec = |pattern: &str, rows: Vec<usize>| FindSpec {
            pattern: pattern.into(),
            rows,
            ..Default::default()
        };
        assert_eq!(s.find_all(&spec("o", vec![])), Ok(vec![0, 1]));
        assert_eq!(s.find_all(&spec("e", vec![2, 0, 2, 9])), Ok(vec![0, 2]));
        assert_eq!(s.find_all(&spec("x", vec![])), Ok(vec![]));
        let at = |pattern: &str, regex, max| {
            let spec = FindSpec {
                regex,
                ..spec(pattern, vec![])
            };
            (s.find_matches(&spec, max).unwrap().iter())
                .map(|m| (m.row, m.start, m.end))
                .collect::<Vec<_>>()
        };
        assert_eq!(at("e", false, 9), [(0, 2, 3), (2, 3, 4), (2, 4, 5)]);
        assert_eq!(at("e", false, 2), [(0, 2, 3), (2, 3, 4)]);
        assert_eq!(at("$", true, 9), [(0, 3, 3), (1, 3, 3), (2, 5, 5)]);
    }

    #[test]
    fn find_next_and_replace_one_match() {
        let mut s = session(ASS);
        let spec = |pattern: &str, replacement: &str, rows: Vec<usize>| FindSpec {
            pattern: pattern.into(),
            replacement: replacement.into(),
            rows,
            match_case: true,
            ..Default::default()
        };
        let hit = |row, start, end| FindHit {
            row,
            original: false,
            start,
            end,
        };
        // "one", "two", "three": match by match, through the rows and around
        let e = spec("e", "E", vec![]);
        assert_eq!(s.find_next(&e, 0, true, 0), Ok(hit(0, 2, 3)));
        assert_eq!(s.find_next(&e, 0, false, 3), Ok(hit(2, 3, 4)));
        assert_eq!(s.find_next(&e, 2, false, 4), Ok(hit(2, 4, 5)));
        assert_eq!(s.find_next(&e, 2, false, 5), Ok(hit(0, 2, 3)));
        assert_eq!(
            s.find_next(&spec("e", "", vec![2]), 0, true, 0),
            Ok(hit(2, 3, 4))
        );
        assert_eq!(
            s.find_next(&spec("", "", vec![]), 0, true, 0),
            Ok(hit(-1, 0, 0))
        );
        // Replace next takes the one shown, while it is still there
        assert_eq!(s.replace_hit(&e, &hit(2, 4, 5)), Ok(5));
        assert_eq!(s.row(2).text, "threE");
        assert_eq!(s.replace_hit(&e, &hit(2, 4, 5)), Ok(-1));
        assert!(s.undo());
        // an empty search replaces nothing (Aegisub)
        assert_eq!(s.replace_all(&spec("", "x", vec![])), Ok(0));
        assert_eq!(s.row(0).text, "one");
        // a translation row: its original first, then the translation; Replace all does both
        s.tl_enable(true);
        s.paste_translation(1, "two too");
        let o = spec("o", "0", vec![1]);
        let orig = FindHit {
            original: true,
            ..hit(1, 2, 3)
        };
        assert_eq!(s.find_next(&o, 1, true, 0), Ok(orig.clone()));
        assert_eq!(s.find_next(&o, 1, true, 3), Ok(hit(1, 2, 3)));
        assert_eq!(s.find_next(&o, 1, false, 3), Ok(hit(1, 5, 6)));
        assert_eq!(s.replace_hit(&o, &orig), Ok(3));
        assert_eq!(s.replace_all(&o), Ok(3));
        assert_eq!(
            (s.row(1).original.as_str(), s.row(1).text.as_str()),
            ("tw0", "tw0 t00")
        );
    }

    #[test]
    fn replace_the_matches_ticked() {
        let mut s = session(ASS);
        let e = FindSpec {
            pattern: "e".into(),
            replacement: "E".into(),
            match_case: true,
            ..Default::default()
        };
        // "one", "two", "three": the first and the last of three ticked
        let found = s.find_matches(&e, 9).unwrap();
        let ticked = [found[0].clone(), found[2].clone()];
        assert_eq!(s.replace_matches(&e, &ticked), Ok(2));
        assert_eq!(
            (s.row(0).text.as_str(), s.row(2).text.as_str()),
            ("onE", "threE")
        );
        assert!(s.undo());
        assert_eq!(
            (s.row(0).text.as_str(), s.row(2).text.as_str()),
            ("one", "three")
        );
        // a line changed since the search is left
        let mut r = s.row(0);
        r.text = "one!".into();
        s.set_row(0, &r, "");
        assert_eq!(s.replace_matches(&e, &found), Ok(2));
        assert_eq!(
            (s.row(0).text.as_str(), s.row(2).text.as_str()),
            ("one!", "thrEE")
        );
    }

    #[test]
    fn replace_rules_in_one_undo_step() {
        let mut s = session(ASS);
        let rule = |pattern: &str, replacement: &str| FindSpec {
            pattern: pattern.into(),
            replacement: replacement.into(),
            regex: true,
            skip_tags: true,
            rows: vec![0, 1],
            ..Default::default()
        };
        let bad = [rule("o", "0"), rule("(", "")];
        assert!(s.replace_rules(&bad).is_err());
        assert_eq!(s.row(0).text, "one");
        let rules = [rule(r"\bone\b", "One!"), rule("!", "?"), rule("T", "t")];
        assert_eq!(s.replace_rules(&rules), Ok(3));
        let texts = |s: &Session| (0..3).map(|i| s.row(i).text).collect::<Vec<_>>();
        assert_eq!(texts(&s), ["One?", "two", "three"]);
        assert!(s.undo());
        assert_eq!(texts(&s), ["one", "two", "three"]);
    }

    #[test]
    fn replacing_the_rule_matches_ticked() {
        let mut s = session(ASS);
        let rule = |pattern: &str, replacement: &str, case_rule: u8| FindSpec {
            pattern: pattern.into(),
            replacement: replacement.into(),
            regex: true,
            case_rule,
            ..Default::default()
        };
        // two rules in one line, each in its case: "one" -> "ONE" upper, "e" -> "x" written
        let rules = [rule("o", "a", 4), rule("e", "x", 2), rule("t", "Q", 3)];
        let mut ticked = Vec::new();
        let mut of = Vec::new();
        for (k, r) in rules.iter().enumerate() {
            for m in s.find_matches(r, 99).unwrap() {
                ticked.push(m);
                of.push(k);
            }
        }
        // the "t" of "three", found last, left unticked
        ticked.pop();
        of.pop();
        assert_eq!(s.replace_rule_matches(&rules, &ticked, &of), Ok(6));
        let texts = |s: &Session| (0..3).map(|i| s.row(i).text).collect::<Vec<_>>();
        assert_eq!(texts(&s), ["Anx", "qwA", "thrxx"]);
        assert!(s.undo());
        assert_eq!(texts(&s), ["one", "two", "three"]);
        // "w" is matched before a letter that another rule takes away: replaced as found; the
        // overlapping "tw" left
        let rules = [rule(r"w\B", "W", 2), rule("o", "", 2), rule("tw", "YY", 2)];
        let found: Vec<_> = rules
            .iter()
            .map(|r| s.find_matches(r, 9).unwrap())
            .collect();
        let in_two = |k: usize| found[k].iter().find(|m| m.row == 1).unwrap().clone();
        let ticked = [in_two(0), in_two(1), in_two(2)];
        assert_eq!(s.replace_rule_matches(&rules, &ticked, &[0, 1, 2]), Ok(2));
        assert_eq!(texts(&s)[1], "tW");
    }
}
