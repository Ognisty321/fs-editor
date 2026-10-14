//! Aegisub's kanji timer: copy karaoke timing from a romaji line onto a kanji
//! line by grouping source syllables with destination characters
//! (libaegisub karaoke_matcher.cpp).
//!
//! shortcut: "characters" are Unicode code points, not grapheme clusters as in
//! Aegisub's ICU break iterator; upgrade if combining marks show up in lyrics.

use crate::kana_table::KANA;

/// Case-, accent- and width-insensitive form of a character (ICU's primary
/// collation strength, as far as lyrics need it).
fn fold(c: char) -> char {
    let c = match c as u32 {
        // fullwidth ASCII
        0xFF01..=0xFF5E => char::from_u32(c as u32 - 0xFEE0).unwrap_or(c),
        _ => c,
    };
    let c = c.to_lowercase().next().unwrap_or(c);
    const ACCENTED: &str = "àáâãäåèéêëìíîïòóôõöùúûüýÿñçāēīōū";
    const PLAIN: &str = "aaaaaaeeeeiiiiooooouuuuyyncaeiou";
    match ACCENTED.chars().position(|a| a == c) {
        Some(i) => PLAIN.chars().nth(i).unwrap_or(c),
        None => c,
    }
}

fn is_ws(s: &str) -> bool {
    s.chars().all(char::is_whitespace)
}

/// Every kana written as the longest romaji (3..1 chars) that starts `s`, and
/// that romaji's length.
fn romaji_to_kana(s: &str) -> (Vec<&'static str>, usize) {
    for len in (1..=s.len().min(3)).rev() {
        let Some(p) = s.get(..len) else { continue };
        let v: Vec<&str> = KANA
            .iter()
            .filter(|(_, r)| *r == p)
            .map(|(k, _)| *k)
            .collect();
        if !v.is_empty() {
            return (v, len);
        }
    }
    (Vec::new(), 0)
}

fn kana_to_romaji(k: &str) -> impl Iterator<Item = &'static str> + '_ {
    KANA.iter().filter(move |(x, _)| *x == k).map(|(_, r)| *r)
}

/// Aegisub's AutoMatchKaraoke: how many source syllables and destination
/// characters belong to the first source syllable.
pub fn auto_match(src: &[&str], dst: &str) -> (usize, usize) {
    if src.is_empty() {
        return (0, 0);
    }
    let (mut src_len, mut dst_len) = (1usize, 0usize);
    let s: Vec<char> = src[0].to_lowercase().chars().collect();
    let d: Vec<char> = dst.chars().collect();
    let (mut si, mut di) = (0usize, 0usize);
    let tail = |v: &[char], i: usize| v[i..].iter().collect::<String>();

    // skips leading whitespace; true when either side ran out
    let eat = |si: &mut usize, di: &mut usize, dst_len: &mut usize, src_len: &mut usize| {
        while *si < s.len() && s[*si].is_whitespace() {
            *si += 1;
        }
        while *di < d.len() && d[*di].is_whitespace() {
            *di += 1;
            *dst_len += 1;
        }
        if *di >= d.len() {
            // out of destination: this takes the rest of the source
            *src_len = src.len();
            return true;
        }
        *si >= s.len()
    };
    if eat(&mut si, &mut di, &mut dst_len, &mut src_len) {
        return (src_len, dst_len);
    }

    while si < s.len() {
        while fold(s[si]) == fold(d[di]) {
            si += 1;
            di += 1;
            dst_len += 1;
            if eat(&mut si, &mut di, &mut dst_len, &mut src_len) {
                return (src_len, dst_len);
            }
        }
        // a romanised kana at the start of the source, written as kana in the destination?
        let rest_s = tail(&s, si);
        let rest_d = tail(&d, di);
        let (kanas, romaji_len) = romaji_to_kana(&rest_s);
        let Some(kana) = kanas.into_iter().find(|k| rest_d.starts_with(k)) else {
            break;
        };
        si += romaji_len;
        let n = kana.chars().count();
        di += n;
        dst_len += n;
        if eat(&mut si, &mut di, &mut dst_len, &mut src_len) {
            return (src_len, dst_len);
        }
    }

    // one destination character left: it takes all the remaining source
    if di + 1 == d.len() {
        return (src.len(), dst_len + 1);
    }

    // Look up to 3 characters ahead in the destination for kana that start a
    // later syllable; everything before it belongs to this one. A kanji reads
    // as at most 5 kana, which bounds how far ahead in the source to look.
    const MAX_CHARACTER_LENGTH: usize = 5;
    const DST_LOOKAHEAD_MAX: usize = 3;
    for lookahead in 0..DST_LOOKAHEAD_MAX {
        di += 1;
        if di >= d.len() {
            break;
        }
        let cur = d[di];
        if !(0x3000..=0x3FFF).contains(&(cur as u32)) {
            continue; // not kana
        }
        let mut translit: Vec<&str> = Vec::new();
        if let Some(&next) = d.get(di + 1) {
            translit.extend(kana_to_romaji(&format!("{cur}{next}")));
        }
        translit.extend(kana_to_romaji(&cur.to_string()));

        let src_lookahead_max = (lookahead + 1) * MAX_CHARACTER_LENGTH;
        let mut pos = 0usize;
        for syl in src {
            if is_ws(syl) {
                continue;
            }
            pos += 1;
            if pos == 1 {
                continue;
            }
            if pos > src_lookahead_max {
                break;
            }
            let lsyl = syl.to_lowercase();
            if !(syl.starts_with(cur) || translit.iter().any(|t| lsyl.starts_with(t))) {
                continue;
            }
            if pos == 2 {
                return (src_len, dst_len + lookahead + 1);
            }
            // further ahead: share the characters evenly between the syllables
            let n = ((pos as f64 - 1.0) / (lookahead as f64 + 1.0) + 0.5) as usize;
            return (n, dst_len + 1);
        }
    }
    (src_len, dst_len.max(1))
}

