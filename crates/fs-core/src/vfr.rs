//! Frame rate handling: constant (rational) and variable (timecode list) frame
//! rates, timecode file v1/v2 parsing and writing, and the frame↔time
//! conversions used for frame-exact subtitle timing.
//!
//! Semantics follow Aegisub so that scripts timed in either editor land on the
//! same frames:
//! * `Exact`: the frame being displayed at time `t`.
//! * `Start`: the first frame a line starting at `t` is visible on.
//! * `End`: the last frame a line ending at `t` is visible on.

use std::fmt;

const DEFAULT_DENOMINATOR: i64 = 1_000_000_000;
const MAX_TIMECODES: usize = 10_000_000;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TimeType {
    Exact,
    Start,
    End,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VfrError {
    InvalidFramerate(String),
    MalformedLine(String),
    UnknownFormat(String),
}

impl fmt::Display for VfrError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            VfrError::InvalidFramerate(s) => write!(f, "invalid frame rate: {s}"),
            VfrError::MalformedLine(s) => write!(f, "malformed timecode line: {s}"),
            VfrError::UnknownFormat(s) => write!(f, "unknown timecode file format: {s}"),
        }
    }
}

impl std::error::Error for VfrError {}

fn invalid(s: &str) -> VfrError {
    VfrError::InvalidFramerate(s.to_string())
}

#[derive(Debug, Clone, PartialEq)]
pub struct Framerate {
    numerator: i64,
    denominator: i64,
    /// Frame start times in ms; a single `[0]` for CFR.
    timecodes: Vec<i64>,
    /// Unrounded time of the last frame × numerator (for extrapolation).
    last: i64,
    drop: bool,
}

impl Default for Framerate {
    /// "No frame rate" — every conversion returns 0.
    fn default() -> Self {
        Framerate {
            numerator: 0,
            denominator: 1,
            timecodes: vec![0],
            last: 0,
            drop: false,
        }
    }
}

impl Framerate {
    /// Parse a user-typed rate: `24000/1001`, `25`, `23.976` (common NTSC
    /// values map to their exact rationals), with `,` accepted as decimal point.
    pub fn parse(s: &str) -> Result<Self, VfrError> {
        let s = s.trim();
        if let Some((n, d)) = s.split_once('/') {
            let num = |x: &str| {
                x.trim()
                    .parse::<i64>()
                    .map_err(|_| invalid("invalid frame rate"))
            };
            return Self::from_rational(num(n)?, num(d)?, true);
        }
        let s = s.replace(',', ".");
        let v: f64 = s.parse().map_err(|_| invalid("invalid frame rate"))?;
        match s.as_str() {
            "23.976" | "23.98" => Self::from_rational(24000, 1001, true),
            "29.97" => Self::from_rational(30000, 1001, true),
            "59.94" => Self::from_rational(60000, 1001, true),
            "47.952" => Self::from_rational(48000, 1001, true),
            _ if v.fract() == 0.0 => Self::from_rational(v as i64, 1, true),
            _ => Self::from_fps(v),
        }
    }

    /// Constant frame rate from a floating-point FPS value.
    pub fn from_fps(fps: f64) -> Result<Self, VfrError> {
        if !fps.is_finite() || fps < 0.0 {
            return Err(invalid("FPS must be greater than zero"));
        }
        if fps > 1000.0 {
            return Err(invalid("FPS must not be greater than 1000"));
        }
        Ok(Framerate {
            numerator: (fps * DEFAULT_DENOMINATOR as f64) as i64,
            denominator: DEFAULT_DENOMINATOR,
            timecodes: vec![0],
            last: 0,
            drop: false,
        })
    }

    /// Constant frame rate `numerator/denominator`. `drop` enables SMPTE
    /// drop-frame timecodes for non-integral rates.
    pub fn from_rational(numerator: i64, denominator: i64, drop: bool) -> Result<Self, VfrError> {
        if numerator <= 0 || denominator <= 0 {
            return Err(invalid(
                "Numerator and denominator must both be greater than zero",
            ));
        }
        if numerator / denominator > 1000 {
            return Err(invalid("FPS must not be greater than 1000"));
        }
        Ok(Framerate {
            numerator,
            denominator,
            timecodes: vec![0],
            last: 0,
            drop: drop && numerator % denominator != 0,
        })
    }

