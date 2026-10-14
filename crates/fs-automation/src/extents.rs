//! `aegisub.text_extents` (Aegisub `CalculateTextExtents`).
//!
//! Windows uses GDI exactly like Aegisub, so karaoke templates and
//! typesetting scripts get identical numbers. Elsewhere the font is measured
//! with its own metrics, emulating GDI's cell-height sizing.

use fs_core::Style;

/// `(width, height, descent, external leading)` in script pixels.
pub fn text_extents(style: &Style, text: &str) -> (f64, f64, f64, f64) {
    let (w, h, d, e) = measure(style, text);
    let sx = style.scale_x / 100.0;
    let sy = style.scale_y / 100.0;
    (sx * w / 64.0, sy * h / 64.0, sy * d / 64.0, sy * e / 64.0)
}

#[cfg(windows)]
fn measure(style: &Style, text: &str) -> (f64, f64, f64, f64) {
    use windows_sys::Win32::Foundation::SIZE;
    use windows_sys::Win32::Graphics::Gdi::*;

    let size = style.size * 64.0;
    let spacing = style.spacing * 64.0;
    unsafe {
        let dc = CreateCompatibleDC(std::ptr::null_mut());
        if dc.is_null() {
            return (0.0, 0.0, 0.0, 0.0);
        }
        SetMapMode(dc, MM_TEXT);
        let mut lf: LOGFONTW = std::mem::zeroed();
        lf.lfHeight = size as i32;
        lf.lfWeight = if style.is_bold() { 700 } else { 400 };
        lf.lfItalic = style.italic as u8;
        lf.lfUnderline = style.underline as u8;
        lf.lfStrikeOut = style.strikeout as u8;
        lf.lfCharSet = style.encoding as u8;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = ANTIALIASED_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        for (dst, src) in lf
            .lfFaceName
            .iter_mut()
            .zip(style.font.encode_utf16().take(31))
        {
            *dst = src;
        }
        let font = CreateFontIndirectW(&lf);
        let old = SelectObject(dc, font as _);

        let wide: Vec<u16> = text.encode_utf16().collect();
        let mut sz = SIZE { cx: 0, cy: 0 };
        let (mut width, mut height) = (0.0, 0.0);
        if spacing != 0.0 {
            for u in &wide {
                GetTextExtentPoint32W(dc, u, 1, &mut sz);
                width += sz.cx as f64 + spacing;
                height = sz.cy as f64;
            }
        } else {
            GetTextExtentPoint32W(dc, wide.as_ptr(), wide.len() as i32, &mut sz);
            width = sz.cx as f64;
            height = sz.cy as f64;
        }
        let mut tm: TEXTMETRICW = std::mem::zeroed();
        GetTextMetricsW(dc, &mut tm);

        SelectObject(dc, old);
        DeleteObject(font as _);
        DeleteDC(dc);
        (
            width,
            height,
            tm.tmDescent as f64,
            tm.tmExternalLeading as f64,
        )
    }
}

#[cfg(not(windows))]
fn measure(style: &Style, text: &str) -> (f64, f64, f64, f64) {
    use std::sync::OnceLock;
    static DB: OnceLock<fontdb::Database> = OnceLock::new();
    let db = DB.get_or_init(|| {
        let mut db = fontdb::Database::new();
        db.load_system_fonts();
        // fontdb's generic sans-serif is "Arial", absent on most Linux systems
        let has = |f: &str| db.faces().any(|x| x.families.iter().any(|(n, _)| n == f));
        if let Some(f) = ["Arial", "Liberation Sans", "DejaVu Sans", "Noto Sans"]
            .into_iter()
            .find(|f| has(f))
        {
            db.set_sans_serif_family(f);
        }
        db
    });
    // fontconfig's metric-compatible substitutes, so extents match what libass draws on Linux
    let alias = match style.font.to_ascii_lowercase().as_str() {
        "arial" | "helvetica" => "Liberation Sans",
        "times new roman" | "times" => "Liberation Serif",
        "courier new" | "courier" => "Liberation Mono",
        _ => "",
    };
    let query = fontdb::Query {
        families: &[
            fontdb::Family::Name(&style.font),
            fontdb::Family::Name(alias),
            fontdb::Family::SansSerif,
        ],
        weight: if style.is_bold() {
            fontdb::Weight::BOLD
        } else {
            fontdb::Weight::NORMAL
        },
        style: if style.italic {
            fontdb::Style::Italic
        } else {
            fontdb::Style::Normal
        },
        ..Default::default()
    };
    let size = style.size * 64.0;
    let spacing = style.spacing * 64.0;
    let Some(id) = db.query(&query) else {
        return (0.0, size, 0.0, 0.0);
    };
    db.with_face_data(id, |data, index| {
        let Ok(face) = ttf_parser::Face::parse(data, index) else {
            return (0.0, size, 0.0, 0.0);
        };
        // GDI sizes a font so that winAscent + winDescent equals the cell height.
        let (asc, desc) = face
            .tables()
            .os2
            .map(|o| {
                (
                    o.windows_ascender() as f64,
                    o.windows_descender().unsigned_abs() as f64,
                )
            })
            .filter(|(a, d)| a + d > 0.0)
            .unwrap_or((face.ascender() as f64, -face.descender() as f64));
        let scale = size / (asc + desc);
        let mut width = 0.0;
        let mut prev = None;
        let kern = face
            .tables()
            .kern
            .and_then(|k| k.subtables.into_iter().find(|s| s.horizontal));
        for c in text.chars() {
            let g = face.glyph_index(c);
            let adv = g.and_then(|g| face.glyph_hor_advance(g)).unwrap_or(0) as f64;
            if spacing == 0.0
                && let (Some(p), Some(g), Some(k)) = (prev, g, kern.as_ref())
            {
                width += k.glyphs_kerning(p, g).unwrap_or(0) as f64 * scale;
            }
            width += adv * scale;
            if spacing != 0.0 {
                width += spacing;
            }
            prev = g;
        }
        let hhea_h = (face.ascender() - face.descender()) as f64;
        let extlead = (face.line_gap() as f64 - ((asc + desc) - hhea_h)).max(0.0) * scale;
        (width, size, desc * scale, extlead)
    })
    .unwrap_or((0.0, size, 0.0, 0.0))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scales_and_spacing() {
        let mut s = Style {
            font: "Arial".into(),
            size: 40.0,
            ..Default::default()
        };
        let (w, h, d, _) = text_extents(&s, "Hello");
        assert!(w > 40.0 && w < 200.0, "width {w}");
        assert!((h - 40.0).abs() < 2.0, "height {h}");
        assert!(d > 0.0 && d < h);
        s.scale_x = 200.0;
        let (w2, ..) = text_extents(&s, "Hello");
        assert!((w2 - 2.0 * w).abs() < 1e-6);
        s.scale_x = 100.0;
        s.spacing = 5.0;
        let (w3, ..) = text_extents(&s, "Hello");
        assert!((w3 - w - 25.0).abs() < 3.0, "{w3} vs {w}");
        assert_eq!(text_extents(&s, "").0, 0.0);
    }
}
