//! Time values used throughout the editor.
//!
//! Internally every timestamp is an integer number of milliseconds. ASS stores
//! centiseconds, SRT milliseconds, MicroDVD frames — conversion happens at the
//! format boundary.

use std::fmt;
use std::ops::{Add, AddAssign, Neg, Sub, SubAssign};

/// Maximum time representable in ASS (`9:59:59.99`). Aegisub clamps to this;
/// we allow larger values internally but formatters keep the hour field growing.
pub const ASS_MAX_MS: i64 = 10 * 3_600_000 - 10;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct Time(pub i64);

impl Time {
    pub const ZERO: Time = Time(0);

    #[inline]
    pub const fn from_ms(ms: i64) -> Self {
        Time(ms)
    }

    #[inline]
    pub const fn ms(self) -> i64 {
        self.0
    }

    pub const fn from_hmsms(h: i64, m: i64, s: i64, ms: i64) -> Self {
        Time(((h * 60 + m) * 60 + s) * 1000 + ms)
    }

    /// Round to the nearest centisecond (half away from zero), as ASS can only
    /// store centiseconds.
    pub fn round_cs(self) -> Self {
        let ms = self.0;
        let r = if ms >= 0 {
            (ms + 5).div_euclid(10) * 10
        } else {
            -((-ms + 5).div_euclid(10) * 10)
        };
        Time(r)
    }

    pub fn clamp_ass(self) -> Self {
        Time(self.0.clamp(0, ASS_MAX_MS))
    }

    /// Format as ASS timestamp `H:MM:SS.cc`. Negative times are clamped to 0,
    /// matching Aegisub/VSFilter.
    pub fn to_ass(self) -> String {
        let mut s = String::with_capacity(10);
        self.write_ass(&mut s);
        s
    }

    pub fn write_ass(self, out: &mut String) {
        use std::fmt::Write;
        let t = self.round_cs().0.max(0);
        let cs = (t / 10) % 100;
        let sec = (t / 1000) % 60;
        let min = (t / 60_000) % 60;
        let h = t / 3_600_000;
        let _ = write!(out, "{h}:{min:02}:{sec:02}.{cs:02}");
    }

    /// Format as SRT timestamp `HH:MM:SS,mmm`.
    pub fn to_srt(self) -> String {
        let t = self.0.max(0);
        format!(
            "{:02}:{:02}:{:02},{:03}",
            t / 3_600_000,
            (t / 60_000) % 60,
            (t / 1000) % 60,
            t % 1000
        )
    }

    /// Format as `H:MM:SS.mmm` (used in the CLI and GUI time fields when
    /// millisecond precision is requested).
    pub fn to_hms_ms(self) -> String {
        let neg = self.0 < 0;
        let t = self.0.abs();
        format!(
            "{}{}:{:02}:{:02}.{:03}",
            if neg { "-" } else { "" },
            t / 3_600_000,
            (t / 60_000) % 60,
            (t / 1000) % 60,
            t % 1000
        )
    }

    /// Parse an ASS-style timestamp. Accepts the forms produced by real-world
    /// tools: `H:MM:SS.cc`, `HH:MM:SS.ccc`, `M:SS.cc`, `SS.cc`, missing
    /// fraction, extra whitespace, and a leading minus. The fractional part is
    /// interpreted as a decimal fraction of a second (so `.5` = 500 ms,
    /// `.05` = 50 ms, `.123` = 123 ms).
    pub fn parse_ass(s: &str) -> Option<Self> {
        parse_clock(s, b'.')
    }