    /// Variable frame rate from frame start times (ms).
    pub fn from_timecodes(timecodes: Vec<i64>) -> Result<Self, VfrError> {
        let mut f = Framerate {
            numerator: 0,
            denominator: DEFAULT_DENOMINATOR,
            timecodes,
            last: 0,
            drop: false,
        };
        f.set_from_timecodes()?;
        Ok(f)
    }

    fn set_from_timecodes(&mut self) -> Result<(), VfrError> {
        let tc = &mut self.timecodes;
        if tc.len() <= 1 {
            return Err(invalid(
                "Must have at least two timecodes to do anything useful",
            ));
        }
        if !tc.is_sorted() {
            return Err(invalid("Timecodes are out of order"));
        }
        if tc.first() == tc.last() {
            return Err(invalid("Timecodes are all identical"));
        }
        let front = tc[0];
        if front != 0 {
            tc.iter_mut().for_each(|t| *t -= front);
        }
        self.denominator = DEFAULT_DENOMINATOR;
        let n = (tc.len() - 1) as i64;
        self.numerator = n * self.denominator * 1000 / *tc.last().unwrap();
        self.last = n * self.denominator * 1000;
        Ok(())
    }

    /// Parse the contents of a timecode file (v1 or v2).
    pub fn parse_timecodes(text: &str) -> Result<Self, VfrError> {
        let text = text.strip_prefix('\u{feff}').unwrap_or(text);
        let mut lines = text.lines().map(|l| l.trim_end_matches('\r'));
        let first = lines.next().unwrap_or("");
        if first == "# timecode format v2" {
            let mut tcs = Vec::new();
            for l in lines {
                let t = l.trim();
                if t.is_empty() || t.starts_with('#') {
                    continue;
                }
                // Aegisub reads integers; fractional ms are truncated.
                let v: f64 = match t.parse() {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                if tcs.len() >= MAX_TIMECODES {
                    return Err(invalid("Timecode file exceeds the 10000000 entry limit"));
                }
                tcs.push(v as i64);
            }
            return Self::from_timecodes(tcs);
        }
        if first == "# timecode format v1" || first.starts_with("Assume ") {
            let header = if first.starts_with('#') {
                lines.next().unwrap_or("")
            } else {
                first
            };
            return Self::parse_v1(header, lines);
        }
        Err(VfrError::UnknownFormat(first.to_string()))
    }

    fn parse_v1<'a>(header: &str, lines: impl Iterator<Item = &'a str>) -> Result<Self, VfrError> {
        let fps_str = header.get(7..).unwrap_or("").trim();
        let fps: f64 = fps_str
            .parse()
            .ok()
            .filter(|v: &f64| v.is_finite() && *v > 0.0)
            .ok_or_else(|| invalid("Assumed FPS must be greater than zero"))?;
        if fps > 1000.0 {
            return Err(invalid("Assumed FPS must not be greater than 1000"));
        }
        struct Range {
            start: i64,
            end: i64,
            fps: f64,
        }
        let mut ranges = Vec::new();
        for line in lines {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let parts: Vec<&str> = line.split(',').collect();
            if parts.len() != 3 {
                return Err(VfrError::MalformedLine(line.to_string()));
            }
            let parse_int = |s: &str| -> Result<i64, VfrError> {
                s.trim()
                    .parse::<i64>()
                    .map_err(|_| VfrError::MalformedLine(line.to_string()))
            };
            // leading whitespace is allowed (stream extraction), trailing is not
            let start = parse_int(parts[0])?;
            let end = parse_int(parts[1])?;
            if parts[2].ends_with(|c: char| c.is_whitespace()) && !parts[2].trim().is_empty() {
                return Err(VfrError::MalformedLine(line.to_string()));
            }
            let rfps: f64 = parts[2]
                .trim()
                .parse()
                .map_err(|_| VfrError::MalformedLine(line.to_string()))?;
            if start < 0 || end < 0 {
                return Err(invalid("Cannot specify frame rate for negative frames."));
            }
            if end < start {
                return Err(invalid(
                    "End frame must be greater than or equal to start frame",
                ));
            }
            if end > MAX_TIMECODES as i64 - 2 {
                return Err(invalid("V1 timecode range exceeds the frame limit"));
            }
            if !rfps.is_finite() || rfps <= 0.0 {
                return Err(invalid("FPS must be greater than zero"));
            }
            if rfps > 1000.0 {
                return Err(invalid("FPS must be at most 1000"));
            }
            ranges.push(Range {
                start,
                end,
                fps: rfps,
            });
        }
        ranges.sort_by_key(|r| r.start);
        let mut timecodes: Vec<i64> = Vec::new();
        let push = |tcs: &mut Vec<i64>, time: f64| -> Result<(), VfrError> {
            if !time.is_finite() || time < 0.0 || time > i32::MAX as f64 - 0.5 {
                return Err(invalid("V1 timecode exceeds the supported timestamp range"));
            }
            tcs.push((time + 0.5) as i64);
            Ok(())
        };
        let mut time = 0.0f64;
        let mut frame = 0i64;
        for r in &ranges {
            if frame > r.start {
                return Err(invalid("Override ranges must not overlap"));
            }
            while frame < r.start {
                push(&mut timecodes, time)?;
                time += 1000.0 / fps;
                frame += 1;
            }
            while frame <= r.end {
                push(&mut timecodes, time)?;
                time += 1000.0 / r.fps;
                frame += 1;
            }
        }
        push(&mut timecodes, time)?;
        let last_time = time * fps * DEFAULT_DENOMINATOR as f64;
        if !last_time.is_finite() || last_time >= 2f64.powi(63) {
            return Err(invalid("V1 timecode exceeds the supported timestamp range"));
        }
        Ok(Framerate {
            numerator: (fps * DEFAULT_DENOMINATOR as f64) as i64,
            denominator: DEFAULT_DENOMINATOR,
            timecodes,
            last: last_time as i64,
            drop: false,
        })
    }

