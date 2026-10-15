//! Line-based formats: SRT, MPL2, MicroDVD and TMP.
//!
//! Each parser produces [`Sub`] items whose text is already converted to ASS
//! markup; each writer takes ASS text and converts it back.

use fs_core::Time;
use fs_core::vfr::{Framerate, TimeType};

use crate::markup::{Fmt, Run, ass_to_runs, html_to_runs, runs_to_ass, runs_to_html};

#[derive(Debug, Clone, PartialEq)]
pub struct Sub {
    pub start: Time,
    pub end: Time,
    /// ASS text (override tags, `\N`).
    pub text: String,
}

fn lines_of(text: &str) -> impl Iterator<Item = &str> {
    text.lines().map(|l| l.trim_end_matches('\r'))
}

// ---------------------------------------------------------------------------
// SRT
// ---------------------------------------------------------------------------

/// Parse SubRip. Tolerant of missing indices, extra blank lines, `.` as the
/// millisecond separator, coordinates after the timing and WebVTT headers.
pub fn parse_srt(text: &str) -> Vec<Sub> {
    let mut out = Vec::new();
    let lines: Vec<&str> = lines_of(text).collect();
    let mut i = 0;
    while i < lines.len() {
        let l = lines[i];
        if let Some((a, b)) = l.split_once("-->") {
            let start = Time::parse_srt(a.trim());
            let end = Time::parse_srt(b.split_whitespace().next().unwrap_or(""));
            if let (Some(start), Some(end)) = (start, end) {
                i += 1;
                // the timing or the index and timing of the next cue
                let next_cue = |k: usize| {
                    is_srt_timing(lines[k])
                        || (!lines[k].trim().is_empty()
                            && lines[k].trim().chars().all(|c| c.is_ascii_digit())
                            && lines.get(k + 1).is_some_and(|n| is_srt_timing(n)))
                };
                let mut body: Vec<&str> = Vec::new();
                while i < lines.len() && !next_cue(i) {
                    if lines[i].trim().is_empty() {
                        // blank lines end the cue unless more of its text follows: they are
                        // its line breaks then (Aegisub)
                        match (i..lines.len()).find(|&k| !lines[k].trim().is_empty()) {
                            Some(j) if !next_cue(j) => {
                                body.extend(&lines[i..j]);
                                i = j;
                            }
                            _ => break,
                        }
                    }
                    body.push(lines[i]);
                    i += 1;
                }
                out.push(Sub {
                    start,
                    end,
                    text: runs_to_ass(&html_to_runs(&body.join("\n")), None),
                });
                continue;
            }
        }
        i += 1;
    }
    out
}

fn is_srt_timing(l: &str) -> bool {
    l.split_once("-->").is_some_and(|(a, b)| {
        Time::parse_srt(a.trim()).is_some()
            && Time::parse_srt(b.split_whitespace().next().unwrap_or("")).is_some()
    })
}

pub fn write_srt(subs: &[Sub]) -> String {
    let mut s = String::new();
    for (n, sub) in subs.iter().enumerate() {
        let (runs, an) = ass_to_runs(&sub.text);
        let body = runs_to_html(&runs, an);
        s.push_str(&format!(
            "{}\r\n{} --> {}\r\n",
            n + 1,
            sub.start.to_srt(),
            sub.end.to_srt()
        ));
        // a blank line would end the cue: drop empty lines inside the text
        for l in body.split('\n').filter(|l| !l.trim().is_empty()) {
            s.push_str(l);
            s.push_str("\r\n");
        }
        s.push_str("\r\n");
    }
    s
}

// ---------------------------------------------------------------------------
// MPL2: [start][end]text, deciseconds, '|' newline, leading '/' italics
// ---------------------------------------------------------------------------

pub fn parse_mpl2(text: &str) -> Vec<Sub> {
    let mut out: Vec<Sub> = Vec::new();
    for l in lines_of(text) {
        let Some((start, rest)) = bracket_num(l, '[', ']') else {
            continue;
        };
        let (end, body) = match bracket_num(rest, '[', ']') {
            Some((e, b)) => (Some(e), b),
            None => match rest.strip_prefix("[]") {
                Some(b) => (None, b),
                None => continue,
            },
        };
        let start = Time(start * 100);
        let end = end.map(|e| Time(e * 100)).unwrap_or(start + Time(3000));
        out.push(Sub {
            start,
            end,
            text: slash_lines_to_ass(body, '/'),
        });
    }
    out
}

pub fn write_mpl2(subs: &[Sub]) -> String {
    let mut s = String::new();
    for sub in subs {
        s.push_str(&format!(
            "[{}][{}]{}\r\n",
            (sub.start.ms() + 50) / 100,
            (sub.end.ms() + 50) / 100,
            ass_to_slash_lines(&sub.text, "/")
        ));
    }
    s
}

