//! Typed values of ASS styles and events plus the `Format:` field machinery.

use crate::color::Color;
use crate::time::Time;

/// Script version, determined from the styles section header / `ScriptType`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum AssVersion {
    /// SSA `v4.00` (`[V4 Styles]`).
    Ssa,
    /// ASS `v4.00+` (`[V4+ Styles]`).
    #[default]
    Ass,
    /// ASS `v4.00++` (`[V4++ Styles]`, MarginT/MarginB).
    AssPlusPlus,
}

impl AssVersion {
    pub fn styles_header(self) -> &'static str {
        match self {
            AssVersion::Ssa => "[V4 Styles]",
            AssVersion::Ass => "[V4+ Styles]",
            AssVersion::AssPlusPlus => "[V4++ Styles]",
        }
    }

    pub fn script_type(self) -> &'static str {
        match self {
            AssVersion::Ssa => "v4.00",
            AssVersion::Ass => "v4.00+",
            AssVersion::AssPlusPlus => "v4.00++",
        }
    }
}

// ---------------------------------------------------------------------------
// Field definitions
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum StyleField {
    Name,
    Fontname,
    Fontsize,
    PrimaryColour,
    SecondaryColour,
    /// `OutlineColour` in ASS, `TertiaryColour` in SSA.
    OutlineColour,
    BackColour,
    Bold,
    Italic,
    Underline,
    StrikeOut,
    ScaleX,
    ScaleY,
    Spacing,
    Angle,
    BorderStyle,
    Outline,
    Shadow,
    Alignment,
    MarginL,
    MarginR,
    MarginV,
    MarginT,
    MarginB,
    AlphaLevel,
    Encoding,
    RelativeTo,
    Unknown(String),
}

impl StyleField {
    pub fn from_name(s: &str) -> StyleField {
        match s.trim().to_ascii_lowercase().as_str() {
            "name" => StyleField::Name,
            "fontname" => StyleField::Fontname,
            "fontsize" => StyleField::Fontsize,
            "primarycolour" | "primarycolor" => StyleField::PrimaryColour,
            "secondarycolour" | "secondarycolor" => StyleField::SecondaryColour,
            "outlinecolour" | "outlinecolor" | "tertiarycolour" | "tertiarycolor" => {
                StyleField::OutlineColour
            }
            "backcolour" | "backcolor" => StyleField::BackColour,
            "bold" => StyleField::Bold,
            "italic" => StyleField::Italic,
            "underline" => StyleField::Underline,
            "strikeout" => StyleField::StrikeOut,
            "scalex" => StyleField::ScaleX,
            "scaley" => StyleField::ScaleY,
            "spacing" => StyleField::Spacing,
            "angle" => StyleField::Angle,
            "borderstyle" => StyleField::BorderStyle,
            "outline" => StyleField::Outline,
            "shadow" => StyleField::Shadow,
            "alignment" => StyleField::Alignment,
            "marginl" => StyleField::MarginL,
            "marginr" => StyleField::MarginR,
            "marginv" => StyleField::MarginV,
            "margint" => StyleField::MarginT,
            "marginb" => StyleField::MarginB,
            "alphalevel" => StyleField::AlphaLevel,
            "encoding" => StyleField::Encoding,
            "relativeto" => StyleField::RelativeTo,
            _ => StyleField::Unknown(s.trim().to_string()),
        }
    }

    pub fn name(&self, version: AssVersion) -> &str {
        match self {
            StyleField::Name => "Name",
            StyleField::Fontname => "Fontname",
            StyleField::Fontsize => "Fontsize",
            StyleField::PrimaryColour => "PrimaryColour",
            StyleField::SecondaryColour => "SecondaryColour",
            StyleField::OutlineColour => {
                if version == AssVersion::Ssa {
                    "TertiaryColour"
                } else {
                    "OutlineColour"
                }
            }
            StyleField::BackColour => "BackColour",
            StyleField::Bold => "Bold",
            StyleField::Italic => "Italic",
            StyleField::Underline => "Underline",
            StyleField::StrikeOut => "StrikeOut",
            StyleField::ScaleX => "ScaleX",
            StyleField::ScaleY => "ScaleY",
            StyleField::Spacing => "Spacing",
            StyleField::Angle => "Angle",
            StyleField::BorderStyle => "BorderStyle",
            StyleField::Outline => "Outline",
            StyleField::Shadow => "Shadow",
            StyleField::Alignment => "Alignment",
            StyleField::MarginL => "MarginL",
            StyleField::MarginR => "MarginR",
            StyleField::MarginV => "MarginV",
            StyleField::MarginT => "MarginT",
            StyleField::MarginB => "MarginB",
            StyleField::AlphaLevel => "AlphaLevel",
            StyleField::Encoding => "Encoding",
            StyleField::RelativeTo => "RelativeTo",
            StyleField::Unknown(s) => s,
        }
    }

