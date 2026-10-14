//! Line splitting that remembers each line's terminator, so files can be
//! written back byte-for-byte.

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum LineEnding {
    #[default]
    CrLf,
    Lf,
    Cr,
    /// Last line of a file without a trailing newline.
    None,
}

impl LineEnding {
    pub const fn as_str(self) -> &'static str {
        match self {
            LineEnding::CrLf => "\r\n",
            LineEnding::Lf => "\n",
            LineEnding::Cr => "\r",
            LineEnding::None => "",
        }
    }

    /// Platform-neutral default for newly written files (matches Aegisub,
    /// which always writes CRLF).
    pub const fn default_for_new() -> Self {
        LineEnding::CrLf
    }
}

/// A line exactly as it appeared in the source file.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Default)]
pub struct RawLine {
    pub text: String,
    pub ending: LineEnding,
}

impl RawLine {
    pub fn new(text: impl Into<String>, ending: LineEnding) -> Self {
        RawLine {
            text: text.into(),
            ending,
        }
    }

    pub fn write_to(&self, out: &mut String) {
        out.push_str(&self.text);
        out.push_str(self.ending.as_str());
    }
}

/// Split text into lines, keeping the exact terminator of each.
pub fn split_lines(s: &str) -> Vec<(&str, LineEnding)> {
    let mut out = Vec::new();
    let b = s.as_bytes();
    let mut start = 0;
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'\n' => {
                out.push((&s[start..i], LineEnding::Lf));
                i += 1;
                start = i;
            }
            b'\r' => {
                if i + 1 < b.len() && b[i + 1] == b'\n' {
                    out.push((&s[start..i], LineEnding::CrLf));
                    i += 2;
                } else {
                    out.push((&s[start..i], LineEnding::Cr));
                    i += 1;
                }
                start = i;
            }
            _ => i += 1,
        }
    }
    if start < b.len() {
        out.push((&s[start..], LineEnding::None));
    }
    out
}

/// Most common line ending among the given lines (CRLF if none).
pub fn dominant_ending<'a>(endings: impl IntoIterator<Item = &'a LineEnding>) -> LineEnding {
    let (mut crlf, mut lf, mut cr) = (0usize, 0usize, 0usize);
    for e in endings {
        match e {
            LineEnding::CrLf => crlf += 1,
            LineEnding::Lf => lf += 1,
            LineEnding::Cr => cr += 1,
            LineEnding::None => {}
        }
    }
    if lf > crlf && lf >= cr {
        LineEnding::Lf
    } else if cr > crlf && cr > lf {
        LineEnding::Cr
    } else {
        LineEnding::CrLf
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn split() {
        let v = split_lines("a\r\nb\nc\rd");
        assert_eq!(
            v,
            vec![
                ("a", LineEnding::CrLf),
                ("b", LineEnding::Lf),
                ("c", LineEnding::Cr),
                ("d", LineEnding::None)
            ]
        );
        assert_eq!(split_lines("a\n"), vec![("a", LineEnding::Lf)]);
        assert_eq!(split_lines(""), vec![]);
        assert_eq!(
            split_lines("\n\n"),
            vec![("", LineEnding::Lf), ("", LineEnding::Lf)]
        );
    }
}