    /// An event's Start or End as Aegisub's `AssTime` reads it, which for what files hold is
    /// what libass shows: anything but digits, `:`, `.` and `,` is skipped, so "-0:00:01.00" is
    /// one second (libass reads the sign of the hours alone) and a stray letter costs nothing.
    /// Never negative; an absurd hour count is 0.
    pub fn from_ass_field(s: &str) -> Time {
        let read = || -> Option<i64> {
            let (mut ms, mut cur, mut frac) = (0i64, 0i64, -1i64);
            for c in s.bytes() {
                match c {
                    b':' if frac < 0 => (ms, cur) = (ms.checked_mul(60)?.checked_add(cur)?, 0),
                    b'.' | b',' if frac < 0 => {
                        ms = ms.checked_mul(60)?.checked_add(cur)?.checked_mul(1000)?;
                        (cur, frac) = (0, 100);
                    }
                    b'0'..=b'9' if frac < 0 => {
                        cur = cur.checked_mul(10)?.checked_add(i64::from(c - b'0'))?
                    }
                    b'0'..=b'9' => (ms, frac) = (ms + i64::from(c - b'0') * frac, frac / 10),
                    _ => {}
                }
            }
            if frac < 0 {
                ms = ms.checked_mul(60)?.checked_add(cur)?.checked_mul(1000)?;
            }
            Some(ms)
        };
        Time(read().unwrap_or(0))
    }

    /// Parse an SRT timestamp (`HH:MM:SS,mmm`; also tolerates `.`).
    pub fn parse_srt(s: &str) -> Option<Self> {
        parse_clock(s, b',')
    }

    /// Parse a user-entered duration/offset such as `1:02.5`, `-0:00:01.20`,
    /// `1500ms`, `2.5s`, `+300`.
    pub fn parse_offset(s: &str) -> Option<Self> {
        let s = s.trim();
        let (neg, rest) = match s.as_bytes().first() {
            Some(b'-') => (true, &s[1..]),
            Some(b'+') => (false, &s[1..]),
            _ => (false, s),
        };
        let rest = rest.trim();
        let v = if let Some(n) = rest.strip_suffix("ms") {
            Time(n.trim().parse::<i64>().ok()?)
        } else if let Some(n) = rest.strip_suffix('s') {
            let f: f64 = n.trim().parse().ok()?;
            Time((f * 1000.0).round() as i64)
        } else if rest.contains(':') {
            parse_clock(rest, b'.')?
        } else if rest.contains('.') {
            let f: f64 = rest.parse().ok()?;
            Time((f * 1000.0).round() as i64)
        } else {
            Time(rest.parse::<i64>().ok()?)
        };
        Some(if neg { -v } else { v })
    }
}

fn parse_clock(s: &str, frac_sep: u8) -> Option<Time> {
    let s = s.trim();
    if s.is_empty() {
        return None;
    }
    let (neg, s) = match s.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, s),
    };
    let bytes = s.as_bytes();
    // Split into colon-separated integer groups and an optional fraction.
    let mut groups: [i64; 3] = [0; 3];
    let mut ngroups = 0usize;
    let mut cur: i64 = 0;
    let mut have_digit = false;
    let mut frac_ms: i64 = 0;
    let mut i = 0;
    while i < bytes.len() {
        let c = bytes[i];
        match c {
            b'0'..=b'9' => {
                cur = cur.checked_mul(10)?.checked_add((c - b'0') as i64)?;
                have_digit = true;
            }
            b':' => {
                if ngroups >= 2 {
                    return None;
                }
                groups[ngroups] = cur;
                ngroups += 1;
                cur = 0;
                have_digit = false;
            }
            b' ' | b'\t' => {}
            c2 if c2 == frac_sep || c2 == b'.' || c2 == b',' => {
                // fraction: up to three significant digits, rest ignored
                let mut scale = 100;
                let mut j = i + 1;
                while j < bytes.len() && bytes[j].is_ascii_digit() {
                    if scale > 0 {
                        frac_ms += (bytes[j] - b'0') as i64 * scale;
                        scale /= 10;
                    }
                    j += 1;
                }
                // trailing garbage after fraction is tolerated only if whitespace
                if bytes[j..].iter().any(|b| !b.is_ascii_whitespace()) {
                    return None;
                }
                i = bytes.len();
                continue;
            }
            _ => return None,
        }
        i += 1;
    }
    if !have_digit && ngroups == 0 && frac_ms == 0 {
        return None;
    }
    groups[ngroups] = cur;
    let (h, m, sec) = match ngroups {
        0 => (0, 0, groups[0]),
        1 => (0, groups[0], groups[1]),
        _ => (groups[0], groups[1], groups[2]),
    };
    // checked: a corrupt file's absurd hour count is not a time
    let ms = h
        .checked_mul(60)?
        .checked_add(m)?
        .checked_mul(60)?
        .checked_add(sec)?
        .checked_mul(1000)?
        .checked_add(frac_ms)?;
    Some(Time(if neg { -ms } else { ms }))
}

