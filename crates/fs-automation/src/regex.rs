//! Aegisub's regex engine (boost::u32regex, Perl syntax) on top of PCRE2:
//! UTF-8 with Unicode properties, boost's option semantics, and boost's
//! Perl format strings for replacements.

use pcre2_sys::*;
use std::ptr;

pub const ICASE: i32 = 1;
pub const NOSUB: i32 = 2;
pub const COLLATE: i32 = 4;
pub const NEWLINE_ALT: i32 = 8;
pub const NO_MOD_M: i32 = 16;
pub const NO_MOD_S: i32 = 32;
pub const MOD_S: i32 = 64;
pub const MOD_X: i32 = 128;
pub const NO_EMPTY_SUBEXPRESSIONS: i32 = 256;

pub struct Regex {
    code: *mut pcre2_code_8,
    data: *mut pcre2_match_data_8,
}

// The compiled pattern is immutable; match data is only used through &mut.
unsafe impl Send for Regex {}

/// Capture groups of one match as byte ranges (`None` = unmatched group).
pub type Groups = Vec<Option<(usize, usize)>>;

fn error_message(code: i32) -> String {
    let mut buf = [0u8; 256];
    let n = unsafe { pcre2_get_error_message_8(code, buf.as_mut_ptr(), buf.len()) };
    String::from_utf8_lossy(&buf[..n.max(0) as usize]).into_owned()
}

/// Boost-only escapes outside character classes: `\<` `\>` (word start and
/// end), `` \` `` and `\'` (buffer start and end).
fn translate(pattern: &str) -> String {
    let mut out = String::with_capacity(pattern.len());
    let mut chars = pattern.chars().peekable();
    let (mut in_class, mut quoted) = (false, false);
    while let Some(c) = chars.next() {
        if c != '\\' {
            match c {
                '[' if !quoted => in_class = true,
                ']' if !quoted => in_class = false,
                _ => {}
            }
            out.push(c);
            continue;
        }
        let Some(n) = chars.next() else {
            out.push(c);
            break;
        };
        match n {
            'Q' => quoted = true,
            'E' => quoted = false,
            _ => {}
        }
        match (n, in_class || quoted) {
            ('<', false) => out.push_str(r"\b(?=\w)"),
            ('>', false) => out.push_str(r"\b(?<=\w)"),
            ('`', false) => out.push_str(r"\A"),
            ('\'', false) => out.push_str(r"\z"),
            _ => {
                out.push(c);
                out.push(n);
            }
        }
    }
    out
}

impl Regex {
    pub fn new(pattern: &str, flags: i32) -> Result<Regex, String> {
        let original = pattern;
        let translated = translate(pattern);
        let pattern = translated.as_str();
        let mut opts = PCRE2_UTF | PCRE2_UCP;
        if flags & ICASE != 0 {
            opts |= PCRE2_CASELESS;
        }
        // boost: ^/$ match at line breaks and `.` matches newlines unless disabled
        if flags & NO_MOD_M == 0 {
            opts |= PCRE2_MULTILINE;
        }
        if flags & NO_MOD_S == 0 || flags & MOD_S != 0 {
            opts |= PCRE2_DOTALL;
        }
        if flags & MOD_X != 0 {
            opts |= PCRE2_EXTENDED;
        }
        if flags & NOSUB != 0 {
            opts |= PCRE2_NO_AUTO_CAPTURE;
        }
        let (mut err, mut off) = (0, 0usize);
        let code = unsafe {
            pcre2_compile_8(
                pattern.as_ptr(),
                pattern.len(),
                opts,
                &mut err,
                &mut off,
                ptr::null_mut(),
            )
        };
        if code.is_null() {
            return Err(format!(
                "{} at offset {} in regular expression \"{}\"",
                error_message(err),
                off,
                original
            ));
        }
        unsafe {
            pcre2_jit_compile_8(code, PCRE2_JIT_COMPLETE); // falls back to the interpreter
        }
        let data = unsafe { pcre2_match_data_create_from_pattern_8(code, ptr::null_mut()) };
        Ok(Regex { code, data })
    }

    /// First match in `subject[start..]` (the text before `start` is visible to
    /// lookbehind, like boost's `match_prev_avail | match_not_bob`).
    pub fn search(
        &mut self,
        subject: &[u8],
        start: usize,
        not_empty_at_start: bool,
    ) -> Result<Option<Groups>, String> {
        let opts = if not_empty_at_start {
            PCRE2_NOTEMPTY_ATSTART
        } else {
            0
        };
        let rc = unsafe {
            pcre2_match_8(
                self.code,
                subject.as_ptr(),
                subject.len(),
                start,
                opts,
                self.data,
                ptr::null_mut(),
            )
        };
        if rc == PCRE2_ERROR_NOMATCH {
            return Ok(None);
        }
        if rc < 0 {
            return Err(error_message(rc));
        }
        let n = unsafe { pcre2_get_ovector_count_8(self.data) } as usize;
        let ov =
            unsafe { std::slice::from_raw_parts(pcre2_get_ovector_pointer_8(self.data), n * 2) };
        Ok(Some(
            ov.chunks(2)
                .map(|p| (p[0] != usize::MAX).then_some((p[0], p[1])))
                .collect(),
        ))
    }

