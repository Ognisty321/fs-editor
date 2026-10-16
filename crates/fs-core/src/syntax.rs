//! Aegisub's syntax highlighting of a line's text (libaegisub's TokenizeDialogueBody, MarkDrawings,
//! the drawing half of SplitWords and SyntaxHighlight), made anew as a hand-written lexer with the
//! same longest-match rules: tags, their parameters and punctuation, comments, line breaks,
//! karaoke templater code, and drawings down to their commands, coordinates and curve endpoints,
//! with what VSFilter would not accept marked as an error.

/// What a piece of the text is drawn as (Aegisub's SyntaxStyle, in its order).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum Style {
    Normal,
    Comment,
    DrawingCmd,
    DrawingX,
    DrawingY,
    DrawingEndpointX,
    DrawingEndpointY,
    /// `{` and `}`
    Override,
    /// `\`, `(`, `)` and `,`
    Punctuation,
    Tag,
    Error,
    Parameter,
    LineBreak,
    KaraokeTemplate,
    KaraokeVariable,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum Tok {
    Text,
    LineBreak,
    OvrBegin,
    OvrEnd,
    TagStart,
    TagName,
    OpenParen,
    CloseParen,
    ArgSep,
    Arg,
    Error,
    Comment,
    Whitespace,
    DrawingFull,
    DrawingCmd,
    DrawingX,
    DrawingY,
    DrawingEndpointX,
    DrawingEndpointY,
    KaraokeTemplate,
    KaraokeVariable,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Token {
    pub ty: Tok,
    pub len: usize,
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    Initial,
    Ovr,
    Arg,
    TagStart,
    TagName,
}

fn is_space(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c)
}

/// The karaoke templater's `!expression!` or `$variable` at the start of `s`.
fn templater(s: &[u8]) -> Option<(Tok, usize)> {
    match s.first()? {
        b'!' => s[1..]
            .iter()
            .position(|&b| b == b'!')
            .map(|p| (Tok::KaraokeTemplate, p + 2)),
        b'$' => {
            let n = s[1..]
                .iter()
                .take_while(|b| b.is_ascii_alphabetic() || **b == b'_')
                .count();
            (n > 0).then_some((Tok::KaraokeVariable, n + 1))
        }
        _ => None,
    }
}

/// The lexer: each step takes the longest of the rules of the state it is in, the first rule on a
/// tie; neighbouring tokens of one type are one token.
pub(crate) fn tokenize(text: &str, template: bool) -> Vec<Token> {
    use Tok::*;
    let b = text.as_bytes();
    let mut out: Vec<Token> = Vec::new();
    let mut state = State::Initial;
    let mut depth = 0i32;
    let mut i = 0;
    while i < b.len() {
        let rest = &b[i..];
        let c = rest[0];
        let one = text[i..].chars().next().map_or(1, char::len_utf8); // the lexer's "."
        let kara = if template { templater(rest) } else { None };
        let ws = rest.iter().take_while(|&&x| is_space(x)).count();
        let lower = rest.iter().take_while(|x| x.is_ascii_lowercase()).count();
        let (ty, len) = match state {
            State::Initial => match c {
                b'\\' if matches!(rest.get(1), Some(b'n' | b'N' | b'h')) => (LineBreak, 2),
                b'{' => {
                    depth = 0;
                    state = State::Ovr;
                    (OvrBegin, 1)
                }
                _ => kara.unwrap_or((Text, one)),
            },
            State::Ovr => match c {
                b'{' => (Error, 1),
                b'}' => {
                    state = State::Initial;
                    (OvrEnd, 1)
                }
                b'\\' => {
                    state = State::TagStart;
                    (TagStart, 1)
                }
                _ if ws > 0 => (Whitespace, ws),
                _ => kara.unwrap_or((Comment, one)),
            },
            State::Arg => match c {
                b'{' => (Error, 1),
                b'}' => {
                    state = State::Initial;
                    (OvrEnd, 1)
                }
                b'(' => {
                    depth += 1;
                    (OpenParen, 1)
                }
                b')' => {
                    depth -= 1;
                    if depth == 0 {
                        state = State::Ovr;
                    }
                    (CloseParen, 1)
                }
                b'\\' => {
                    state = State::TagStart;
                    (TagStart, 1)
                }
                b',' => (ArgSep, 1),
                _ if ws > 0 => (Whitespace, ws),
                _ => kara.unwrap_or((Arg, one)),
            },
            State::TagStart => match c {
                _ if ws > 0 => (Whitespace, ws),
                // \fn and \r take any text as their argument
                b'f' if rest.get(1) == Some(&b'n') => {
                    state = State::Arg;
                    (TagName, 2)
                }
                b'r' => {
                    state = State::Arg;
                    (TagName, 1)
                }
                b'\\' => (TagStart, 1),
                b'}' => {
                    state = State::Initial;
                    (OvrEnd, 1)
                }
                b'a'..=b'z' | b'0'..=b'9' => {
                    state = State::TagName;
                    (TagName, 1)
                }
                _ => match kara {
                    Some(k) => k,
                    None => {
                        state = State::Ovr;
                        (Comment, one)
                    }
                },
            },
            State::TagName => match c {
                _ if lower > 0 => {
                    state = State::Arg;
                    (TagName, lower)
                }
                b'(' => {
                    depth += 1;
                    state = State::Arg;
                    (OpenParen, 1)
                }
                b')' => {
                    depth -= 1;
                    if depth == 0 {
                        state = State::Ovr;
                    }
                    (CloseParen, 1)
                }
                b'}' => {
                    state = State::Initial;
                    (OvrEnd, 1)
                }
                b'\\' => {
                    state = State::TagStart;
                    (TagStart, 1)
                }
                _ => match kara {
                    Some(k) => k,
                    None => {
                        state = State::Arg;
                        (Arg, one)
                    }
                },
            },
        };
        match out.last_mut() {
            Some(t) if t.ty == ty => t.len += len,
            _ => out.push(Token { ty, len }),
        }
        i += len;
    }
    out
}