    /// Serialise as a v2 timecode file, extrapolating up to `length` frames.
    pub fn to_v2_string(&self, length: usize) -> String {
        let mut s = String::from("# timecode format v2\n");
        for t in &self.timecodes {
            s.push_str(&t.to_string());
            s.push('\n');
        }
        for f in self.timecodes.len()..length {
            s.push_str(&self.time_at_frame(f as i64, TimeType::Exact).to_string());
            s.push('\n');
        }
        s
    }

    pub fn is_loaded(&self) -> bool {
        self.numerator != 0
    }

    pub fn is_vfr(&self) -> bool {
        self.timecodes.len() > 1
    }

    pub fn fps(&self) -> f64 {
        if self.denominator == 0 {
            0.0
        } else {
            self.numerator as f64 / self.denominator as f64
        }
    }

    pub fn needs_drop_frames(&self) -> bool {
        self.drop
    }

    pub fn timecodes(&self) -> &[i64] {
        &self.timecodes
    }

    pub fn frame_at_time(&self, ms: i64, ty: TimeType) -> i64 {
        // With X ms per frame, this returns 0 for:
        //   Exact: [0, X-1], Start: [1-X, 0], End: [1, X]
        match ty {
            TimeType::Start => return self.frame_at_time(ms - 1, TimeType::Exact) + 1,
            TimeType::End => return self.frame_at_time(ms - 1, TimeType::Exact),
            TimeType::Exact => {}
        }
        if self.numerator == 0 {
            return 0;
        }
        if ms < 0 {
            return (ms * self.numerator / self.denominator - 999) / 1000;
        }
        let back = *self.timecodes.last().unwrap();
        if ms > back {
            let d = 1000 * self.denominator;
            return ((ms + 1) * self.numerator - self.last - self.numerator / 2 + (d - 1)) / d
                + self.timecodes.len() as i64
                - 2;
        }
        // index of the last timecode <= ms
        self.timecodes.partition_point(|&t| t <= ms) as i64 - 1
    }

    pub fn time_at_frame(&self, frame: i64, ty: TimeType) -> i64 {
        match ty {
            TimeType::Start => {
                let prev = self.time_at_frame(frame - 1, TimeType::Exact);
                let cur = self.time_at_frame(frame, TimeType::Exact);
                // +1 so two frames 1 ms apart round up
                return prev + (cur - prev + 1) / 2;
            }
            TimeType::End => {
                let cur = self.time_at_frame(frame, TimeType::Exact);
                let next = self.time_at_frame(frame + 1, TimeType::Exact);
                return cur + (next - cur + 1) / 2;
            }
            TimeType::Exact => {}
        }
        if self.numerator == 0 {
            return 0;
        }
        if frame < 0 {
            return frame * self.denominator * 1000 / self.numerator;
        }
        let n = self.timecodes.len() as i64;
        if frame >= n {
            let past = frame - n + 1;
            return (past * 1000 * self.denominator + self.last + self.numerator / 2)
                / self.numerator;
        }
        self.timecodes[frame as usize]
    }