    /// Replace up to `max_count` matches using a boost Perl format string
    /// (iteration like `boost::regex_iterator`).
    pub fn replace(
        &mut self,
        fmt: &str,
        subject: &[u8],
        max_count: i32,
    ) -> Result<Vec<u8>, String> {
        let mut out = Vec::with_capacity(subject.len());
        let (mut pos, mut last_end, mut left) = (0usize, 0usize, max_count);
        let mut empty = false;
        while left > 0 && pos <= subject.len() {
            let Some(g) = self.search(subject, pos, empty)? else {
                break;
            };
            let (s, e) = g[0].unwrap();
            out.extend_from_slice(&subject[last_end..s]);
            format(fmt.as_bytes(), subject, &g, last_end, &mut out);
            last_end = e;
            pos = e;
            empty = s == e;
            left -= 1;
        }
        out.extend_from_slice(&subject[last_end..]);
        Ok(out)
    }
}

impl Drop for Regex {
    fn drop(&mut self) {
        unsafe {
            pcre2_match_data_free_8(self.data);
            pcre2_code_free_8(self.code);
        }
    }
}

#[derive(Clone, Copy, PartialEq)]
enum Case {
    Copy,
    NextLower,
    NextUpper,
    Lower,
    Upper,
}

struct Out<'a> {
    out: &'a mut Vec<u8>,
    state: Case,
    restore: Case,
}

impl Out<'_> {
    fn put(&mut self, bytes: &[u8]) {
        let s = String::from_utf8_lossy(bytes);
        for c in s.chars() {
            let mapped: String = match self.state {
                Case::Copy => c.to_string(),
                Case::Lower | Case::NextLower => c.to_lowercase().collect(),
                Case::Upper | Case::NextUpper => c.to_uppercase().collect(),
            };
            self.out.extend_from_slice(mapped.as_bytes());
            if matches!(self.state, Case::NextLower | Case::NextUpper) {
                self.state = self.restore;
            }
        }
    }
}

fn group<'s>(subject: &'s [u8], g: &Groups, i: usize) -> &'s [u8] {
    match g.get(i).copied().flatten() {
        Some((s, e)) => &subject[s..e],
        None => b"",
    }
}

/// Leading decimal digits of `f[*i..]` (at most `max`), advancing `*i`.
fn digits(f: &[u8], i: &mut usize, max: usize) -> Option<usize> {
    let start = *i;
    while *i < f.len() && *i - start < max && f[*i].is_ascii_digit() {
        *i += 1;
    }
    (start != *i).then(|| {
        std::str::from_utf8(&f[start..*i])
            .unwrap()
            .parse()
            .unwrap_or(usize::MAX)
    })
}

