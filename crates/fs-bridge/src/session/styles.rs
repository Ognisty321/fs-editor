//! The style manager's side: the script's styles, renames that follow into the lines, and the
//! catalog files.

use fs_core::model::Style;
use fs_core::ops;
use fs_core::tlmode;
use fs_core::{Color, Document};

use super::Session;
use crate::ffi::StyleData;

/// The syllables `text` would have on a line from `start` to `end` (a proposed split, before it
/// is written).
pub(super) const CATALOG_HEAD: &str = "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";

impl Session {
    pub fn styles(&self) -> Vec<StyleData> {
        self.ed.doc.styles().map(style_data).collect()
    }

    /// Replace style `index`, or add a new one when `index < 0`. Renaming
    /// updates the lines that used the old name.
    /// A rename moves the lines, the `\r` tags and the translation style's name along when
    /// `rename_lines` (Aegisub asks).
    pub fn set_style(&mut self, index: i64, style: &StyleData, rename_lines: bool) {
        let style = style.clone();
        self.edit("Edit style", move |d| {
            if index < 0 {
                let mut s = Style::default();
                apply_style_data(&mut s, &style);
                d.add_style(s);
                return;
            }
            let Some(s) = d.style_mut(index as usize) else {
                return;
            };
            let old = s.name.clone();
            apply_style_data(s, &style);
            if old != style.name && rename_lines {
                // translation rows pair by that style
                if tlmode::tl_style(d).as_deref() == Some(old.as_str()) {
                    d.set_info("TLMode Style", &style.name);
                }
                for i in 0..d.event_count() {
                    let e = d.event(i).unwrap();
                    let text = rename_reset_tags(&e.text, &old, &style.name);
                    if e.style == old || text.is_some() {
                        let e = d.event_mut(i).unwrap();
                        if e.style == old {
                            e.style = style.name.clone();
                        }
                        if let Some(t) = text {
                            e.text = t;
                        }
                    }
                }
            }
        });
    }

    /// `style_with_changes` on the styles at `indices`, in one undo step; how many changed.
    pub fn change_styles(
        &mut self,
        before: &StyleData,
        after: &StyleData,
        indices: &[usize],
    ) -> usize {
        let changed: Vec<(usize, StyleData)> = (self.ed.doc.styles().enumerate())
            .filter(|(i, _)| indices.contains(i))
            .map(|(i, s)| {
                let old = style_data(s);
                (i, style_with_changes(before, after, &old), old)
            })
            .filter(|(_, new, old)| new != old)
            .map(|(i, new, _)| (i, new))
            .collect();
        let n = changed.len();
        if n > 0 {
            self.edit("Edit styles", move |d| {
                for (i, new) in changed {
                    if let Some(s) = d.style_mut(i) {
                        apply_style_data(s, &new);
                    }
                }
            });
        }
        n
    }

    /// Delete styles no line uses; returns their names (no undo step when none).
    pub fn style_in_use(&self, name: &str) -> bool {
        let d = &self.ed.doc;
        tlmode::tl_style(d).as_deref() == Some(name)
            || (0..d.event_count()).any(|i| {
                let e = d.event(i).unwrap();
                e.style == name || rename_reset_tags(&e.text, name, "").is_some()
            })
    }

    pub fn clean_styles(&mut self) -> Vec<String> {
        let mut probe = self.ed.doc.clone();
        if ops::remove_unused_styles(&mut probe).is_empty() {
            return Vec::new();
        }
        self.edit("Delete unused styles", ops::remove_unused_styles)
    }

    pub fn delete_styles(&mut self, indices: &[usize]) {
        let mut v = indices.to_vec();
        v.sort_unstable_by(|a, b| b.cmp(a));
        v.dedup();
        self.edit("Delete styles", |d| {
            for i in v {
                d.remove_style(i);
            }
        });
    }

    pub fn reorder_styles(&mut self, order: &[usize]) {
        self.edit("Move styles", |d| d.reorder_styles(order));
    }

    pub fn add_styles(&mut self, styles: &[StyleData]) {
        self.edit("Add styles", |d| put_styles(d, styles));
    }

    /// Aegisub's default style catalog of new files: a new script starts with its styles
    /// (replacing ones of the same name) and still counts as unchanged.
    pub fn start_with_styles(&mut self, styles: &[StyleData]) {
        put_styles(&mut self.ed.doc, styles);
        self.snapshot_loaded();
        self.refresh();
    }
}

