//! ASS colours. ASS stores colours as `&HAABBGGRR` where alpha 0 is opaque.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct Color {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    /// ASS alpha: 0 = opaque, 255 = fully transparent.
    pub a: u8,
}

impl Color {
    pub const WHITE: Color = Color::rgb(255, 255, 255);
    pub const BLACK: Color = Color::rgb(0, 0, 0);
    pub const RED: Color = Color::rgb(255, 0, 0);

    pub const fn rgb(r: u8, g: u8, b: u8) -> Self {
        Color { r, g, b, a: 0 }
    }

    pub const fn rgba(r: u8, g: u8, b: u8, a: u8) -> Self {
        Color { r, g, b, a }
    }

    /// From the packed `0xAABBGGRR` integer as used by ASS/VSFilter.
    pub const fn from_abgr(v: u32) -> Self {
        Color {
            r: (v & 0xff) as u8,
            g: ((v >> 8) & 0xff) as u8,
            b: ((v >> 16) & 0xff) as u8,
            a: ((v >> 24) & 0xff) as u8,
        }
    }

    pub const fn to_abgr(self) -> u32 {
        (self.a as u32) << 24 | (self.b as u32) << 16 | (self.g as u32) << 8 | self.r as u32
    }

    /// Parse a style colour field. Accepts `&HAABBGGRR`, `&HBBGGRR`, `H...`,
    /// `&H...&`, `0x...` and decimal integers (SSA v4 files). Mirrors libass's
    /// tolerant behaviour: trailing garbage is ignored, overflowing values wrap.
    pub fn parse_style(s: &str) -> Option<Self> {
        let s = s.trim();
        let (digits, radix) = if let Some(r) = s.strip_prefix("&H").or_else(|| s.strip_prefix("&h"))
        {
            (r, 16)
        } else if let Some(r) = s.strip_prefix('H').or_else(|| s.strip_prefix('h')) {
            (r, 16)
        } else if let Some(r) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
            (r, 16)
        } else {
            (s, 10)
        };
        let v = parse_prefix_int(digits, radix)?;
        Some(Color::from_abgr(v as u32))
    }

    /// Parse a colour from an override tag argument (`&HBBGGRR&`, `H...`,
    /// possibly with alpha). Alpha in a `\c` tag is ignored by renderers, so we
    /// return it with `a = 0`.
    pub fn parse_tag(s: &str) -> Option<Self> {
        let mut c = Self::parse_style(s)?;
        c.a = 0;
        Some(c)
    }

    /// Parse an alpha override argument (`&HAA&`, `&HAA`, `HAA`, decimal).
    pub fn parse_alpha(s: &str) -> Option<u8> {
        let s = s.trim();
        let (digits, radix) = if let Some(r) = s.strip_prefix("&H").or_else(|| s.strip_prefix("&h"))
        {
            (r, 16)
        } else if let Some(r) = s.strip_prefix('H').or_else(|| s.strip_prefix('h')) {
            (r, 16)
        } else {
            (s, 16)
        };
        let v = parse_prefix_int(digits, radix)?;
        Some((v & 0xff) as u8)
    }

    /// `&HAABBGGRR` as written in style lines.
    pub fn to_style_string(self) -> String {
        format!("&H{:02X}{:02X}{:02X}{:02X}", self.a, self.b, self.g, self.r)
    }

    /// `&HBBGGRR&` as written in `\c` tags.
    pub fn to_tag_string(self) -> String {
        format!("&H{:02X}{:02X}{:02X}&", self.b, self.g, self.r)
    }

    /// `&HAA&` as written in `\alpha` tags.
    pub fn alpha_tag_string(a: u8) -> String {
        format!("&H{a:02X}&")
    }

    /// `#RRGGBB` / `#RRGGBBAA` (HTML-style, alpha as opacity).
    pub fn to_html(self) -> String {
        if self.a == 0 {
            format!("#{:02X}{:02X}{:02X}", self.r, self.g, self.b)
        } else {
            format!(
                "#{:02X}{:02X}{:02X}{:02X}",
                self.r,
                self.g,
                self.b,
                255 - self.a
            )
        }
    }

    pub fn parse_html(s: &str) -> Option<Self> {
        let h = s.trim().strip_prefix('#')?;
        let v = u32::from_str_radix(h, 16).ok()?;
        match h.len() {
            6 => Some(Color::rgb((v >> 16) as u8, (v >> 8) as u8, v as u8)),
            8 => Some(Color::rgba(
                (v >> 24) as u8,
                (v >> 16) as u8,
                (v >> 8) as u8,
                255 - v as u8,
            )),
            3 => {
                let r = ((v >> 8) & 0xf) as u8;
                let g = ((v >> 4) & 0xf) as u8;
                let b = (v & 0xf) as u8;
                Some(Color::rgb(r * 17, g * 17, b * 17))
            }
            _ => None,
        }
    }
}

