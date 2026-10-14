//! Spell checking with Hunspell dictionaries (spellbook), plus the word
//! scanner that skips override blocks, `\N`/`\h` and `\p` drawings.

use spellbook::Dictionary;

pub struct Speller(Dictionary);

/// Hunspell files may be in a legacy charset (`SET ISO8859-2`); spellbook wants UTF-8.
fn decode(bytes: &[u8], set: &str) -> String {
    let enc = encoding_rs::Encoding::for_label(set.as_bytes()).unwrap_or(encoding_rs::UTF_8);
    enc.decode_with_bom_removal(bytes).0.into_owned()
}

pub fn new_speller(aff_path: &str, dic_path: &str) -> Result<Box<Speller>, String> {
    let read = |p: &str| std::fs::read(p).map_err(|e| format!("{p}: {e}"));
    let aff_raw = read(aff_path)?;
    let set = String::from_utf8_lossy(&aff_raw)
        .lines()
        .find_map(|l| l.trim().strip_prefix("SET ").map(|s| s.trim().to_string()))
        .unwrap_or_else(|| "UTF-8".into());
    let aff: String = decode(&aff_raw, &set)
        .lines()
        .map(|l| {
            if l.trim_start().starts_with("SET ") {
                "SET UTF-8"
            } else {
                l
            }
        })
        .collect::<Vec<_>>()
        .join("\n");
    let dic = decode(&read(dic_path)?, &set);
    Dictionary::new(&aff, &dic)
        .map(|d| Box::new(Speller(d)))
        .map_err(|e| format!("{dic_path}: {e}"))
}

/// Words of subtitle text as (UTF-16 start, UTF-16 length, word): letters
/// with inner `'`, `’` or `-`; tags, comments and drawings are skipped.
pub fn words(text: &str) -> Vec<(usize, usize, String)> {
    let mut out = Vec::new();
    let (mut pos, mut drawing) = (0usize, false);
    let mut cur: Option<(usize, String)> = None;
    let mut flush = |cur: &mut Option<(usize, String)>, end: usize| {
        if let Some((s, mut w)) = cur.take() {
            while w.ends_with(['\'', '’', '-']) {
                w.pop();
            }
            let len = w.encode_utf16().count();
            if len > 0 && end >= s {
                out.push((s, len, w));
            }
        }
    };
    let mut it = text.chars().peekable();
    while let Some(c) = it.next() {
        if c == '{' {
            flush(&mut cur, pos);
            let mut block = String::new();
            pos += 1;
            for b in it.by_ref() {
                pos += b.len_utf16();
                if b == '}' {
                    break;
                }
                block.push(b);
            }
            // the last \pN in the block decides whether text that follows is a drawing
            if let Some(i) = block.rfind("\\p") {
                let n: String = block[i + 2..]
                    .chars()
                    .take_while(|d| d.is_ascii_digit())
                    .collect();
                if !n.is_empty() {
                    drawing = n.parse::<u32>().unwrap_or(0) > 0;
                }
            }
            continue;
        }
        if c == '\\' && matches!(it.peek(), Some('N' | 'n' | 'h')) {
            flush(&mut cur, pos);
            it.next();
            pos += 2;
            continue;
        }
        if !drawing && (c.is_alphabetic() || (cur.is_some() && matches!(c, '\'' | '’' | '-'))) {
            cur.get_or_insert((pos, String::new())).1.push(c);
        } else {
            flush(&mut cur, pos);
        }
        pos += c.len_utf16();
    }
    flush(&mut cur, pos);
    out
}

impl Speller {
    /// Misspelled words as flat [start, len, start, len, ...] in UTF-16 units.
    pub fn misspelled(&self, text: &str) -> Vec<usize> {
        words(text)
            .into_iter()
            .filter(|(_, _, w)| !self.0.check(w))
            .flat_map(|(s, l, _)| [s, l])
            .collect()
    }

    pub fn check(&self, word: &str) -> bool {
        self.0.check(word)
    }

    pub fn suggest(&self, word: &str) -> Vec<String> {
        let mut out = Vec::new();
        self.0.suggest(word, &mut out);
        out
    }

    /// Accept `word` for this session (the GUI persists the user's list).
    pub fn add_word(&mut self, word: &str) {
        let _ = self.0.add(word);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scanner_skips_tags_and_drawings() {
        let t = r"{\i1}Zażółć gęślą\Njaźń{komentarz} don't-{\p1}m 0 0 l 10 10{\p0} koniec–";
        let w: Vec<String> = words(t).into_iter().map(|x| x.2).collect();
        assert_eq!(w, ["Zażółć", "gęślą", "jaźń", "don't", "koniec"]);
        let (s, l, _) = &words(t)[2];
        let u: Vec<u16> = t.encode_utf16().collect();
        assert_eq!(String::from_utf16(&u[*s..s + l]).unwrap(), "jaźń");
    }

    /// A real Polish Hunspell dictionary when available: FS_DICT=path/to/pl_PL
    /// (without extension; sjp.pl ships it in ISO8859-2).
    #[test]
    fn polish_dictionary() {
        let Ok(base) = std::env::var("FS_DICT") else {
            eprintln!("skipping: FS_DICT not set");
            return;
        };
        let mut sp = new_speller(&format!("{base}.aff"), &format!("{base}.dic")).unwrap();
        let t = r"{\an8}Zażółć gęślą jaźń, ale błendny wyras.";
        let bad: Vec<String> = sp
            .misspelled(t)
            .chunks(2)
            .map(|c| {
                String::from_utf16(&t.encode_utf16().collect::<Vec<_>>()[c[0]..c[0] + c[1]])
                    .unwrap()
            })
            .collect();
        assert_eq!(bad, ["błendny", "wyras"]);
        assert!(
            sp.suggest("błendny").iter().any(|s| s == "błędny"),
            "{:?}",
            sp.suggest("błendny")
        );
        sp.add_word("wyras");
        assert!(sp.check("wyras"));
    }
}

#[cfg(test)]
mod english {
    /// FS_DICT_EN=path/to/en_US (SCOWL hunspell-en_US release).
    #[test]
    fn english_dictionary() {
        let Ok(base) = std::env::var("FS_DICT_EN") else {
            eprintln!("skipping: FS_DICT_EN not set");
            return;
        };
        let sp = super::new_speller(&format!("{base}.aff"), &format!("{base}.dic")).unwrap();
        let t = r"{\i1}I don't recieve it\Nfrom {\b1}teh{\b0} server's cache.";
        let bad = sp.misspelled(t);
        let u: Vec<u16> = t.encode_utf16().collect();
        let words: Vec<String> = bad
            .chunks(2)
            .map(|c| String::from_utf16(&u[c[0]..c[0] + c[1]]).unwrap())
            .collect();
        assert_eq!(words, ["recieve", "teh"]);
        assert!(sp.suggest("recieve").iter().any(|s| s == "receive"));
    }
}
