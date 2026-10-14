//! Keyframe list files: Aegisub (`# keyframe format v1`), XviD/ffmpeg/avconv
//! 2-pass stats, DivX map, x264 stats and WWXD/SCXviD qpfiles.

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum KeyframeError {
    UnknownFormat,
    Parse(String),
}

impl fmt::Display for KeyframeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            KeyframeError::UnknownFormat => {
                f.write_str("file header does not match any known keyframe format")
            }
            KeyframeError::Parse(s) => write!(f, "keyframe parse error: {s}"),
        }
    }
}

impl std::error::Error for KeyframeError {}

/// Parse a keyframe file and return the sorted list of keyframe numbers.
pub fn parse(text: &str) -> Result<Vec<i64>, KeyframeError> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let mut lines = text.lines().map(|l| l.trim_end_matches('\r'));
    let header = lines.next().ok_or(KeyframeError::UnknownFormat)?;

    if header == "# keyframe format v1" {
        // second line: "fps <value>", then one frame number per line
        let mut out = Vec::new();
        let mut seen_fps = false;
        for l in lines {
            let t = l.trim();
            if t.is_empty() || t.starts_with('#') {
                continue;
            }
            if !seen_fps && t.starts_with("fps") {
                seen_fps = true;
                continue;
            }
            if let Ok(v) = t.parse::<i64>() {
                out.push(v);
            }
        }
        return Ok(out);
    }
    if header.starts_with("# XviD 2pass stat file")
        || header.starts_with("# ffmpeg 2-pass log file, using xvid codec")
        || header.starts_with("# avconv 2-pass log file, using xvid codec")
    {
        return Ok(enumerated(lines, |l| l.chars().next()));
    }
    if header.starts_with("##map version") {
        return Ok(enumerated(lines, |l| {
            ['I', 'P', 'B'].iter().find_map(|c| l.find(*c).map(|_| *c))
        }));
    }
    if header.starts_with("#options:") {
        return Ok(enumerated(lines, |l| {
            let p = l.find("type:")?;
            l[p + 5..].chars().next()
        }));
    }
    if header.starts_with("# WWXD log file, using qpfile format") {
        let mut out = Vec::new();
        for l in lines {
            if l.is_empty() || l.starts_with('#') {
                continue;
            }
            let mut it = l.split_whitespace();
            let (Some(n), Some(t)) = (it.next(), it.next()) else {
                return Err(KeyframeError::Parse(
                    "WWXD keyframe file not in qpfile format".into(),
                ));
            };
            let n: i64 = n.parse().map_err(|_| {
                KeyframeError::Parse("WWXD keyframe file not in qpfile format".into())
            })?;
            if t.starts_with('I') {
                out.push(n);
            }
        }
        return Ok(out);
    }
    Err(KeyframeError::UnknownFormat)
}

fn enumerated<'a>(
    lines: impl Iterator<Item = &'a str>,
    f: impl Fn(&str) -> Option<char>,
) -> Vec<i64> {
    let mut count = 0i64;
    let mut out = Vec::new();
    for l in lines {
        match f(l).map(|c| c.to_ascii_lowercase()) {
            Some('i') => {
                out.push(count);
                count += 1;
            }
            Some('p') | Some('b') => count += 1,
            _ => {}
        }
    }
    out
}

/// Serialise in Aegisub's keyframe format.
pub fn write(keyframes: &[i64]) -> String {
    let mut s = String::from("# keyframe format v1\nfps 0\n");
    for k in keyframes {
        s.push_str(&k.to_string());
        s.push('\n');
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip() {
        let k = vec![0, 10, 20, 35];
        assert_eq!(parse(&write(&k)).unwrap(), k);
    }

    #[test]
    fn unknown() {
        assert_eq!(parse(""), Err(KeyframeError::UnknownFormat));
        assert_eq!(parse("hello\n1\n"), Err(KeyframeError::UnknownFormat));
    }
}