fn style_data(s: &Style) -> StyleData {
    StyleData {
        name: s.name.clone(),
        font: s.font.clone(),
        size: s.size,
        primary: s.primary.to_style_string(),
        secondary: s.secondary.to_style_string(),
        outline_color: s.outline_color.to_style_string(),
        back: s.back.to_style_string(),
        bold: s.is_bold(),
        italic: s.italic,
        underline: s.underline,
        strikeout: s.strikeout,
        scale_x: s.scale_x,
        scale_y: s.scale_y,
        spacing: s.spacing,
        angle: s.angle,
        border_style: s.border_style,
        outline: s.outline,
        shadow: s.shadow,
        alignment: s.alignment as i32,
        margin_l: s.margin_l,
        margin_r: s.margin_r,
        margin_v: s.margin_v,
        encoding: s.encoding,
    }
}

/// `\r<old>` inside override blocks becomes `\r<new>` (Aegisub's StyleRenamer); None when nothing changed.
fn rename_reset_tags(text: &str, old: &str, new: &str) -> Option<String> {
    let needle = format!("\\r{old}");
    let (mut out, mut depth, mut changed, mut i) = (String::with_capacity(text.len()), 0, false, 0);
    while i < text.len() {
        let rest = &text[i..];
        if depth > 0 && rest.starts_with(&needle) {
            // the name runs to the next tag or the end of the block
            let after = &rest[needle.len()..];
            if after.starts_with('\\') || after.starts_with('}') || after.is_empty() {
                out.push_str("\\r");
                out.push_str(new);
                i += needle.len();
                changed = true;
                continue;
            }
        }
        let c = rest.chars().next().unwrap();
        match c {
            '{' => depth += 1,
            '}' => depth = 0,
            _ => {}
        }
        out.push(c);
        i += c.len_utf8();
    }
    changed.then_some(out)
}

/// Styles of the same name take the new values, the others join the script.
fn put_styles(d: &mut Document, styles: &[StyleData]) {
    for st in styles {
        if let Some(s) = d.style_index(&st.name).and_then(|i| d.style_mut(i)) {
            apply_style_data(s, st);
        } else {
            let mut s = Style::default();
            apply_style_data(&mut s, st);
            d.add_style(s);
        }
    }
}

fn apply_style_data(s: &mut Style, d: &StyleData) {
    let color = |v: &str, old: Color| Color::parse_style(v).unwrap_or(old);
    s.name = d.name.clone();
    s.font = d.font.clone();
    s.size = d.size;
    s.primary = color(&d.primary, s.primary);
    s.secondary = color(&d.secondary, s.secondary);
    s.outline_color = color(&d.outline_color, s.outline_color);
    s.back = color(&d.back, s.back);
    if d.bold != s.is_bold() {
        s.bold = if d.bold { -1 } else { 0 };
    }
    s.italic = d.italic;
    s.underline = d.underline;
    s.strikeout = d.strikeout;
    s.scale_x = d.scale_x;
    s.scale_y = d.scale_y;
    s.spacing = d.spacing;
    s.angle = d.angle;
    s.border_style = d.border_style;
    s.outline = d.outline;
    s.shadow = d.shadow;
    s.alignment = d.alignment.clamp(1, 9) as u8;
    s.margin_l = d.margin_l;
    s.margin_r = d.margin_r;
    s.margin_v = d.margin_v;
    s.encoding = d.encoding;
}

/// Kainote's editing of several styles: what changed from `before` to `after` (not the name), set
/// on `target`.
pub fn style_with_changes(before: &StyleData, after: &StyleData, target: &StyleData) -> StyleData {
    let mut t = target.clone();
    macro_rules! take {
        ($($f:ident),*) => {$(
            if before.$f != after.$f {
                t.$f = after.$f.clone();
            }
        )*};
    }
    take!(
        font,
        size,
        primary,
        secondary,
        outline_color,
        back,
        bold,
        italic,
        underline,
        strikeout,
        scale_x,
        scale_y,
        spacing,
        angle,
        border_style,
        outline,
        shadow,
        alignment,
        margin_l,
        margin_r,
        margin_v,
        encoding
    );
    t
}

pub fn parse_style_lines(text: &str) -> Vec<StyleData> {
    let lines: String = text
        .lines()
        .map(|l| l.trim_start_matches('\u{feff}'))
        .filter(|l| l.starts_with("Style:"))
        .map(|l| format!("{l}\n"))
        .collect();
    Document::parse(&format!("{CATALOG_HEAD}{lines}"))
        .styles()
        .map(style_data)
        .collect()
}