    /// SMPTE timecode `(h, m, s, f)` of a frame.
    pub fn smpte_at_frame(&self, frame: i64) -> (i64, i64, i64, i64) {
        let mut frame = frame.max(0);
        let fps = self.fps();
        let ifps = fps.ceil() as i64;
        if ifps == 0 {
            return (0, 0, 0, 0);
        }
        if self.drop && self.denominator == 1001 && self.numerator % 30000 == 0 {
            let drop_factor = self.numerator / 30000;
            let one_minute = 60 * 30 * drop_factor - drop_factor * 2;
            let ten_minutes = 60 * 10 * 30 * drop_factor - drop_factor * 18;
            let groups = frame / ten_minutes;
            let last_ten = frame % ten_minutes;
            frame += groups * 18 * drop_factor;
            frame += (last_ten - 2 * drop_factor) / one_minute * 2 * drop_factor;
        } else if self.drop && ifps as f64 != fps {
            frame = (frame as f64 / fps * ifps as f64 + 0.5) as i64;
        }
        (
            frame / (ifps * 3600),
            (frame / (ifps * 60)) % 60,
            (frame / ifps) % 60,
            frame % ifps,
        )
    }

    pub fn smpte_at_time(&self, ms: i64) -> (i64, i64, i64, i64) {
        self.smpte_at_frame(self.frame_at_time(ms, TimeType::Exact))
    }

    pub fn frame_at_smpte(&self, h: i64, m: i64, s: i64, f: i64) -> i64 {
        let fps = self.fps();
        let ifps = fps.ceil() as i64;
        if self.drop && self.denominator == 1001 && self.numerator % 30000 == 0 {
            let drop_factor = self.numerator / 30000;
            let one_minute = 60 * 30 * drop_factor - drop_factor * 2;
            let ten_minutes = 60 * 10 * 30 * drop_factor - drop_factor * 18;
            let ten_m = m / 10;
            let m = m % 10;
            let f = if m != 0 && s == 0 && f < 2 * drop_factor {
                2 * drop_factor
            } else {
                f
            };
            return h * ten_minutes * 6 + ten_m * ten_minutes + m * one_minute + s * ifps + f;
        } else if self.drop && ifps as f64 != fps {
            let frame = (h * 3600 + m * 60 + s) * ifps + f;
            return (frame as f64 / ifps as f64 * fps + 0.5) as i64;
        }
        (h * 3600 + m * 60 + s) * ifps + f
    }

    pub fn time_at_smpte(&self, h: i64, m: i64, s: i64, f: i64) -> i64 {
        self.time_at_frame(self.frame_at_smpte(h, m, s, f), TimeType::Exact)
    }
}

#[cfg(test)]
mod tests {
    use super::TimeType::*;
    use super::*;

    fn cfr(fps: f64) -> Framerate {
        Framerate::from_fps(fps).unwrap()
    }

    fn vfr(t: &[i64]) -> Framerate {
        Framerate::from_timecodes(t.to_vec()).unwrap()
    }

    fn in_range(lo: i64, hi: i64, v: i64) {
        assert!(lo <= v && v <= hi, "{v} not in [{lo}, {hi}]");
    }

    #[test]
    fn constructors_bad() {
        assert!(Framerate::from_fps(-1.0).is_err());
        assert!(Framerate::from_fps(1000.1).is_err());
        assert!(Framerate::from_fps(f64::NAN).is_err());
        assert!(Framerate::from_fps(f64::INFINITY).is_err());
        assert!(Framerate::from_fps(f64::MAX).is_err());
        assert!(Framerate::from_timecodes(vec![]).is_err());
        assert!(Framerate::from_timecodes(vec![0]).is_err());
        assert!(Framerate::from_timecodes(vec![10, 0]).is_err());
        assert!(Framerate::from_timecodes(vec![0, 0]).is_err());
    }

    #[test]
    fn cfr_frame_at_time() {
        let f = cfr(1.0);
        for (ms, e) in [
            (0, 0),
            (999, 0),
            (1000, 1),
            (1999, 1),
            (100000, 100),
            (-1, -1),
            (-1000, -1),
            (-1001, -2),
        ] {
            assert_eq!(f.frame_at_time(ms, Exact), e, "exact {ms}");
        }
        for (ms, e) in [
            (0, 0),
            (1, 1),
            (1000, 1),
            (1001, 2),
            (100000, 100),
            (-1, 0),
            (-999, 0),
            (-1000, -1),
            (-1999, -1),
        ] {
            assert_eq!(f.frame_at_time(ms, Start), e, "start {ms}");
        }
        for (ms, e) in [
            (0, -1),
            (1, 0),
            (1000, 0),
            (1001, 1),
            (100000, 99),
            (-1, -1),
            (-999, -1),
            (-1000, -2),
            (-1999, -2),
        ] {
            assert_eq!(f.frame_at_time(ms, End), e, "end {ms}");
        }
    }