/// Parse the longest valid prefix of `s` as an unsigned integer in `radix`,
/// with wrapping on overflow (like `strtoul` cast to 32 bits in VSFilter).
fn parse_prefix_int(s: &str, radix: u32) -> Option<u64> {
    let mut v: u64 = 0;
    let mut any = false;
    let s = s.trim_start();
    let (neg, s) = match s.strip_prefix('-') {
        Some(r) if radix == 10 => (true, r),
        _ => (false, s.strip_prefix('+').unwrap_or(s)),
    };
    for c in s.chars() {
        match c.to_digit(radix) {
            Some(d) => {
                v = v.wrapping_mul(radix as u64).wrapping_add(d as u64) & 0xffff_ffff_ffff;
                any = true;
            }
            None => break,
        }
    }
    if !any {
        return None;
    }
    let v32 = v as u32;
    Some(if neg {
        (v32 as i32).wrapping_neg() as u32 as u64
    } else {
        v32 as u64
    })
}

/// Aegisub's `ycbcr_converter::rgb_to_rgb`: a colour goes to YCbCr with one
/// "YCbCr Matrix" (`TV.601`, `PC.709`, ...) and back with another, so a script
/// made for one video colour space looks the same on another.
#[derive(Debug, Clone, Copy)]
pub struct MatrixConversion {
    to_ycbcr: [f64; 9],
    from_ycbcr: [f64; 9],
    shift: [f64; 3],
}

impl MatrixConversion {
    /// None when either header is not an explicit matrix, or they are equal.
    pub fn new(from: &str, to: &str) -> Option<Self> {
        fn parse(h: &str) -> Option<([f64; 3], bool)> {
            let (range, m) = h.split_once('.')?;
            let full = match range {
                "TV" => false,
                "PC" => true,
                _ => return None,
            };
            let k = match m {
                "601" => [0.299, 0.587, 0.114],
                "709" => [0.2126, 0.7152, 0.0722],
                "FCC" => [0.3, 0.59, 0.11],
                "240M" => [0.212, 0.701, 0.087],
                _ => return None,
            };
            Some((k, full))
        }
        let ([kr, kg, kb], full) = parse(from)?;
        let ([dr, dg, db], dfull) = parse(to)?;
        if from == to {
            return None;
        }
        let mut to_ycbcr = [
            kr,
            kg,
            kb,
            -kr / (1.0 - kb),
            -kg / (1.0 - kb),
            1.0,
            1.0,
            -kg / (1.0 - kr),
            -kb / (1.0 - kr),
        ];
        let (rows, shift_to) = if full {
            ([1.0, 0.5, 0.5], [0.0, 128.0, 128.0])
        } else {
            (
                [219.0 / 255.0, 112.0 / 255.0, 112.0 / 255.0],
                [16.0, 128.0, 128.0],
            )
        };
        let mut from_ycbcr = [
            1.0,
            0.0,
            1.0 - dr,
            1.0,
            -(1.0 - db) * db / dg,
            -(1.0 - dr) * dr / dg,
            1.0,
            1.0 - db,
            0.0,
        ];
        let (cols, shift_from) = if dfull {
            ([1.0, 2.0, 2.0], [0.0, -128.0, -128.0])
        } else {
            (
                [255.0 / 219.0, 255.0 / 112.0, 255.0 / 112.0],
                [-16.0, -128.0, -128.0],
            )
        };
        for (i, v) in to_ycbcr.iter_mut().enumerate() {
            *v *= rows[i / 3];
        }
        for (i, v) in from_ycbcr.iter_mut().enumerate() {
            *v *= cols[i % 3];
        }
        let shift = [0, 1, 2].map(|i| shift_to[i] + shift_from[i]);
        Some(MatrixConversion {
            to_ycbcr,
            from_ycbcr,
            shift,
        })
    }