/// The text of drawings (after `\p` with a non-zero scale, and a vector `\clip`'s argument) as
/// drawings; everything after the last `}` as text, as VSFilter reads an unclosed block.
fn mark_drawings(s: &[u8], t: &mut Vec<Token>) {
    use Tok::*;
    let mut last = t.iter().rposition(|k| k.ty == OvrEnd).map_or(0, |p| p + 1);
    let mut pos = 0;
    let mut drawing = false;
    let mut i = 0;
    while i < last {
        let len = t[i].len;
        match t[i].ty {
            Text if drawing => t[i].ty = DrawingFull,
            TagName => {
                if i + 3 < t.len()
                    && (len == 4 || len == 5)
                    && s[pos..pos + len].ends_with(b"clip")
                    && t[i + 1].ty == OpenParen
                {
                    // one argument (or a scale and the drawing) is a vector clip, four a rectangle
                    let (mut start, mut end) = (0, 0);
                    for (j, k) in t.iter().enumerate().skip(i + 2) {
                        match k.ty {
                            ArgSep if start != 0 => break,
                            ArgSep => start = j + 1,
                            CloseParen => {
                                end = j;
                                break;
                            }
                            Whitespace | Arg => {}
                            _ => break,
                        }
                    }
                    if end != 0 {
                        if start == 0 {
                            start = i + 2;
                        }
                        if end != start {
                            let n = t[start..end].iter().map(|k| k.len).sum();
                            t[start] = Token {
                                ty: DrawingFull,
                                len: n,
                            };
                            t.drain(start + 1..end);
                            last -= end - start - 1;
                        }
                    }
                }
                if len == 1 && i + 1 < t.len() && s[pos] == b'p' {
                    drawing = false;
                    if i + 1 != last && t[i + 1].ty == Arg {
                        // VSFilter allows leading zeros in the scale
                        for &c in &s[pos + len..pos + len + t[i + 1].len] {
                            if (b'1'..=b'9').contains(&c) {
                                drawing = true;
                            } else if c != b'0' {
                                break;
                            }
                        }
                    }
                }
            }
            _ => {}
        }
        pos += len;
        i += 1;
    }
    let mut i = last;
    while i < t.len() {
        if !matches!(t[i].ty, KaraokeTemplate | KaraokeVariable | LineBreak) {
            t[i].ty = if drawing { DrawingFull } else { Text };
            if i > 0 && t[i - 1].ty == t[i].ty {
                t[i - 1].len += t[i].len;
                t.remove(i);
                continue;
            }
        }
        i += 1;
    }
}

/// A drawing split at its spaces, each word labelled: a command, a coordinate (of a curve's
/// endpoint), or an error (a command glued to something, an odd number of coordinates, no `m` first).
fn split_drawing(s: &[u8], t: &mut Vec<Token>, i: &mut usize, pos: usize) {
    use Tok::*;
    let start = *i;
    let ws = |c: u8| c == b' ' || c == b'\t';
    let (mut at, mut run, mut space) = (pos, 0, ws(s[pos]));
    while run < t[*i].len {
        if ws(s[at]) != space {
            space = !space;
            let rest = t[*i].len - run;
            t[*i] = Token {
                ty: if space { DrawingFull } else { Whitespace },
                len: run,
            };
            t.insert(
                *i + 1,
                Token {
                    ty: if space { Whitespace } else { DrawingFull },
                    len: rest,
                },
            );
            *i += 1;
            run = 0;
        }
        run += 1;
        at += 1;
    }
    let (mut at, mut coords, mut cmd) = (pos, 0, b' ');
    for k in &mut t[start..=*i] {
        let c = s[at];
        if k.ty == Whitespace {
        } else if cmd == b' ' && c != b'm' {
            k.ty = Error;
        } else if b"mnlsbpc".contains(&c) {
            k.ty = if k.len != 1 || coords % 2 != 0 {
                Error
            } else {
                DrawingCmd
            };
            cmd = c;
            coords = 0;
        } else {
            let number = s[at..at + k.len]
                .iter()
                .all(|c| c.is_ascii_digit() || b".+-eE".contains(c));
            k.ty = match (number, cmd == b'b' && coords % 6 >= 4, coords % 2 == 0) {
                (false, ..) => Error,
                (true, true, true) => DrawingEndpointX,
                (true, true, false) => DrawingEndpointY,
                (true, false, true) => DrawingX,
                (true, false, false) => DrawingY,
            };
            coords += 1;
        }
        at += k.len;
    }
}