pub fn style_lines(styles: &[StyleData]) -> String {
    let mut d = Document::parse(CATALOG_HEAD);
    for st in styles {
        let mut s = Style::default();
        apply_style_data(&mut s, st);
        d.add_style(s);
    }
    d.to_ass_string()
        .lines()
        .filter(|l| l.starts_with("Style:"))
        .map(|l| format!("{l}\n"))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::fixtures::{ASS, session};

    #[test]
    fn new_script_starts_with_catalog_styles() {
        let mut s = Session::default();
        let cat = parse_style_lines(
            "Style: Default,Verdana,50,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\
             Style: Signs,Arial,40,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,8,10,10,10,1",
        );
        s.start_with_styles(&cat);
        let names: Vec<_> = s
            .styles()
            .into_iter()
            .map(|st| (st.name, st.font))
            .collect();
        assert_eq!(
            names,
            [
                ("Default".to_string(), "Verdana".to_string()),
                ("Signs".to_string(), "Arial".to_string())
            ]
        );
        assert!(!s.file_info().modified && !s.can_undo());
    }

    #[test]
    fn catalog_files_round_trip() {
        let line = "Style: Sign,Open Sans,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,-1,0,0,0,100,100,0,0,4,2,150,8,-20,10,10,1";
        let text = format!(
            "\u{feff}{line}\r\n; a comment\r\nStyle: Second,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
        );
        let styles = parse_style_lines(&text);
        assert_eq!(styles.len(), 2);
        assert_eq!(
            (
                styles[0].name.as_str(),
                styles[0].border_style,
                styles[0].margin_l
            ),
            ("Sign", 4, -20)
        );
        assert_eq!(style_lines(&styles[..1]), format!("{line}\n"));
        assert!(parse_style_lines("").is_empty());
    }

    #[test]
    fn style_rename_and_delete() {
        let mut s = session(ASS);
        s.tl_enable(true);
        let mut r = s.row(0);
        r.text = "jeden".into();
        s.set_row(0, &r, "");
        let names = |s: &Session| {
            s.styles()
                .iter()
                .map(|x| x.name.clone())
                .collect::<Vec<_>>()
        };
        assert_eq!(names(&s), ["Default", "TLmode"]);
        let mut tl = s.styles()[1].clone();
        tl.name = "Orig".into();
        s.set_style(1, &tl, true);
        // the pair still pairs: the translation style's name followed
        assert_eq!(s.info_value("TLMode Style"), "Orig");
        assert_eq!((s.row_count(), s.row(0).original.as_str()), (3, "one"));
        let mut main = s.styles()[0].clone();
        main.name = "Main".into();
        s.set_style(0, &main, false);
        assert_eq!(s.row(1).style, "Default");
        s.delete_styles(&[0, 1, 0]);
        assert!(s.styles().is_empty());
        assert!(s.undo());
        assert_eq!(names(&s), ["Main", "Orig"]);
    }

    #[test]
    fn several_styles_take_the_changes_of_one() {
        let mut s = session(ASS);
        let mut other = s.styles()[0].clone();
        other.name = "Sign".into();
        other.font = "Impact".into();
        s.set_style(-1, &other, false);
        let before = s.styles()[0].clone();
        let after = StyleData {
            size: 60.0,
            bold: true,
            ..before.clone()
        };
        assert_eq!(s.change_styles(&before, &after, &[1, 7]), 1);
        let sign = &s.styles()[1];
        assert_eq!(
            (sign.name.as_str(), sign.font.as_str(), sign.size, sign.bold),
            ("Sign", "Impact", 60.0, true)
        );
        // nothing more to change: no step
        assert_eq!(s.change_styles(&before, &after, &[1]), 0);
        assert!(s.undo());
        assert_eq!(
            (s.styles()[1].size, s.styles()[1].bold),
            (before.size, before.bold)
        );
    }

    #[test]
    fn style_in_use_by_lines_and_reset_tags() {
        let mut s = session(ASS);
        assert!(s.style_in_use("Default"));
        assert!(!s.style_in_use("Sign"));
        let mut r = s.row(0);
        r.text = r"{\rSign}x".into();
        s.set_row(0, &r, "");
        assert!(s.style_in_use("Sign"));
        assert!(!s.style_in_use("Signs"));
    }

    #[test]
    fn rename_style_moves_reset_tags() {
        assert_eq!(
            rename_reset_tags(r"{\rSign\b1}a \rSign {\rSigns}{\rSign}", "Sign", "Title").as_deref(),
            Some(r"{\rTitle\b1}a \rSign {\rSigns}{\rTitle}")
        );
        assert_eq!(rename_reset_tags(r"{\r}x", "Sign", "Title"), None);
    }
}