/// `/` (MPL2) or `{y:i}` (MicroDVD per line) italics per `|`-separated line.
fn slash_lines_to_ass(body: &str, italic_prefix: char) -> String {
    let mut runs = Vec::new();
    for (k, part) in body.split('|').enumerate() {
        let (i, t) = match part.strip_prefix(italic_prefix) {
            Some(t) => (true, t),
            None => (false, part),
        };
        let text = if k > 0 {
            format!("\n{t}")
        } else {
            t.to_string()
        };
        runs.push(Run {
            fmt: Fmt {
                i,
                ..Default::default()
            },
            text,
        });
    }
    // a line break belongs to the previous formatting so tags are minimal
    for k in 1..runs.len() {
        if runs[k].text.starts_with('\n') && runs[k].fmt != runs[k - 1].fmt {
            runs[k].text.remove(0);
            runs[k - 1].text.push('\n');
        }
    }
    runs_to_ass(&runs, None)
}

fn ass_to_slash_lines(text: &str, italic: &str) -> String {
    let (runs, _) = ass_to_runs(text);
    // per output line: italic if every non-empty run on it is italic
    let mut lines: Vec<(String, bool, bool)> = vec![(String::new(), true, false)];
    for r in &runs {
        for (k, piece) in r.text.split('\n').enumerate() {
            if k > 0 {
                lines.push((String::new(), true, false));
            }
            let cur = lines.last_mut().unwrap();
            cur.0.push_str(piece);
            if !piece.trim().is_empty() {
                cur.1 &= r.fmt.i;
                cur.2 = true;
            }
        }
    }
    lines
        .into_iter()
        .map(|(t, it, any)| if it && any { format!("{italic}{t}") } else { t })
        .collect::<Vec<_>>()
        .join("|")
}

fn bracket_num(s: &str, open: char, close: char) -> Option<(i64, &str)> {
    let r = s.trim_start().strip_prefix(open)?;
    let e = r.find(close)?;
    let n: i64 = r[..e].trim().parse().ok()?;
    Some((n, &r[e + close.len_utf8()..]))
}

// ---------------------------------------------------------------------------
// MicroDVD: {start}{end}text in frames
// ---------------------------------------------------------------------------

/// Parse MicroDVD. If the first line is `{1}{1}<fps>` that frame rate is
/// used (and returned); otherwise `default_fps`.
pub fn parse_microdvd(text: &str, default_fps: &Framerate) -> (Vec<Sub>, Option<f64>) {
    let mut out = Vec::new();
    let mut fps_decl = None;
    let mut fr = default_fps.clone();
    for (n, l) in lines_of(text).enumerate() {
        let l = if n == 0 {
            l.trim_start_matches('\u{feff}')
        } else {
            l
        };
        let Some((sf, rest)) = bracket_num(l, '{', '}') else {
            continue;
        };
        let (ef, body) = match bracket_num(rest, '{', '}') {
            Some((e, b)) => (Some(e), b),
            None => match rest.strip_prefix("{}") {
                Some(b) => (None, b),
                None => continue,
            },
        };
        if out.is_empty()
            && fps_decl.is_none()
            && sf <= 1
            && ef.is_some_and(|e| e <= 1)
            && let Ok(v) = body.trim().replace(',', ".").parse::<f64>()
            && v > 0.0
            && let Ok(f) = Framerate::from_fps(v)
        {
            fps_decl = Some(v);
            fr = f;
            continue;
        }
        let start = Time(fr.time_at_frame(sf, TimeType::Start).max(0));
        let end = match ef {
            Some(e) => Time(fr.time_at_frame(e, TimeType::End).max(0)),
            None => start + Time(3000),
        };
        out.push(Sub {
            start,
            end,
            text: microdvd_to_ass(body),
        });
    }
    (out, fps_decl)
}