/// One kanji-timer line pair being grouped (Aegisub's KaraokeMatcher).
#[derive(Debug, Clone, Default)]
pub struct KanjiMatcher {
    /// (duration ms, text) of the source syllables.
    syls: Vec<(i64, String)>,
    dst: Vec<char>,
    /// Matched (source syllable range, destination char range).
    groups: Vec<(std::ops::Range<usize>, std::ops::Range<usize>)>,
    src_start: usize,
    src_len: usize,
    dst_start: usize,
    dst_len: usize,
}

impl KanjiMatcher {
    pub fn new(syls: Vec<(i64, String)>, dst: &str) -> Self {
        let dst: Vec<char> = dst.chars().collect();
        Self {
            src_len: usize::from(!syls.is_empty()),
            dst_len: usize::from(!dst.is_empty()),
            syls,
            dst,
            ..Default::default()
        }
    }

    fn text(&self, r: std::ops::Range<usize>) -> String {
        self.dst[r].iter().collect()
    }

    pub fn output(&self) -> String {
        self.groups
            .iter()
            .map(|(s, d)| {
                let dur: i64 = self.syls[s.clone()].iter().map(|x| x.0).sum();
                format!("{{\\k{}}}{}", dur / 10, self.text(d.clone()))
            })
            .collect()
    }

    /// (source texts, destination text) of each match so far.
    pub fn groups(&self) -> Vec<(Vec<String>, String)> {
        self.groups
            .iter()
            .map(|(s, d)| {
                let src = self.syls[s.clone()].iter().map(|x| x.1.clone()).collect();
                (src, self.text(d.clone()))
            })
            .collect()
    }

    pub fn source_selection(&self) -> Vec<String> {
        self.syls[self.src_start..self.src_start + self.src_len]
            .iter()
            .map(|x| x.1.clone())
            .collect()
    }
    pub fn unmatched_source(&self) -> Vec<String> {
        self.syls[self.src_start + self.src_len..]
            .iter()
            .map(|x| x.1.clone())
            .collect()
    }
    pub fn destination_selection(&self) -> String {
        self.text(self.dst_start..self.dst_start + self.dst_len)
    }
    pub fn unmatched_destination(&self) -> String {
        self.text(self.dst_start + self.dst_len..self.dst.len())
    }
    /// Source syllables / destination characters not yet in a group.
    pub fn remaining(&self) -> (usize, usize) {
        (
            self.syls.len() - self.src_start,
            self.dst.len() - self.dst_start,
        )
    }

    pub fn increase_source(&mut self) -> bool {
        let ok = self.src_start + self.src_len < self.syls.len();
        self.src_len += usize::from(ok);
        ok
    }
    pub fn decrease_source(&mut self) -> bool {
        let ok = self.src_len > 0;
        self.src_len -= usize::from(ok);
        ok
    }
    pub fn increase_destination(&mut self) -> bool {
        let ok = self.dst_start + self.dst_len < self.dst.len();
        self.dst_len += usize::from(ok);
        ok
    }
    pub fn decrease_destination(&mut self) -> bool {
        let ok = self.dst_len > 0;
        self.dst_len -= usize::from(ok);
        ok
    }

    pub fn auto_match(&mut self) {
        let src: Vec<&str> = self.syls[self.src_start..]
            .iter()
            .map(|x| x.1.as_str())
            .collect();
        let rest = self.text(self.dst_start..self.dst.len());
        (self.src_len, self.dst_len) = auto_match(&src, &rest);
    }

    pub fn accept(&mut self) -> bool {
        if self.src_len == 0 && self.dst_len == 0 {
            return false;
        }
        self.groups.push((
            self.src_start..self.src_start + self.src_len,
            self.dst_start..self.dst_start + self.dst_len,
        ));
        self.src_start += self.src_len;
        self.dst_start += self.dst_len;
        self.src_len = 0;
        self.dst_len = 0;
        self.increase_source();
        self.increase_destination();
        true
    }