/// The styles of `text` as runs of (style, length in bytes) covering all of it. `template`: the
/// line is a karaoke template (a comment whose effect starts with "template"), whose `!…!` and
/// `$name` are the templater's.
pub fn highlight(text: &str, template: bool) -> Vec<(Style, usize)> {
    let s = text.as_bytes();
    let mut t = tokenize(text, template);
    mark_drawings(s, &mut t);
    let (mut i, mut pos) = (0, 0);
    while i < t.len() {
        let len = t[i].len;
        if t[i].ty == Tok::DrawingFull {
            split_drawing(s, &mut t, &mut i, pos);
        }
        pos += len;
        i += 1;
    }
    let mut out: Vec<(Style, usize)> = Vec::new();
    for k in t {
        let style = match k.ty {
            Tok::KaraokeTemplate => Style::KaraokeTemplate,
            Tok::KaraokeVariable => Style::KaraokeVariable,
            Tok::LineBreak => Style::LineBreak,
            Tok::Error => Style::Error,
            Tok::Arg => Style::Parameter,
            Tok::Comment => Style::Comment,
            Tok::DrawingCmd => Style::DrawingCmd,
            Tok::DrawingX => Style::DrawingX,
            Tok::DrawingY => Style::DrawingY,
            Tok::DrawingEndpointX => Style::DrawingEndpointX,
            Tok::DrawingEndpointY => Style::DrawingEndpointY,
            Tok::Text | Tok::DrawingFull => Style::Normal, // split_drawing leaves no DrawingFull
            Tok::TagName => Style::Tag,
            Tok::OpenParen | Tok::CloseParen | Tok::ArgSep | Tok::TagStart => Style::Punctuation,
            Tok::OvrBegin | Tok::OvrEnd => Style::Override,
            // a parameter's spaces are the parameter's; an endpoint's join its x and y underlined
            Tok::Whitespace => match out.last() {
                Some((Style::Parameter, _)) => Style::Parameter,
                Some((Style::DrawingEndpointX, _)) => Style::DrawingEndpointX,
                _ => Style::Normal,
            },
        };
        match out.last_mut() {
            Some((st, l)) if *st == style => *l += k.len,
            _ => out.push((style, k.len)),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    // Aegisub's tests/tests/syntax_highlight.cpp, without its spell checker (its SPELLING is NORMAL)
    use super::Style::*;
    use super::*;

    fn check(text: &str, template: bool, want: &[(Style, usize)]) {
        assert_eq!(highlight(text, template), want, "{text}");
    }

    #[test]
    fn empty() {
        assert!(highlight("", false).is_empty());
    }

    #[test]
    fn drawing() {
        check(
            "incorrect{\\clip(m 10 10 l 20 20 c)\\p1}m 10 10 b 0 0 0 100 100 0{\\p}correct",
            false,
            &[
                (Normal, 9),
                (Override, 1),
                (Punctuation, 1),
                (Tag, 4),
                (Punctuation, 1),
                (DrawingCmd, 1),
                (Normal, 1),
                (DrawingX, 2),
                (Normal, 1),
                (DrawingY, 2),
                (Normal, 1),
                (DrawingCmd, 1),
                (Normal, 1),
                (DrawingX, 2),
                (Normal, 1),
                (DrawingY, 2),
                (Normal, 1),
                (DrawingCmd, 1),
                (Punctuation, 2),
                (Tag, 1),
                (Parameter, 1),
                (Override, 1),
                (DrawingCmd, 1),
                (Normal, 1),
                (DrawingX, 2),
                (Normal, 1),
                (DrawingY, 2),
                (Normal, 1),
                (DrawingCmd, 1),
                (Normal, 1),
                (DrawingX, 1),
                (Normal, 1),
                (DrawingY, 1),
                (Normal, 1),
                (DrawingX, 1),
                (Normal, 1),
                (DrawingY, 3),
                (Normal, 1),
                (DrawingEndpointX, 4),
                (DrawingEndpointY, 1),
                (Override, 1),
                (Punctuation, 1),
                (Tag, 1),
                (Override, 1),
                (Normal, 7),
            ],
        );
    }

    #[test]
    fn drawing_without_m() {
        check(
            "{\\p1}l 100 100 0 100",
            false,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 1),
                (Parameter, 1),
                (Override, 1),
                (Error, 1),
                (Normal, 1),
                (Error, 3),
                (Normal, 1),
                (Error, 3),
                (Normal, 1),
                (Error, 1),
                (Normal, 1),
                (Error, 3),
            ],
        );
    }

    #[test]
    fn transform() {
        check(
            "{\\t(0, 0, \\clip(0,0,10,10)}clipped text",
            false,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 1),
                (Punctuation, 1),
                (Parameter, 1),
                (Punctuation, 1),
                (Normal, 1),
                (Parameter, 1),
                (Punctuation, 1),
                (Normal, 1),
                (Punctuation, 1),
                (Tag, 4),
                (Punctuation, 1),
                (Parameter, 1),
                (Punctuation, 1),
                (Parameter, 1),
                (Punctuation, 1),
                (Parameter, 2),
                (Punctuation, 1),
                (Parameter, 2),
                (Punctuation, 1),
                (Override, 1),
                (Normal, 12),
            ],
        );
    }

    #[test]
    fn unclosed_comment_linebreak() {
        check(
            "{\\incorrect}{\\incorrect",
            false,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 9),
                (Override, 1),
                (Normal, 11),
            ],
        );
        check(
            "abc{def}ghi",
            false,
            &[
                (Normal, 3),
                (Override, 1),
                (Comment, 3),
                (Override, 1),
                (Normal, 3),
            ],
        );
        check(
            "a\\Nb\\nc\\hd\\N\\N",
            false,
            &[
                (Normal, 1),
                (LineBreak, 2),
                (Normal, 1),
                (LineBreak, 2),
                (Normal, 1),
                (LineBreak, 2),
                (Normal, 1),
                (LineBreak, 4),
            ],
        );
        check(
            "{\\fnComic Sans MS}",
            false,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 2),
                (Parameter, 13),
                (Override, 1),
            ],
        );
    }

    #[test]
    fn templater() {
        check(
            "{\\pos($x, $y)\\fs!10 + 10!}abc",
            false,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 3),
                (Punctuation, 1),
                (Parameter, 2),
                (Punctuation, 1),
                (Normal, 1),
                (Parameter, 2),
                (Punctuation, 2),
                (Tag, 2),
                (Parameter, 9),
                (Override, 1),
                (Normal, 3),
            ],
        );
        check("$a", true, &[(KaraokeVariable, 2)]);
        check(
            "{\\pos($x,$y)}a",
            true,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 3),
                (Punctuation, 1),
                (KaraokeVariable, 2),
                (Punctuation, 1),
                (KaraokeVariable, 2),
                (Punctuation, 1),
                (Override, 1),
                (Normal, 1),
            ],
        );
        check(
            "{\\fn$fn}a",
            true,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 2),
                (KaraokeVariable, 3),
                (Override, 1),
                (Normal, 1),
            ],
        );
        check(
            "{foo$bar}",
            true,
            &[
                (Override, 1),
                (Comment, 3),
                (KaraokeVariable, 4),
                (Override, 1),
            ],
        );
        check("{foo$bar", true, &[(Normal, 4), (KaraokeVariable, 4)]);
        check("!5!", true, &[(KaraokeTemplate, 3)]);
        check("!5", true, &[(Normal, 2)]);
        check("!x * 10!", true, &[(KaraokeTemplate, 8)]);
        check(
            "{\\pos(!x + 1!, $y)}a",
            true,
            &[
                (Override, 1),
                (Punctuation, 1),
                (Tag, 3),
                (Punctuation, 1),
                (KaraokeTemplate, 7),
                (Punctuation, 1),
                (Normal, 1),
                (KaraokeVariable, 2),
                (Punctuation, 1),
                (Override, 1),
                (Normal, 1),
            ],
        );
        check(
            "{\\b1!'}'!a",
            true,
            &[(Normal, 4), (KaraokeTemplate, 5), (Normal, 1)],
        );
        check(
            "!1!2!3!",
            true,
            &[(KaraokeTemplate, 3), (Normal, 1), (KaraokeTemplate, 3)],
        );
    }

    #[test]
    fn multibyte_text_keeps_byte_lengths() {
        check(
            "żółw{\\i1}ćma",
            false,
            &[
                (Normal, "żółw".len()),
                (Override, 1),
                (Punctuation, 1),
                (Tag, 1),
                (Parameter, 1),
                (Override, 1),
                (Normal, "ćma".len()),
            ],
        );
    }
}