fn microdvd_to_ass(body: &str) -> String {
    let mut runs: Vec<Run> = Vec::new();
    let mut global = Fmt::default();
    for (k, part) in body.split('|').enumerate() {
        let mut fmt = global.clone();
        let mut rest = part;
        // leading control codes: {y:i}, {Y:b}, {c:$BBGGRR}, {f:..}, {s:..}
        while let Some(r) = rest.strip_prefix('{') {
            let Some(e) = r.find('}') else { break };
            let code = &r[..e];
            let Some((key, val)) = code.split_once(':') else {
                break;
            };
            let upper = key.chars().next().is_some_and(|c| c.is_ascii_uppercase());
            let target = if upper { &mut global } else { &mut fmt };
            match key.to_ascii_lowercase().as_str() {
                "y" => {
                    for c in val.chars() {
                        match c.to_ascii_lowercase() {
                            'i' => target.i = true,
                            'b' => target.b = true,
                            'u' => target.u = true,
                            's' => target.s = true,
                            _ => {}
                        }
                    }
                }
                "c" => {
                    let hex = val.trim().trim_start_matches('$');
                    if let Ok(v) = u32::from_str_radix(hex, 16) {
                        target.color = Some(fs_core::Color::rgb(
                            v as u8,
                            (v >> 8) as u8,
                            (v >> 16) as u8,
                        ));
                    }
                }
                "f" => target.face = Some(val.trim().to_string()).filter(|f| !f.is_empty()),
                "s" => target.size = val.trim().parse().ok().filter(|s: &f64| *s > 0.0),
                "p" | "o" | "h" => {}
                _ => break,
            }
            if upper {
                fmt = merge(&global, &fmt);
            }
            rest = &r[e + 1..];
        }
        // '/' at the start of a line is italics in many MicroDVD files too
        if let Some(r) = rest.strip_prefix('/') {
            fmt.i = true;
            rest = r;
        }
        let text = if k > 0 {
            format!("\n{rest}")
        } else {
            rest.to_string()
        };
        runs.push(Run { fmt, text });
    }
    for k in 1..runs.len() {
        if runs[k].text.starts_with('\n') && runs[k].fmt != runs[k - 1].fmt {
            runs[k].text.remove(0);
            runs[k - 1].text.push('\n');
        }
    }
    runs_to_ass(&runs, None)
}

fn merge(g: &Fmt, l: &Fmt) -> Fmt {
    Fmt {
        i: g.i || l.i,
        b: g.b || l.b,
        u: g.u || l.u,
        s: g.s || l.s,
        color: l.color.or(g.color),
        face: l.face.clone().or_else(|| g.face.clone()),
        size: l.size.or(g.size),
    }
}

pub fn write_microdvd(subs: &[Sub], fr: &Framerate, declare_fps: bool) -> String {
    let mut s = String::new();
    if declare_fps {
        s.push_str(&format!(
            "{{1}}{{1}}{}\r\n",
            fs_core::model::fmt_f64((fr.fps() * 1000.0).round() / 1000.0)
        ));
    }
    for sub in subs {
        let sf = fr.frame_at_time(sub.start.ms(), TimeType::Start);
        let ef = fr.frame_at_time(sub.end.ms(), TimeType::End);
        let body = ass_to_slash_lines(&sub.text, "{y:i}");
        s.push_str(&format!("{{{sf}}}{{{ef}}}{body}\r\n"));
    }
    s
}

// ---------------------------------------------------------------------------
// TMP: H:MM:SS:text (no end times)
// ---------------------------------------------------------------------------

/// Parse TMPlayer. End time = next line's start, capped at `max_duration`.
pub fn parse_tmp(text: &str, max_duration: Time) -> Vec<Sub> {
    let mut starts: Vec<(Time, String)> = Vec::new();
    for l in lines_of(text) {
        let l = l.trim_start_matches('\u{feff}');
        // H:MM:SS:text, HH:MM:SS=text, H:MM:SS,1=text variants
        let b = l.as_bytes();
        let mut fields = [0i64; 3];
        let mut fi = 0;
        let mut p = 0;
        let mut ok = false;
        while p < b.len() && fi < 3 {
            let s = p;
            while p < b.len() && b[p].is_ascii_digit() {
                p += 1;
            }
            if s == p {
                break;
            }
            fields[fi] = l[s..p].parse().unwrap_or(0);
            fi += 1;
            if fi == 3 {
                // skip optional ",N" and the separator ':' or '='
                if p < b.len() && b[p] == b',' {
                    p += 1;
                    while p < b.len() && b[p].is_ascii_digit() {
                        p += 1;
                    }
                }
                if p < b.len() && (b[p] == b':' || b[p] == b'=') {
                    p += 1;
                    ok = true;
                }
                break;
            }
            if p < b.len() && b[p] == b':' {
                p += 1;
            } else {
                break;
            }
        }
        if !ok {
            continue;
        }
        let t = Time::from_hmsms(fields[0], fields[1], fields[2], 0);
        starts.push((t, slash_lines_to_ass(&l[p..], '/')));
    }
    let mut out = Vec::new();
    for k in 0..starts.len() {
        let (s, ref text) = starts[k];
        if text.trim().is_empty() {
            continue;
        }
        let next = starts.get(k + 1).map(|n| n.0).unwrap_or(s + max_duration);
        let end = next.min(s + max_duration).max(s);
        out.push(Sub {
            start: s,
            end,
            text: text.clone(),
        });
    }
    out
}

