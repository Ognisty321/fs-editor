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

/// A match context with a JIT stack of its own, one per thread. Without one PCRE2's JIT has
/// 32 KiB of the machine stack, which a pattern that backtracks uses up on a line of a couple
/// of thousand characters ("JIT stack limit reached"); boost has no such limit.
struct MatchContext {
    ctx: *mut pcre2_match_context_8,
    stack: *mut pcre2_jit_stack_8,
}

impl MatchContext {
    fn new() -> MatchContext {
        unsafe {
            let ctx = pcre2_match_context_create_8(ptr::null_mut());
            // grows as it is needed, up to the second size
            let stack = pcre2_jit_stack_create_8(64 << 10, 32 << 20, ptr::null_mut());
            if !ctx.is_null() && !stack.is_null() {
                pcre2_jit_stack_assign_8(ctx, None, stack.cast());
            }
            MatchContext { ctx, stack }
        }
    }
}

impl Drop for MatchContext {
    fn drop(&mut self) {
        unsafe {
            if !self.ctx.is_null() {
                pcre2_match_context_free_8(self.ctx);
            }
            if !self.stack.is_null() {
                pcre2_jit_stack_free_8(self.stack);
            }
        }
    }
}

thread_local! {
    static MATCH_CONTEXT: MatchContext = MatchContext::new();
}

/// A Unicode general category by the long name boost's ICU traits know it under
/// (`\p{Lowercase Letter}`, `[[:Letter:]]`), as the short one PCRE2 has; spaces, `_` and `-`
/// in the name and its case do not count.
fn category(name: &str) -> Option<&'static str> {
    let key: String = (name.chars())
        .filter(|c| !matches!(c, ' ' | '_' | '-'))
        .flat_map(char::to_lowercase)
        .collect();
    Some(match key.as_str() {
        "letter" => "L",
        "lowercaseletter" => "Ll",
        "uppercaseletter" => "Lu",
        "titlecaseletter" => "Lt",
        "casedletter" => "L&",
        "modifierletter" => "Lm",
        "otherletter" => "Lo",
        "mark" => "M",
        "nonspacingmark" => "Mn",
        "spacingmark" | "spacingcombiningmark" | "combiningspacingmark" => "Mc",
        "enclosingmark" => "Me",
        "number" => "N",
        "decimalnumber" | "decimaldigitnumber" | "digit" => "Nd",
        "letternumber" => "Nl",
        "othernumber" => "No",
        "punctuation" => "P",
        "connectorpunctuation" => "Pc",
        "dashpunctuation" => "Pd",
        "openpunctuation" => "Ps",
        "closepunctuation" => "Pe",
        "initialpunctuation" => "Pi",
        "finalpunctuation" => "Pf",
        "otherpunctuation" => "Po",
        "symbol" => "S",
        "mathsymbol" => "Sm",
        "currencysymbol" => "Sc",
        "modifiersymbol" => "Sk",
        "othersymbol" => "So",
        "separator" => "Z",
        "spaceseparator" => "Zs",
        "lineseparator" => "Zl",
        "paragraphseparator" => "Zp",
        "other" => "C",
        "control" => "Cc",
        "format" => "Cf",
        "surrogate" => "Cs",
        "privateuse" => "Co",
        "notassigned" | "unassigned" => "Cn",
        _ => return None,
    })
}

/// The POSIX class names PCRE2 has itself.
const POSIX_CLASSES: &[&str] = &[
    "alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower", "print", "punct",
    "space", "upper", "word", "xdigit",
];

/// `[:name:]` inside a character class, for the names boost has and PCRE2 does not: the
/// one-letter ones, `unicode` (code points past 255) and the Unicode categories.
fn class_name(name: &str) -> Option<String> {
    let (not, name) = match name.strip_prefix('^') {
        Some(n) => (true, n),
        None => (false, name),
    };
    if POSIX_CLASSES.contains(&name) {
        return None;
    }
    let escape = |plain: &str, negated: &str| Some(if not { negated } else { plain }.to_string());
    match name {
        "d" => escape(r"\d", r"\D"),
        "w" => escape(r"\w", r"\W"),
        "s" => escape(r"\s", r"\S"),
        "h" => escape(r"\h", r"\H"),
        "v" => escape(r"\v", r"\V"),
        "l" => escape(r"\p{Ll}", r"\P{Ll}"),
        "u" => escape(r"\p{Lu}", r"\P{Lu}"),
        "unicode" if !not => Some(r"\x{100}-\x{10FFFF}".into()),
        "unicode" => Some(r"\x{0}-\x{FF}".into()),
        _ => {
            let short = category(name).unwrap_or(name);
            Some(format!(r"\{}{{{short}}}", if not { 'P' } else { 'p' }))
        }
    }
}