    /// Alpha is kept.
    pub fn convert(&self, c: Color) -> Color {
        let prod = |m: &[f64; 9], v: [f64; 3]| {
            [0, 1, 2].map(|r| m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2])
        };
        let y = prod(&self.to_ycbcr, [c.r, c.g, c.b].map(f64::from));
        let [r, g, b] = prod(&self.from_ycbcr, [0, 1, 2].map(|i| y[i] + self.shift[i]))
            .map(|v| (v + 0.5).clamp(0.0, 255.0) as u8);
        Color { r, g, b, a: c.a }
    }
}

impl fmt::Display for Color {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_style_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matrix_conversion() {
        assert!(MatrixConversion::new("TV.601", "TV.601").is_none());
        assert!(MatrixConversion::new("None", "TV.709").is_none());
        let c = MatrixConversion::new("TV.601", "TV.709").unwrap();
        assert_eq!(
            c.convert(Color::rgba(255, 0, 0, 7)),
            Color::rgba(255, 25, 0, 7)
        );
        assert_eq!(c.convert(Color::rgb(0, 128, 255)), Color::rgb(0, 123, 255));
        assert_eq!(c.convert(Color::rgb(77, 77, 77)), Color::rgb(77, 77, 77));
        let range = MatrixConversion::new("TV.601", "PC.601").unwrap();
        assert_eq!(
            range.convert(Color::rgb(255, 0, 0)),
            Color::rgb(239, 15, 15)
        );
    }

    #[test]
    fn style_colours() {
        assert_eq!(Color::parse_style("&H00FFFFFF"), Some(Color::WHITE));
        assert_eq!(Color::parse_style("&H000000FF"), Some(Color::RED));
        assert_eq!(
            Color::parse_style("&H80000000"),
            Some(Color::rgba(0, 0, 0, 0x80))
        );
        assert_eq!(Color::parse_style("16777215"), Some(Color::WHITE));
        assert_eq!(Color::parse_style("&HFF"), Some(Color::RED));
        assert_eq!(Color::parse_style("&H0000FF&"), Some(Color::RED));
        assert_eq!(Color::parse_style("xyz"), None);
        assert_eq!(Color::RED.to_style_string(), "&H000000FF");
        assert_eq!(Color::rgba(1, 2, 3, 4).to_style_string(), "&H04030201");
    }

    #[test]
    fn tag_colours() {
        assert_eq!(Color::parse_tag("&H0000FF&"), Some(Color::RED));
        assert_eq!(Color::parse_tag("H0000FF"), Some(Color::RED));
        assert_eq!(Color::RED.to_tag_string(), "&H0000FF&");
        assert_eq!(Color::parse_alpha("&H80&"), Some(0x80));
        assert_eq!(Color::parse_alpha("&HFF"), Some(0xff));
    }

    #[test]
    fn html() {
        assert_eq!(Color::parse_html("#FF0000"), Some(Color::RED));
        assert_eq!(Color::RED.to_html(), "#FF0000");
        let c = Color::rgba(1, 2, 3, 0x40);
        assert_eq!(Color::parse_html(&c.to_html()), Some(c));
    }
}