pub fn write_tmp(subs: &[Sub]) -> String {
    let mut s = String::new();
    for (k, sub) in subs.iter().enumerate() {
        let t = sub.start.ms() / 1000;
        s.push_str(&format!(
            "{}:{:02}:{:02}:{}\r\n",
            t / 3600,
            (t / 60) % 60,
            t % 60,
            ass_to_slash_lines(&sub.text, "/")
        ));
        // TMP has no end times: emit an empty line when there is a gap
        let gap_end = subs.get(k + 1).map(|n| n.start).unwrap_or(Time(i64::MAX));
        if sub.end < gap_end
            && subs
                .get(k + 1)
                .is_some_and(|n| n.start.ms() / 1000 > sub.end.ms() / 1000)
        {
            let e = sub.end.ms() / 1000;
            s.push_str(&format!(
                "{}:{:02}:{:02}:\r\n",
                e / 3600,
                (e / 60) % 60,
                e % 60
            ));
        }
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn srt() {
        let src = "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i>\r\nworld\r\n\r\n2\r\n00:00:03,000 --> 00:00:04,000 X1:10 X2:20\r\n{\\an8}Top\r\n\r\n";
        let subs = parse_srt(src);
        assert_eq!(subs.len(), 2);
        assert_eq!(subs[0].start, Time(1000));
        assert_eq!(subs[0].end, Time(2500));
        assert_eq!(subs[0].text, r"{\i1}Hello{\i0}\Nworld");
        assert_eq!(subs[1].text, r"{\an8}Top");
        let out = write_srt(&subs);
        assert_eq!(
            out,
            "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i>\r\nworld\r\n\r\n2\r\n00:00:03,000 --> 00:00:04,000\r\n{\\an8}Top\r\n\r\n"
        );
        assert_eq!(parse_srt(&out), subs);
    }

    #[test]
    fn srt_without_indices_and_blank_lines() {
        let subs =
            parse_srt("00:00:01.000 --> 00:00:02.000\nA\n00:00:03,000 --> 00:00:04,000\nB\n");
        assert_eq!(subs.len(), 2);
        assert_eq!(subs[1].text, "B");
        // a blank line inside a cue keeps the text after it (Aegisub); one before the next
        // cue's index or timing ends it
        let subs = parse_srt(
            "1\n00:00:01,000 --> 00:00:02,000\nA\n\n\nmore\n\n2\n00:00:03,000 --> 00:00:04,000\n\nB\n\n12\n\n",
        );
        assert_eq!(subs.len(), 2);
        assert_eq!(subs[0].text, r"A\N\N\Nmore");
        assert_eq!(subs[1].text, r"\NB\N\N12");
    }

    #[test]
    fn mpl2() {
        let subs = parse_mpl2("[10][25]/Kursywa|zwykły\n[30][]bez końca\n");
        assert_eq!(subs[0].start, Time(1000));
        assert_eq!(subs[0].end, Time(2500));
        assert_eq!(subs[0].text, r"{\i1}Kursywa\N{\i0}zwykły");
        assert_eq!(subs[1].end, Time(6000));
        assert_eq!(write_mpl2(&subs[..1]), "[10][25]/Kursywa|zwykły\r\n");
    }

    #[test]
    fn microdvd() {
        let fr = Framerate::from_fps(25.0).unwrap();
        let (subs, fps) = parse_microdvd(
            "{1}{1}25\n{25}{50}{y:i}Hej|ty\n{75}{100}{Y:b}A|B\n",
            &Framerate::default(),
        );
        assert_eq!(fps, Some(25.0));
        assert_eq!(subs.len(), 2);
        assert_eq!(subs[0].text, r"{\i1}Hej\N{\i0}ty");
        assert_eq!(subs[1].text, r"{\b1}A\NB");
        let fs = fr.frame_at_time(subs[0].start.ms(), TimeType::Start);
        assert_eq!(fs, 25);
        let out = write_microdvd(&subs[..1], &fr, true);
        assert_eq!(out, "{1}{1}25\r\n{25}{50}{y:i}Hej|ty\r\n");
    }

    #[test]
    fn tmp() {
        let subs = parse_tmp(
            "0:00:01:Pierwsza|linia\n0:00:03:Druga\n0:00:30:\n",
            Time(5000),
        );
        assert_eq!(subs.len(), 2);
        assert_eq!(subs[0].end, Time(3000));
        assert_eq!(subs[1].end, Time(8000));
        assert_eq!(subs[0].text, r"Pierwsza\Nlinia");
        assert_eq!(
            write_tmp(&subs),
            "0:00:01:Pierwsza|linia\r\n0:00:03:Druga\r\n"
        );
    }
}