    #[test]
    fn cfr_time_at_frame() {
        let f = cfr(1.0);
        assert_eq!(f.time_at_frame(0, Exact), 0);
        assert_eq!(f.time_at_frame(1, Exact), 1000);
        assert_eq!(f.time_at_frame(2, Exact), 2000);
        assert_eq!(f.time_at_frame(-1, Exact), -1000);
        in_range(-999, 0, f.time_at_frame(0, Start));
        in_range(1, 1000, f.time_at_frame(1, Start));
        in_range(1001, 2000, f.time_at_frame(2, Start));
        in_range(-1999, -1000, f.time_at_frame(-1, Start));
        in_range(1, 1000, f.time_at_frame(0, End));
        in_range(1001, 2000, f.time_at_frame(1, End));
        in_range(2001, 3000, f.time_at_frame(2, End));
        in_range(-999, 0, f.time_at_frame(-1, End));
    }

    #[test]
    fn round_trips() {
        let fps1 = cfr(1.0);
        let ntsc = Framerate::from_rational(24000, 1001, true).unwrap();
        let v = vfr(&[0, 1000, 1500, 2000, 2001, 2002, 2003]);
        for ty in [Exact, Start, End] {
            for i in -10..11 {
                assert_eq!(fps1.frame_at_time(fps1.time_at_frame(i, ty), ty), i);
                assert_eq!(v.frame_at_time(v.time_at_frame(i, ty), ty), i, "{ty:?} {i}");
            }
            for i in [-100, -10, -1, 0, 1, 10, 100, 6820] {
                assert_eq!(ntsc.frame_at_time(ntsc.time_at_frame(i, ty), ty), i);
            }
        }
    }

    #[test]
    fn vfr_conversions() {
        let f = vfr(&[0, 1000, 1500, 2000, 2001]);
        assert_eq!(
            [0, 1, 2, 3, 4].map(|i| f.time_at_frame(i, Exact)),
            [0, 1000, 1500, 2000, 2001]
        );
        let f = vfr(&[0, 1000, 1500, 2000, 2001, 2002, 2003]);
        assert!(f.time_at_frame(0, Start) <= 0);
        in_range(1, 1000, f.time_at_frame(1, Start));
        in_range(1001, 1500, f.time_at_frame(2, Start));
        in_range(1501, 2000, f.time_at_frame(3, Start));
        assert_eq!(f.time_at_frame(4, Start), 2001);
        assert_eq!(f.time_at_frame(5, Start), 2002);
        assert!(f.time_at_frame(6, Start) >= 2003);
        in_range(1, 1000, f.time_at_frame(0, End));
        in_range(1001, 1500, f.time_at_frame(1, End));
        in_range(1501, 2000, f.time_at_frame(2, End));
        assert_eq!(f.time_at_frame(3, End), 2001);
        assert_eq!(f.time_at_frame(4, End), 2002);
        assert_eq!(f.time_at_frame(5, End), 2003);
        assert!(f.time_at_frame(6, End) >= 2004);
        for (ms, e) in [
            (0, 0),
            (999, 0),
            (1000, 1),
            (1499, 1),
            (1500, 2),
            (1999, 2),
            (2000, 3),
            (2001, 4),
            (2002, 5),
            (2003, 6),
        ] {
            assert_eq!(f.frame_at_time(ms, Exact), e);
        }
        assert!(f.frame_at_time(-1, Exact) < 0);
        assert!(f.frame_at_time(2004, Exact) >= 6);
        for (ms, e) in [
            (0, 0),
            (1, 1),
            (1000, 1),
            (1001, 2),
            (1500, 2),
            (1501, 3),
            (2000, 3),
            (2001, 4),
            (2002, 5),
            (2003, 6),
        ] {
            assert_eq!(f.frame_at_time(ms, Start), e);
        }
    }