    pub fn default_format(version: AssVersion) -> Vec<StyleField> {
        use StyleField::*;
        match version {
            AssVersion::Ssa => vec![
                Name,
                Fontname,
                Fontsize,
                PrimaryColour,
                SecondaryColour,
                OutlineColour,
                BackColour,
                Bold,
                Italic,
                BorderStyle,
                Outline,
                Shadow,
                Alignment,
                MarginL,
                MarginR,
                MarginV,
                AlphaLevel,
                Encoding,
            ],
            AssVersion::Ass => vec![
                Name,
                Fontname,
                Fontsize,
                PrimaryColour,
                SecondaryColour,
                OutlineColour,
                BackColour,
                Bold,
                Italic,
                Underline,
                StrikeOut,
                ScaleX,
                ScaleY,
                Spacing,
                Angle,
                BorderStyle,
                Outline,
                Shadow,
                Alignment,
                MarginL,
                MarginR,
                MarginV,
                Encoding,
            ],
            AssVersion::AssPlusPlus => vec![
                Name,
                Fontname,
                Fontsize,
                PrimaryColour,
                SecondaryColour,
                OutlineColour,
                BackColour,
                Bold,
                Italic,
                Underline,
                StrikeOut,
                ScaleX,
                ScaleY,
                Spacing,
                Angle,
                BorderStyle,
                Outline,
                Shadow,
                Alignment,
                MarginL,
                MarginR,
                MarginT,
                MarginB,
                Encoding,
                RelativeTo,
            ],
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum EventField {
    Layer,
    /// SSA `Marked` field (value written as `Marked=0`).
    Marked,
    Start,
    End,
    Style,
    /// `Name` (a.k.a. Actor).
    Name,
    MarginL,
    MarginR,
    MarginV,
    MarginT,
    MarginB,
    Effect,
    Text,
    Unknown(String),
}

impl EventField {
    pub fn from_name(s: &str) -> EventField {
        match s.trim().to_ascii_lowercase().as_str() {
            "layer" => EventField::Layer,
            "marked" => EventField::Marked,
            "start" => EventField::Start,
            "end" => EventField::End,
            "style" => EventField::Style,
            "name" | "actor" => EventField::Name,
            "marginl" => EventField::MarginL,
            "marginr" => EventField::MarginR,
            "marginv" => EventField::MarginV,
            "margint" => EventField::MarginT,
            "marginb" => EventField::MarginB,
            "effect" => EventField::Effect,
            "text" => EventField::Text,
            _ => EventField::Unknown(s.trim().to_string()),
        }
    }

    pub fn name(&self) -> &str {
        match self {
            EventField::Layer => "Layer",
            EventField::Marked => "Marked",
            EventField::Start => "Start",
            EventField::End => "End",
            EventField::Style => "Style",
            EventField::Name => "Name",
            EventField::MarginL => "MarginL",
            EventField::MarginR => "MarginR",
            EventField::MarginV => "MarginV",
            EventField::MarginT => "MarginT",
            EventField::MarginB => "MarginB",
            EventField::Effect => "Effect",
            EventField::Text => "Text",
            EventField::Unknown(s) => s,
        }
    }

    pub fn default_format(version: AssVersion) -> Vec<EventField> {
        use EventField::*;
        match version {
            AssVersion::Ssa => vec![
                Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text,
            ],
            AssVersion::Ass => vec![
                Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text,
            ],
            AssVersion::AssPlusPlus => {
                vec![
                    Layer, Start, End, Style, Name, MarginL, MarginR, MarginT, MarginB, Effect,
                    Text,
                ]
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
pub struct Style {
    pub name: String,
    pub font: String,
    pub size: f64,
    pub primary: Color,
    pub secondary: Color,
    pub outline_color: Color,
    pub back: Color,
    /// 0 = normal, -1/1 = bold, >1 = explicit weight (libass semantics).
    pub bold: i32,
    pub italic: bool,
    pub underline: bool,
    pub strikeout: bool,
    pub scale_x: f64,
    pub scale_y: f64,
    pub spacing: f64,
    pub angle: f64,
    pub border_style: i32,
    pub outline: f64,
    pub shadow: f64,
    /// Numpad alignment 1–9 (converted from legacy SSA values when needed).
    pub alignment: u8,
    pub margin_l: i32,
    pub margin_r: i32,
    /// MarginV; for v4++ this is MarginT.
    pub margin_v: i32,
    /// v4++ MarginB (defaults to `margin_v`).
    pub margin_b: i32,
    pub alpha_level: i32,
    pub encoding: i32,
    pub relative_to: i32,
    /// Values of `Unknown` format fields, by field name.
    pub extra: Vec<(String, String)>,
}

impl Default for Style {
    fn default() -> Self {
        Style {
            name: "Default".into(),
            font: "Arial".into(),
            size: 48.0,
            primary: Color::WHITE,
            secondary: Color::RED,
            outline_color: Color::BLACK,
            back: Color::BLACK,
            bold: 0,
            italic: false,
            underline: false,
            strikeout: false,
            scale_x: 100.0,
            scale_y: 100.0,
            spacing: 0.0,
            angle: 0.0,
            border_style: 1,
            outline: 2.0,
            shadow: 2.0,
            alignment: 2,
            margin_l: 10,
            margin_r: 10,
            margin_v: 10,
            margin_b: 10,
            alpha_level: 0,
            encoding: 1,
            relative_to: 0,
            extra: Vec::new(),
        }
    }
}

/// Convert legacy SSA alignment (1–3 sub, +4 top, +8 mid) to numpad.
pub fn ssa_to_numpad(a: i32) -> u8 {
    let h = match a & 3 {
        0 => 2, // invalid horizontal → centre
        x => x,
    };
    let v = if a & 4 != 0 {
        6 // top
    } else if a & 8 != 0 {
        3 // middle
    } else {
        0
    };
    (h + v) as u8
}

/// Convert numpad alignment to legacy SSA value.
pub fn numpad_to_ssa(a: u8) -> i32 {
    let a = a.clamp(1, 9) as i32;
    let h = (a - 1) % 3 + 1;
    match (a - 1) / 3 {
        0 => h,
        1 => h + 8,
        _ => h + 4,
    }
}

fn parse_f64(s: &str) -> f64 {
    let s = s.trim();
    s.parse::<f64>()
        .ok()
        .filter(|v| v.is_finite())
        .unwrap_or_else(|| {
            // tolerate trailing junk like "20px"
            let end = s
                .char_indices()
                .find(|&(i, c)| {
                    !(c.is_ascii_digit() || c == '.' || ((c == '-' || c == '+') && i == 0))
                })
                .map(|(i, _)| i)
                .unwrap_or(s.len());
            s[..end].parse::<f64>().unwrap_or(0.0)
        })
}

fn parse_i32(s: &str) -> i32 {
    let s = s.trim();
    s.parse::<i32>().unwrap_or_else(|_| parse_f64(s) as i32)
}

fn trim_sp(s: &str) -> &str {
    s.trim_matches([' ', '\t'])
}

fn parse_flag(s: &str) -> bool {
    parse_i32(s) != 0
}

/// Format a float the way Aegisub does: no trailing zeros, no exponent.
pub fn fmt_f64(v: f64) -> String {
    if v.fract() == 0.0 && v.abs() < 1e15 {
        return format!("{}", v as i64);
    }
    let mut s = format!("{v:.4}");
    while s.ends_with('0') {
        s.pop();
    }
    if s.ends_with('.') {
        s.pop();
    }
    if s == "-0" { "0".into() } else { s }
}

impl Style {
    /// Set a single field from its textual value.
    pub fn set_field(&mut self, f: &StyleField, v: &str, version: AssVersion) {
        match f {
            StyleField::Name => self.name = v.trim().to_string(),
            StyleField::Fontname => self.font = v.trim().to_string(),
            StyleField::Fontsize => self.size = parse_f64(v),
            StyleField::PrimaryColour => self.primary = Color::parse_style(v).unwrap_or_default(),
            StyleField::SecondaryColour => {
                self.secondary = Color::parse_style(v).unwrap_or_default()
            }
            StyleField::OutlineColour => {
                self.outline_color = Color::parse_style(v).unwrap_or_default()
            }
            StyleField::BackColour => self.back = Color::parse_style(v).unwrap_or_default(),
            StyleField::Bold => self.bold = parse_i32(v),
            StyleField::Italic => self.italic = parse_flag(v),
            StyleField::Underline => self.underline = parse_flag(v),
            StyleField::StrikeOut => self.strikeout = parse_flag(v),
            StyleField::ScaleX => self.scale_x = parse_f64(v),
            StyleField::ScaleY => self.scale_y = parse_f64(v),
            StyleField::Spacing => self.spacing = parse_f64(v),
            StyleField::Angle => self.angle = parse_f64(v),
            StyleField::BorderStyle => self.border_style = parse_i32(v),
            StyleField::Outline => self.outline = parse_f64(v),
            StyleField::Shadow => self.shadow = parse_f64(v),
            StyleField::Alignment => {
                let a = parse_i32(v);
                self.alignment = if version == AssVersion::Ssa {
                    ssa_to_numpad(a)
                } else if (1..=9).contains(&a) {
                    a as u8
                } else {
                    2
                };
            }
            StyleField::MarginL => self.margin_l = parse_i32(v),
            StyleField::MarginR => self.margin_r = parse_i32(v),
            StyleField::MarginV | StyleField::MarginT => self.margin_v = parse_i32(v),
            StyleField::MarginB => self.margin_b = parse_i32(v),
            StyleField::AlphaLevel => self.alpha_level = parse_i32(v),
            StyleField::Encoding => self.encoding = parse_i32(v),
            StyleField::RelativeTo => self.relative_to = parse_i32(v),
            StyleField::Unknown(name) => {
                if let Some(e) = self.extra.iter_mut().find(|(n, _)| n == name) {
                    e.1 = v.to_string();
                } else {
                    self.extra.push((name.clone(), v.to_string()));
                }
            }
        }
    }

    /// Textual value of a single field.
    pub fn get_field(&self, f: &StyleField, version: AssVersion) -> String {
        match f {
            // a comma would end the field early: Aegisub writes it as ';'
            StyleField::Name => self.name.replace(',', ";"),
            StyleField::Fontname => self.font.replace(',', ";"),
            StyleField::Fontsize => fmt_f64(self.size),
            StyleField::PrimaryColour => self.primary.to_style_string(),
            StyleField::SecondaryColour => self.secondary.to_style_string(),
            StyleField::OutlineColour => self.outline_color.to_style_string(),
            StyleField::BackColour => self.back.to_style_string(),
            StyleField::Bold => {
                if version == AssVersion::Ssa || self.bold == 1 || self.bold == -1 {
                    if self.bold != 0 {
                        "-1".into()
                    } else {
                        "0".into()
                    }
                } else {
                    self.bold.to_string()
                }
            }
            StyleField::Italic => flag(self.italic),
            StyleField::Underline => flag(self.underline),
            StyleField::StrikeOut => flag(self.strikeout),
            StyleField::ScaleX => fmt_f64(self.scale_x),
            StyleField::ScaleY => fmt_f64(self.scale_y),
            StyleField::Spacing => fmt_f64(self.spacing),
            StyleField::Angle => fmt_f64(self.angle),
            StyleField::BorderStyle => self.border_style.to_string(),
            StyleField::Outline => fmt_f64(self.outline),
            StyleField::Shadow => fmt_f64(self.shadow),
            StyleField::Alignment => {
                if version == AssVersion::Ssa {
                    numpad_to_ssa(self.alignment).to_string()
                } else {
                    self.alignment.to_string()
                }
            }
            StyleField::MarginL => self.margin_l.to_string(),
            StyleField::MarginR => self.margin_r.to_string(),
            StyleField::MarginV | StyleField::MarginT => self.margin_v.to_string(),
            StyleField::MarginB => self.margin_b.to_string(),
            StyleField::AlphaLevel => self.alpha_level.to_string(),
            StyleField::Encoding => self.encoding.to_string(),
            StyleField::RelativeTo => self.relative_to.to_string(),
            StyleField::Unknown(name) => self
                .extra
                .iter()
                .find(|(n, _)| n == name)
                .map(|(_, v)| v.clone())
                .unwrap_or_default(),
        }
    }

    pub fn is_bold(&self) -> bool {
        matches!(self.bold, 1 | -1) || self.bold >= 700
    }
}

fn flag(b: bool) -> String {
    if b { "-1".into() } else { "0".into() }
}

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum EventKind {
    #[default]
    Dialogue,
    Comment,
    Picture,
    Sound,
    Movie,
    Command,
}

impl EventKind {
    pub fn from_key(k: &str) -> Option<EventKind> {
        Some(match k.trim().to_ascii_lowercase().as_str() {
            "dialogue" => EventKind::Dialogue,
            "comment" => EventKind::Comment,
            "picture" => EventKind::Picture,
            "sound" => EventKind::Sound,
            "movie" => EventKind::Movie,
            "command" => EventKind::Command,
            _ => return None,
        })
    }

    pub fn key(self) -> &'static str {
        match self {
            EventKind::Dialogue => "Dialogue",
            EventKind::Comment => "Comment",
            EventKind::Picture => "Picture",
            EventKind::Sound => "Sound",
            EventKind::Movie => "Movie",
            EventKind::Command => "Command",
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Event {
    pub kind: EventKind,
    pub layer: i32,
    pub start: Time,
    pub end: Time,
    pub style: String,
    pub actor: String,
    pub margin_l: i32,
    pub margin_r: i32,
    /// MarginV (MarginT for v4++).
    pub margin_v: i32,
    pub margin_b: i32,
    pub effect: String,
    pub text: String,
    /// Values of `Unknown` format fields, by field name.
    pub extra: Vec<(String, String)>,
}

impl Event {
    pub fn dialogue(start: Time, end: Time, style: &str, text: &str) -> Self {
        Event {
            start,
            end,
            style: style.into(),
            text: text.into(),
            ..Default::default()
        }
    }

    #[inline]
    pub fn is_comment(&self) -> bool {
        self.kind == EventKind::Comment
    }

    #[inline]
    pub fn duration(&self) -> Time {
        self.end - self.start
    }

    pub fn set_field(&mut self, f: &EventField, v: &str) {
        match f {
            EventField::Layer => self.layer = parse_i32(v),
            // SSA "Marked=0" carries no information we keep; layer stays 0.
            EventField::Marked => {}
            EventField::Start => self.start = Time::from_ass_field(v),
            EventField::End => self.end = Time::from_ass_field(v),
            EventField::Style => self.style = trim_sp(v).to_string(),
            EventField::Name => self.actor = trim_sp(v).to_string(),
            EventField::MarginL => self.margin_l = parse_i32(v),
            EventField::MarginR => self.margin_r = parse_i32(v),
            EventField::MarginV | EventField::MarginT => self.margin_v = parse_i32(v),
            EventField::MarginB => self.margin_b = parse_i32(v),
            // Only spaces/tabs are trimmed: Kainote stores a form feed in Effect.
            EventField::Effect => self.effect = trim_sp(v).to_string(),
            EventField::Text => self.text = v.to_string(),
            EventField::Unknown(name) => {
                if let Some(e) = self.extra.iter_mut().find(|(n, _)| n == name) {
                    e.1 = v.to_string();
                } else {
                    self.extra.push((name.clone(), v.to_string()));
                }
            }
        }
    }

    /// Whether `v` holds this event's value of `f` (what `set_field` would store),
    /// without allocating: the writer asks this of every field of every line.
    pub fn field_matches(&self, f: &EventField, v: &str) -> bool {
        match f {
            EventField::Layer => self.layer == parse_i32(v),
            EventField::Marked => true,
            EventField::Start => self.start == Time::from_ass_field(v),
            EventField::End => self.end == Time::from_ass_field(v),
            EventField::Style => self.style == trim_sp(v),
            EventField::Name => self.actor == trim_sp(v),
            EventField::MarginL => self.margin_l == parse_i32(v),
            EventField::MarginR => self.margin_r == parse_i32(v),
            EventField::MarginV | EventField::MarginT => self.margin_v == parse_i32(v),
            EventField::MarginB => self.margin_b == parse_i32(v),
            EventField::Effect => self.effect == trim_sp(v),
            EventField::Text => self.text == v,
            EventField::Unknown(name) => {
                let cur = self.extra.iter().find(|(n, _)| n == name);
                cur.map_or("", |(_, x)| x.as_str()) == v
            }
        }
    }

    pub fn get_field(&self, f: &EventField) -> String {
        match f {
            EventField::Layer => self.layer.to_string(),
            EventField::Marked => "Marked=0".into(),
            EventField::Start => self.start.to_ass(),
            EventField::End => self.end.to_ass(),
            // Aegisub's GetEntryData: a comma would end the field early, a line break the line
            EventField::Style => self.style.replace(',', ";"),
            EventField::Name => self.actor.replace(',', ";"),
            EventField::MarginL => self.margin_l.to_string(),
            EventField::MarginR => self.margin_r.to_string(),
            EventField::MarginV | EventField::MarginT => self.margin_v.to_string(),
            EventField::MarginB => self.margin_b.to_string(),
            EventField::Effect => self.effect.replace(',', ";"),
            EventField::Text => self.text.replace(['\r', '\n'], ""),
            EventField::Unknown(name) => self
                .extra
                .iter()
                .find(|(n, _)| n == name)
                .map(|(_, v)| v.clone())
                .unwrap_or_default(),
        }
    }
}

/// Split the payload of a `Key: a,b,c` line into exactly `n` fields; the last
/// field receives the remainder (it may contain commas). Missing fields are
/// returned as empty strings; `None` if there are fewer than `n` fields and
/// `strict` is set.
pub fn split_fields(payload: &str, n: usize) -> (Vec<&str>, bool) {
    let mut out = Vec::with_capacity(n);
    let mut rest = payload;
    for _ in 0..n.saturating_sub(1) {
        match rest.find(',') {
            Some(p) => {
                out.push(&rest[..p]);
                rest = &rest[p + 1..];
            }
            None => {
                out.push(rest);
                // remaining fields missing
                while out.len() < n {
                    out.push("");
                }
                return (out, false);
            }
        }
    }
    if n > 0 {
        out.push(rest);
    }
    (out, true)
}

/// Split a `Key: payload` line. Returns `(key, prefix_len)` where `prefix_len`
/// covers the key, colon, and any following spaces/tabs.
pub fn split_key(line: &str) -> Option<(&str, usize)> {
    let colon = line.find(':')?;
    let key = &line[..colon];
    let mut p = colon + 1;
    let b = line.as_bytes();
    while p < b.len() && (b[p] == b' ' || b[p] == b'\t') {
        p += 1;
    }
    Some((key, p))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ssa_alignment_roundtrip() {
        for a in 1..=9u8 {
            assert_eq!(ssa_to_numpad(numpad_to_ssa(a)), a);
        }
        assert_eq!(ssa_to_numpad(2), 2);
        assert_eq!(ssa_to_numpad(6), 8);
        assert_eq!(ssa_to_numpad(10), 5);
    }

    #[test]
    fn float_format() {
        assert_eq!(fmt_f64(100.0), "100");
        assert_eq!(fmt_f64(2.5), "2.5");
        assert_eq!(fmt_f64(0.1 + 0.2), "0.3");
        assert_eq!(fmt_f64(-0.0), "0");
    }

    #[test]
    fn fields() {
        let (f, ok) = split_fields("0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Hello, world", 10);
        assert!(ok);
        assert_eq!(f[9], "Hello, world");
        let (f, ok) = split_fields("0,1", 4);
        assert!(!ok);
        assert_eq!(f, vec!["0", "1", "", ""]);
        assert_eq!(split_key("Dialogue: 0,1"), Some(("Dialogue", 10)));
        assert_eq!(split_key("Title:x"), Some(("Title", 6)));
    }

    #[test]
    fn written_fields_cannot_break_the_line() {
        let ev = Event {
            style: "A,B".into(),
            actor: "Ann, Bob".into(),
            effect: "x,y".into(),
            text: "one\r\ntwo, three\n".into(),
            ..Default::default()
        };
        let fmt = EventField::default_format(AssVersion::Ass);
        let line: Vec<String> = fmt.iter().map(|f| ev.get_field(f)).collect();
        let line = line.join(",");
        let (f, ok) = split_fields(&line, 10);
        assert!(ok);
        assert_eq!(
            (f[3], f[4], f[8], f[9]),
            ("A;B", "Ann; Bob", "x;y", "onetwo, three")
        );
        let st = Style {
            name: "a,b".into(),
            font: "F, G".into(),
            ..Default::default()
        };
        assert_eq!(st.get_field(&StyleField::Name, AssVersion::Ass), "a;b");
        assert_eq!(st.get_field(&StyleField::Fontname, AssVersion::Ass), "F; G");
    }
}