/// What boost's Perl syntax has and PCRE2 does not. Outside character classes: `\<` `\>` (word
/// start and end), `` \` `` and `\'` (buffer start and end). Anywhere: `\l` `\u` (a lower- or
/// upper-case letter) and `\L` `\U` (anything else), the long names of the Unicode categories
/// in `\p{…}`, and boost's own names in `[[:…:]]`. Nothing between `\Q` and `\E` is touched.
fn translate(pattern: &str) -> String {
    let mut out = String::with_capacity(pattern.len());
    let mut chars = pattern.chars().peekable();
    let (mut in_class, mut quoted) = (false, false);
    while let Some(c) = chars.next() {
        if c != '\\' {
            if c == '[' && in_class && !quoted && chars.peek() == Some(&':') {
                // a named class: read whole, so its "]" does not end the class it is in
                let rest: String = chars.clone().skip(1).collect();
                if let Some(end) = rest.find(":]") {
                    let name = &rest[..end];
                    match class_name(name) {
                        Some(t) => out.push_str(&t),
                        None => {
                            out.push_str("[:");
                            out.push_str(name);
                            out.push_str(":]");
                        }
                    }
                    for _ in 0..name.chars().count() + 3 {
                        chars.next();
                    }
                    continue;
                }
            }
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
            'E' => {
                // also the end of nothing: boost and PCRE2 both let a lone \E be
                quoted = false;
                out.push_str(r"\E");
                continue;
            }
            _ => {}
        }
        if quoted {
            out.push(c);
            out.push(n);
            continue;
        }
        match (n, in_class) {
            ('<', false) => out.push_str(r"\b(?=\w)"),
            ('>', false) => out.push_str(r"\b(?<=\w)"),
            ('`', false) => out.push_str(r"\A"),
            ('\'', false) => out.push_str(r"\z"),
            ('l', _) => out.push_str(r"\p{Ll}"),
            ('u', _) => out.push_str(r"\p{Lu}"),
            ('L', _) => out.push_str(r"\P{Ll}"),
            ('U', _) => out.push_str(r"\P{Lu}"),
            ('p' | 'P', _) if chars.peek() == Some(&'{') => {
                let rest: String = chars.clone().skip(1).collect();
                out.push(c);
                out.push(n);
                if let Some(end) = rest.find('}') {
                    let name = &rest[..end];
                    let (not, plain) = match name.strip_prefix('^') {
                        Some(p) => ("^", p),
                        None => ("", name),
                    };
                    out.push('{');
                    out.push_str(not);
                    out.push_str(category(plain).unwrap_or(plain));
                    out.push('}');
                    for _ in 0..name.chars().count() + 2 {
                        chars.next();
                    }
                }
            }
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
            // boost's line ends for ^, $ and a dot that stops at them: CR, LF, CRLF, form feed,
            // NEL and the Unicode line and paragraph separators, not LF alone
            let context = pcre2_compile_context_create_8(ptr::null_mut());
            if !context.is_null() {
                pcre2_set_newline_8(context, PCRE2_NEWLINE_ANY);
            }
            let code = pcre2_compile_8(
                pattern.as_ptr(),
                pattern.len(),
                opts,
                &mut err,
                &mut off,
                context,
            );
            if !context.is_null() {
                pcre2_compile_context_free_8(context);
            }
            code
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
        let run = |opts: u32, context: *mut pcre2_match_context_8| unsafe {
            pcre2_match_8(
                self.code,
                subject.as_ptr(),
                subject.len(),
                start,
                opts,
                self.data,
                context,
            )
        };
        let mut rc = (MATCH_CONTEXT.try_with(|m| run(opts, m.ctx)))
            .unwrap_or_else(|_| run(opts, ptr::null_mut()));
        if rc == PCRE2_ERROR_JIT_STACKLIMIT {
            // deeper than the JIT's stack goes: the interpreter, which keeps its own on the heap
            rc = run(opts | PCRE2_NO_JIT, ptr::null_mut());
        }
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
        let code = self.code;
        // the number of a named group, for `$+{name}`
        let named = |name: &[u8]| {
            let name = std::ffi::CString::new(name).ok()?;
            let n = unsafe { pcre2_substring_number_from_name_8(code, name.as_ptr().cast()) };
            usize::try_from(n).ok().filter(|n| *n > 0)
        };
        while left > 0 && pos <= subject.len() {
            let Some(g) = self.search(subject, pos, empty)? else {
                break;
            };
            let (s, e) = g[0].unwrap();
            out.extend_from_slice(&subject[last_end..s]);
            format(fmt.as_bytes(), subject, &g, last_end, &named, &mut out);
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

/// What a Perl 5.10 name in a format string stands for (boost's `handle_perl_verb`).
#[derive(Clone, Copy)]
enum Verb {
    Match,
    Prefix,
    Suffix,
    LastGroup,
    LastClosed,
}

/// The name at `f[*i..]`, after `$` or `${`: `MATCH`, `PREMATCH`, `POSTMATCH`,
/// `LAST_PAREN_MATCH`, `LAST_SUBMATCH_RESULT` and `^N`; in braces the first three may have a
/// `^` before them and the brace must close.
fn perl_verb(f: &[u8], i: &mut usize, brace: bool) -> Option<Verb> {
    let mut at = *i;
    if brace && f.get(at) == Some(&b'^') {
        at += 1;
    }
    let names: [(&[u8], Verb); 6] = [
        (b"MATCH", Verb::Match),
        (b"PREMATCH", Verb::Prefix),
        (b"POSTMATCH", Verb::Suffix),
        (b"LAST_PAREN_MATCH", Verb::LastGroup),
        (b"LAST_SUBMATCH_RESULT", Verb::LastClosed),
        (b"^N", Verb::LastClosed),
    ];
    let (name, verb) = names.into_iter().find(|(n, _)| f[at..].starts_with(n))?;
    at += name.len();
    if brace {
        if f.get(at) != Some(&b'}') {
            return None;
        }
        at += 1;
    }
    *i = at;
    Some(verb)
}

/// boost `format_perl`: `$&`, `$0`..`$n`, `${n}`, `` $` ``, `$'`, `$$`, `$+`, `$+{name}`,
/// the Perl 5.10 names ([`perl_verb`]), `\n` style escapes, `\1`..`\9`, and `\l \u \L \U \E`
/// case conversion. `named` gives a named group's number.
fn format(
    f: &[u8],
    subject: &[u8],
    g: &Groups,
    prefix_start: usize,
    named: &dyn Fn(&[u8]) -> Option<usize>,
    out: &mut Vec<u8>,
) {
    let (ms, me) = g[0].unwrap();
    let last_group = if g.len() > 1 { g.len() - 1 } else { 1 };
    // the group that closed last: the one that ends furthest on, the outer of two that end
    // together
    let last_closed = (1..g.len())
        .filter_map(|k| g[k].map(|(_, e)| (e, std::cmp::Reverse(k))))
        .max()
        .map_or(usize::MAX, |(_, k)| k.0);
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
                        let close = (f.get(i) == Some(&b'{'))
                            .then(|| f[i..].iter().position(|&b| b == b'}'))
                            .flatten();
                        match close {
                            // an unknown name is an empty group
                            Some(close) => {
                                let n = named(&f[i + 1..i + close]).unwrap_or(usize::MAX);
                                o.put(group(subject, g, n));
                                i += close + 1;
                            }
                            None => o.put(group(subject, g, last_group)),
                        }
                    }
                    _ => {
                        let brace = f[i] == b'{';
                        let save = i;
                        if brace {
                            i += 1;
                        }
                        let after = i;
                        match digits(f, &mut i, usize::MAX) {
                            Some(n) if !brace || f.get(i) == Some(&b'}') => {
                                if brace {
                                    i += 1;
                                }
                                o.put(group(subject, g, n));
                            }
                            _ => {
                                i = after;
                                match perl_verb(f, &mut i, brace) {
                                    Some(Verb::Match) => o.put(&subject[ms..me]),
                                    Some(Verb::Prefix) => o.put(&subject[prefix_start..ms]),
                                    Some(Verb::Suffix) => o.put(&subject[me..]),
                                    Some(Verb::LastGroup) => o.put(group(subject, g, last_group)),
                                    Some(Verb::LastClosed) => o.put(group(subject, g, last_closed)),
                                    None => {
                                        i = save;
                                        o.put(b"$");
                                    }
                                }
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
    fn boost_names_in_formats() {
        assert_eq!(sub("abc", "b", "[$MATCH|${^MATCH}|${MATCH}]"), "a[b|b|b]c");
        assert_eq!(sub("abc", "b", "[$PREMATCH|${^POSTMATCH}]"), "a[a|c]c");
        assert_eq!(sub("ab", "(?<x>a)(?<y>b)", "$+{y}$+{x}$+{none}!"), "ba!");
        assert_eq!(sub("ab", "(a)(b)", "$LAST_PAREN_MATCH$+"), "bb");
        assert_eq!(sub("abc", "((a)b)c", "$^N|$LAST_SUBMATCH_RESULT"), "ab|ab");
        // not a name: the dollar stays
        assert_eq!(sub("a", "a", "$x ${MATCH"), "$x ${MATCH");
    }

    #[test]
    fn boost_syntax_pcre2_lacks() {
        let found = |p: &str, s: &str| {
            let mut re = Regex::new(p, 0).unwrap_or_else(|e| panic!("{p}: {e}"));
            re.search(s.as_bytes(), 0, false)
                .unwrap()
                .map(|g| g[0].map(|(a, b)| s[a..b].to_string()).unwrap())
        };
        assert_eq!(found(r"\u\l+", "żółw Żółw").as_deref(), Some("Żółw"));
        assert_eq!(found(r"\L+", "abc DEF").as_deref(), Some(" DEF"));
        assert_eq!(found(r"[\u\d]+", "ab12CD").as_deref(), Some("12CD"));
        assert_eq!(
            found(r"\p{Uppercase Letter}+", "abCDe").as_deref(),
            Some("CD")
        );
        assert_eq!(found(r"\P{letter}+", "ab12cd").as_deref(), Some("12"));
        assert_eq!(found(r"\p{Lu}\p{Greek}", "aΩAβ").as_deref(), Some("Aβ"));
        assert_eq!(
            found(r"[[:Lu:][:digit:]]+", "ab1C2d").as_deref(),
            Some("1C2")
        );
        assert_eq!(found(r"[[:^l:]]+", "abCDe").as_deref(), Some("CD"));
        assert_eq!(found(r"[[:alpha:]]+\>", "12 abc.").as_deref(), Some("abc"));
        assert_eq!(found(r"[[:unicode:]]+", "abżłów").as_deref(), Some("żł"));
        // quoted text is the text
        assert_eq!(
            found(r"\Q\u[[:l:]]\E", r"x\u[[:l:]]").as_deref(),
            Some(r"\u[[:l:]]")
        );
        // line ends as boost has them
        assert_eq!(found(r"^b$", "a\r\nb\r\nc").as_deref(), Some("b"));
        assert_eq!(found(r"^b$", "a\u{2028}b\u{2028}c").as_deref(), Some("b"));
    }

    #[test]
    fn long_subjects_do_not_run_out_of_stack() {
        let line = "a ".repeat(200_000);
        let mut re = Regex::new(r"^(?:\w+\s?)*$", 0).unwrap();
        assert!(re.search(line.as_bytes(), 0, false).unwrap().is_some());
        let mut re = Regex::new(r"(a|b|\s)+", 0).unwrap();
        let g = re.search(line.as_bytes(), 0, false).unwrap().unwrap();
        assert_eq!(g[0], Some((0, line.len())));
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