/// boost `format_perl`: `$&`, `$0`..`$n`, `${n}`, `` $` ``, `$'`, `$$`, `$+`,
/// `\n` style escapes, `\1`..`\9`, and `\l \u \L \U \E` case conversion.
fn format(f: &[u8], subject: &[u8], g: &Groups, prefix_start: usize, out: &mut Vec<u8>) {
    let (ms, me) = g[0].unwrap();
    let mut o = Out {
        out,
        state: Case::Copy,
        restore: Case::Copy,
    };
    let mut i = 0;
    while i < f.len() {
        match f[i] {
            b'$' => {
                i += 1;
                if i == f.len() {
                    o.put(b"$");
                    break;
                }
                match f[i] {
                    b'&' => {
                        i += 1;
                        o.put(&subject[ms..me]);
                    }
                    b'`' => {
                        i += 1;
                        o.put(&subject[prefix_start..ms]);
                    }
                    b'\'' => {
                        i += 1;
                        o.put(&subject[me..]);
                    }
                    b'$' => {
                        i += 1;
                        o.put(b"$");
                    }
                    b'+' => {
                        i += 1;
                        let last = if g.len() > 1 { g.len() - 1 } else { 1 };
                        o.put(group(subject, g, last));
                    }
                    _ => {
                        let brace = f[i] == b'{';
                        let save = i;
                        if brace {
                            i += 1;
                        }
                        match digits(f, &mut i, usize::MAX) {
                            Some(n) if !brace || f.get(i) == Some(&b'}') => {
                                if brace {
                                    i += 1;
                                }
                                o.put(group(subject, g, n));
                            }
                            _ => {
                                i = save;
                                o.put(b"$");
                            }
                        }
                    }
                }
            }
            b'\\' => {
                i += 1;
                if i == f.len() {
                    o.put(b"\\");
                    break;
                }
                let c = f[i];
                i += 1;
                let simple = match c {
                    b'a' => Some(7u8),
                    b'f' => Some(12),
                    b'n' => Some(b'\n'),
                    b'r' => Some(b'\r'),
                    b't' => Some(b'\t'),
                    b'v' => Some(11),
                    b'e' => Some(27),
                    _ => None,
                };
                if let Some(b) = simple {
                    o.put(&[b]);
                    continue;
                }
                match c {
                    b'x' => {
                        let (s, e) = if f.get(i) == Some(&b'{') {
                            let e = f[i..].iter().position(|&b| b == b'}').map(|p| i + p);
                            match e {
                                Some(e) => (i + 1, e),
                                None => (i, i),
                            }
                        } else {
                            let n = f[i..]
                                .iter()
                                .take(2)
                                .take_while(|b| b.is_ascii_hexdigit())
                                .count();
                            (i, i + n)
                        };
                        let v = std::str::from_utf8(&f[s..e])
                            .ok()
                            .and_then(|h| u32::from_str_radix(h, 16).ok())
                            .and_then(char::from_u32)
                            .unwrap_or('\0');
                        i = if f.get(e) == Some(&b'}') { e + 1 } else { e };
                        o.put(v.to_string().as_bytes());
                    }
                    b'c' if i < f.len() => {
                        o.put(&[f[i] % 32]);
                        i += 1;
                    }
                    b'l' => {
                        o.restore = o.state;
                        o.state = Case::NextLower;
                    }
                    b'u' => {
                        o.restore = o.state;
                        o.state = Case::NextUpper;
                    }
                    b'L' => o.state = Case::Lower,
                    b'U' => o.state = Case::Upper,
                    b'E' => o.state = Case::Copy,
                    b'1'..=b'9' => o.put(group(subject, g, (c - b'0') as usize)),
                    b'0' => {
                        // octal escape: up to 3 more digits
                        let n = f[i..]
                            .iter()
                            .take(3)
                            .take_while(|b| (b'0'..=b'7').contains(b))
                            .count();
                        let v =
                            u8::from_str_radix(std::str::from_utf8(&f[i..i + n]).unwrap_or("0"), 8)
                                .unwrap_or(0);
                        i += n;
                        o.put(&[v]);
                    }
                    _ => o.put(&[c]),
                }
            }
            _ => {
                // copy a whole UTF-8 sequence at once
                let n = match f[i] {
                    b if b < 0x80 => 1,
                    b if b < 0xE0 => 2,
                    b if b < 0xF0 => 3,
                    _ => 4,
                }
                .min(f.len() - i);
                o.put(&f[i..i + n]);
                i += n;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sub(s: &str, p: &str, r: &str) -> String {
        let mut re = Regex::new(p, 0).unwrap();
        String::from_utf8(re.replace(r, s.as_bytes(), i32::MAX).unwrap()).unwrap()
    }

    #[test]
    fn boost_format_and_empty_matches() {
        assert_eq!(sub("abc", "e?", "d"), "dadbdcd");
        assert_eq!(sub("abc", "b*", "d"), "daddcd");
        assert_eq!(
            sub("hello world", r"(\w+) (\w+)", r"$2 \1 ${1}!"),
            "world hello hello!"
        );
        assert_eq!(sub("abc", "b", r"[$`|$'|$&|$$]"), "a[a|c|b|$]c");
        assert_eq!(sub("ab", "(a)(b)", r"\u$1\U$2x\Ey"), "ABXy");
        assert_eq!(sub("a", "a", r"\x41\t$9"), "A\t");
    }

    #[test]
    fn options() {
        let mut re = Regex::new("^b", 0).unwrap();
        assert!(re.search(b"a\nb", 0, false).unwrap().is_some());
        let mut re = Regex::new("^b", NO_MOD_M).unwrap();
        assert!(re.search(b"a\nb", 0, false).unwrap().is_none());
        let mut re = Regex::new("a.b", 0).unwrap();
        assert!(re.search(b"a\nb", 0, false).unwrap().is_some());
        let mut re = Regex::new("Ż", ICASE).unwrap();
        assert!(re.search("żółw".as_bytes(), 0, false).unwrap().is_some());
        assert!(Regex::new("(", 0).is_err());
        let mut re = Regex::new("d", 0).unwrap();
        assert!(
            re.search(b"abc\xffdef", 0, false)
                .unwrap_err()
                .contains("UTF-8")
        );
    }
}