impl fmt::Display for Time {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_ass())
    }
}

impl Add for Time {
    type Output = Time;
    fn add(self, rhs: Time) -> Time {
        Time(self.0 + rhs.0)
    }
}
impl Sub for Time {
    type Output = Time;
    fn sub(self, rhs: Time) -> Time {
        Time(self.0 - rhs.0)
    }
}
impl AddAssign for Time {
    fn add_assign(&mut self, rhs: Time) {
        self.0 += rhs.0;
    }
}
impl SubAssign for Time {
    fn sub_assign(&mut self, rhs: Time) {
        self.0 -= rhs.0;
    }
}
impl Neg for Time {
    type Output = Time;
    fn neg(self) -> Time {
        Time(-self.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn event_fields_read_as_aegisub() {
        assert_eq!(Time::from_ass_field("0:00:01.00"), Time(1000));
        assert_eq!(Time::from_ass_field("-0:00:01.50"), Time(1500));
        assert_eq!(Time::from_ass_field(" 1:02:03.456x"), Time(3_723_456));
        assert_eq!(Time::from_ass_field("0:00:05"), Time(5000));
        assert_eq!(Time::from_ass_field("12:00:00.00"), Time(43_200_000));
        assert_eq!(Time::from_ass_field("garbage"), Time(0));
        assert_eq!(
            Time::from_ass_field("99999999999999999999:00:00.00"),
            Time(0)
        );
    }

    #[test]
    fn parse_basic() {
        assert_eq!(Time::parse_ass("0:00:01.50"), Some(Time(1500)));
        assert_eq!(
            Time::parse_ass("1:02:03.04"),
            Some(Time::from_hmsms(1, 2, 3, 40))
        );
        assert_eq!(Time::parse_ass("10:00:00.00"), Some(Time(36_000_000)));
        assert_eq!(Time::parse_ass("0:00:01.5"), Some(Time(1500)));
        assert_eq!(Time::parse_ass("0:00:01.123"), Some(Time(1123)));
        assert_eq!(Time::parse_ass("0:00:01"), Some(Time(1000)));
        assert_eq!(Time::parse_ass(" 0:00:01.00 "), Some(Time(1000)));
        assert_eq!(Time::parse_ass("-0:00:01.00"), Some(Time(-1000)));
        assert_eq!(Time::parse_ass("1:30.25"), Some(Time(90_250)));
        assert_eq!(Time::parse_ass("abc"), None);
        assert_eq!(Time::parse_ass(""), None);
        assert_eq!(Time::parse_ass("9999999999999999:00:00.00"), None);
        assert_eq!(
            Time::parse_srt("01:02:03,456"),
            Some(Time::from_hmsms(1, 2, 3, 456))
        );
    }

    #[test]
    fn format() {
        assert_eq!(Time(0).to_ass(), "0:00:00.00");
        assert_eq!(Time(1234).to_ass(), "0:00:01.23");
        assert_eq!(Time(1235).to_ass(), "0:00:01.24");
        assert_eq!(Time(-5).to_ass(), "0:00:00.00");
        assert_eq!(Time(36_000_000).to_ass(), "10:00:00.00");
        assert_eq!(Time::from_hmsms(1, 2, 3, 456).to_srt(), "01:02:03,456");
    }

    #[test]
    fn rounding() {
        assert_eq!(Time(14).round_cs(), Time(10));
        assert_eq!(Time(15).round_cs(), Time(20));
        assert_eq!(Time(-15).round_cs(), Time(-20));
    }

    #[test]
    fn offsets() {
        assert_eq!(Time::parse_offset("1500ms"), Some(Time(1500)));
        assert_eq!(Time::parse_offset("-2.5s"), Some(Time(-2500)));
        assert_eq!(Time::parse_offset("+0:01.00"), Some(Time(1000)));
        assert_eq!(Time::parse_offset("-1.2"), Some(Time(-1200)));
        assert_eq!(Time::parse_offset("300"), Some(Time(300)));
    }
}