    pub fn undo(&mut self) -> bool {
        let Some((s, d)) = self.groups.pop() else {
            return false;
        };
        (self.src_start, self.src_len) = (s.start, s.len());
        (self.dst_start, self.dst_len) = (d.start, d.len());
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Aegisub's tests/tests/karaoke_matcher.cpp
    #[test]
    fn auto_match_cases() {
        let m = |s: &[&str], d: &str| auto_match(s, d);
        assert_eq!(m(&[], "a").0, 0);
        assert_eq!(m(&["a", "b"], ""), (2, 0));
        assert_eq!(m(&["", "b"], "cc"), (1, 0));
        assert_eq!(m(&["a", "b"], "cc"), (1, 1));
        assert_eq!(m(&["abc", "de"], "abcde"), (1, 3));
        assert_eq!(m(&["abc", "de"], "ABCDE"), (1, 3));
        assert_eq!(m(&["ABC", "DE"], "abcde"), (1, 3));
        assert_eq!(m(&[" abc", "de"], "abcde"), (1, 3));
        assert_eq!(m(&["abc ", "de"], "abcde"), (1, 3));
        assert_eq!(m(&["abc ", "de"], " abcde"), (1, 4));
        assert_eq!(m(&["abc ", "de"], "abc de"), (1, 4));
        assert_eq!(m(&["abc ", "de"], "ab c de"), (1, 5));
        assert_eq!(m(&["∫∫", "de"], "∫∫a"), (1, 2));
        assert_eq!(m(&["rode"], "ろ"), (1, 1));
        assert_eq!(m(&["rode"], "ロ"), (1, 1));
        assert_eq!(m(&["romajide"], "ろまじ"), (1, 3));
        assert_eq!(m(&["kya", "e"], "きゃe"), (1, 2));
        assert_eq!(m(&["kya"], "きゃ"), (1, 2));
        assert_eq!(m(&["ro ma jide"], "ろまじ"), (1, 3));
        assert_eq!(m(&["r om aj ide"], "ろまじ"), (1, 1));
        assert_eq!(m(&["a", "b", "c"], "ろ"), (3, 1));
        assert_eq!(m(&["ab", "cd"], "ａｂc"), (1, 2));
        assert_eq!(m(&["ab", "ro"], "eeろ"), (1, 2));
        assert_eq!(m(&["ab", "", "ro"], "eeろ"), (1, 2));
        assert_eq!(m(&["ab", "", "", "", "", "", "ro"], "eeろ"), (1, 2));
        assert_eq!(m(&["abc", "", "ro"], "eeeろ"), (1, 3));
        assert_eq!(m(&["abcd", "", "ro"], "eeeeろ"), (1, 1));
        assert_eq!(m(&["a", "b", "ro"], "eeろ"), (1, 1));
        assert_eq!(m(&["a", "b", "c", "ro"], "eeろ"), (2, 1));
        assert_eq!(m(&["a", "b", "c", "d", "ro"], "eeろ"), (2, 1));
        assert_eq!(m(&["a", "b", "c", "d", "f", "ro"], "eeろ"), (3, 1));
        assert_eq!(m(&["a", "b", "c", "d", "f", "ro"], " eeろ"), (3, 2));
        assert_eq!(m(&["aa", "kya"], "eeきゃ"), (1, 2));
        assert_eq!(m(&["aa", "kyan"], "eeきゃ"), (1, 2));
        assert_eq!(m(&["Bo", "ku", "wa"], "僕は"), (2, 1));
        assert_eq!(m(&["Bo", "ku", "e"], "僕へ"), (2, 1));
        assert_eq!(m(&["shi", "tta", ""], "知った"), (1, 1));
        assert_eq!(m(&["tta", ""], "った"), (2, 2));
        assert_eq!(
            m(
                &["Oh... ", "Nan", "ka ", "ta", "ri", "nai"],
                "Oh…なんか足りない"
            ),
            (1, 3)
        );
    }

    #[test]
    fn grouping_walk_and_undo() {
        let syls = |v: &[(i64, &str)]| v.iter().map(|&(d, t)| (d, t.to_string())).collect();
        let mut m = KanjiMatcher::new(syls(&[]), "");
        assert!(!m.increase_source() && !m.decrease_source());
        assert!(!m.increase_destination() && !m.decrease_destination());
        assert!(!m.accept() && !m.undo());

        let mut m = KanjiMatcher::new(
            syls(&[(100, "1"), (200, "2"), (300, "3"), (400, "4")]),
            "abcd",
        );
        assert!(m.decrease_source() && !m.decrease_source());
        assert!(m.increase_source() && m.increase_source());
        assert_eq!(m.source_selection(), ["1", "2"]);
        assert!(m.decrease_source());
        for out in [
            "{\\k10}a",
            "{\\k10}a{\\k20}b",
            "{\\k10}a{\\k20}b{\\k30}c",
            "{\\k10}a{\\k20}b{\\k30}c{\\k40}d",
        ] {
            assert!(m.accept());
            assert_eq!(m.output(), out);
        }
        assert!(!m.accept());
        assert_eq!(m.remaining(), (0, 0));
        assert!(m.undo() && m.undo());
        assert_eq!(m.output(), "{\\k10}a{\\k20}b");
        assert_eq!(
            (m.source_selection(), m.destination_selection()),
            (vec!["3".to_string()], "c".into())
        );
        assert_eq!(m.unmatched_destination(), "d");
    }
}