    #[test]
    fn outside_range() {
        let f = vfr(&[0, 100, 200]);
        assert!(f.time_at_frame(-1, Exact) < 0);
        assert_eq!(f.time_at_frame(2, Exact), 200);
        let mut prev = f.time_at_frame(3, Exact);
        assert!(prev > 200);
        for i in 4..10 {
            let t = f.time_at_frame(i, Exact);
            assert!(t > prev);
            prev = t;
        }
    }

    #[test]
    fn nonzero_start() {
        let f = vfr(&[10, 20, 30, 40, 50]);
        assert_eq!(
            [0, 1, 2, 3, 4].map(|i| f.time_at_frame(i, Exact)),
            [0, 10, 20, 30, 40]
        );
        let f = vfr(&[-10, 20, 30, 40, 50]);
        assert_eq!(
            [0, 1, 2, 3, 4].map(|i| f.time_at_frame(i, Exact)),
            [0, 30, 40, 50, 60]
        );
    }

    #[test]
    fn rational_timebase() {
        let f = Framerate::from_rational(30000, 1001, true).unwrap();
        for i in 0..100_000 {
            assert_eq!(f.time_at_frame(i * 30, Exact), i * 1001);
            assert_eq!(f.frame_at_time(i * 1001, Exact), i * 30);
        }
        let f = Framerate::from_rational(24000, 1001, true).unwrap();
        for i in 0..100_000 {
            assert_eq!(f.time_at_frame(i * 24, Exact), i * 1001);
            assert_eq!(f.frame_at_time(i * 1001, Exact), i * 24);
        }
    }

    #[test]
    fn duplicate_timestamps() {
        let f = vfr(&[0, 0, 1, 2, 2, 3]);
        assert_eq!(
            [0, 1, 2, 3].map(|t| f.frame_at_time(t, Exact)),
            [1, 2, 4, 5]
        );
        assert_eq!(
            [0, 1, 2, 3, 4, 5].map(|i| f.time_at_frame(i, Exact)),
            [0, 0, 1, 2, 2, 3]
        );
        let f = vfr(&[0, 100, 100, 200, 300]);
        assert_eq!(
            [0, 99, 100, 199, 200].map(|t| f.frame_at_time(t, Exact)),
            [0, 0, 2, 2, 3]
        );
    }

    #[test]
    fn smpte() {
        let f = Framerate::from_rational(30000, 1001, true).unwrap();
        assert!(f.needs_drop_frames());
        for (fr, e) in [
            (0, (0, 0, 0, 0)),
            (29, (0, 0, 0, 29)),
            (30, (0, 0, 1, 0)),
            (1800, (0, 1, 0, 2)),
            (3597, (0, 1, 59, 29)),
            (3598, (0, 2, 0, 2)),
            (107892, (1, 0, 0, 0)),
        ] {
            assert_eq!(f.smpte_at_frame(fr), e);
        }
        for i in 0..100_000 {
            let (h, m, s, fr) = f.smpte_at_frame(i);
            assert_eq!(f.frame_at_smpte(h, m, s, fr), i);
        }
        assert_eq!(f.frame_at_smpte(0, 1, 0, 0), 1800);
        let f = Framerate::from_rational(60000, 1001, true).unwrap();
        assert_eq!(f.smpte_at_frame(3600), (0, 1, 0, 4));
        assert_eq!(f.frame_at_smpte(0, 3, 0, 5), 10793);
        let pal = Framerate::from_rational(25, 1, true).unwrap();
        assert!(!pal.needs_drop_frames());
        assert_eq!(pal.smpte_at_frame(1500), (0, 1, 0, 0));
        for i in 0..36000 {
            let (h, m, s, _) = pal.smpte_at_time(i * 1000);
            assert_eq!(h * 3600 + m * 60 + s, i);
        }
        let dec = Framerate::from_rational(24000, 1001, true).unwrap();
        for i in 0..100_000 {
            let (h, m, s, fr) = dec.smpte_at_frame(i);
            assert_eq!(dec.frame_at_smpte(h, m, s, fr), i);
        }
        let nodrop = Framerate::from_rational(30000, 1001, false).unwrap();
        for i in 0..100_000 {
            let (h, m, s, fr) = nodrop.smpte_at_frame(i);
            assert_eq!(h * 108000 + m * 1800 + s * 30 + fr, i);
        }
    }

    #[test]
    fn no_intermediate_overflow() {
        let f = cfr(1.0);
        let last = i32::MAX as i64 / 1000;
        assert_eq!(f.time_at_frame(last, Exact), last * 1000);
        assert_eq!(f.frame_at_time(last * 1000, Exact), last);
    }
}
