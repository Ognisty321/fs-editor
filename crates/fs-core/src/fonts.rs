//! Fonts a script uses and the characters drawn with each, the input of
//! Aegisub's Fonts Collector. With the `fontdb` feature, [`locate`] finds the
//! font files and the glyphs they lack.

use std::collections::{BTreeMap, BTreeSet};

use crate::tags::{Block, Line};
use crate::{Document, EventKind, Style};

/// One face as libass would request it.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FontUse {
    /// Family name without the vertical-text `@` prefix.
    pub family: String,
    /// CSS-like weight: 400 normal, 700 bold.
    pub weight: u16,
    pub italic: bool,
}

/// `\b` / style Bold to a weight (0 normal, 1 or -1 bold, >1 explicit).
fn weight(b: i32) -> u16 {
    match b {
        0 => 400,
        1 | -1 => 700,
        w if w > 1 => w.min(1000) as u16,
        _ => 400,
    }
}

fn from_style(s: Option<&Style>) -> FontUse {
    FontUse {
        family: s
            .map_or("Arial", |s| &s.font)
            .trim_start_matches('@')
            .to_string(),
        weight: s.map_or(400, |s| weight(s.bold)),
        italic: s.is_some_and(|s| s.italic),
    }
}

/// Every face used by non-comment lines, with the characters drawn in it.
/// Follows `\fn`, `\b`, `\i` and `\r` the way they apply to the text that
/// follows them; drawings and `\N`/`\n` contribute nothing, `\h` is U+00A0.
// shortcut: tags inside \t are ignored (\t cannot animate \fn/\b/\i in libass anyway).
pub fn used_fonts(doc: &Document) -> BTreeMap<FontUse, BTreeSet<char>> {
    let mut out: BTreeMap<FontUse, BTreeSet<char>> = BTreeMap::new();
    for e in doc.events() {
        let ev = &e.event;
        if ev.kind == EventKind::Comment {
            continue;
        }
        let base = from_style(doc.style(&ev.style));
        let mut cur = base.clone();
        for b in Line::parse(&ev.text).blocks {
            match b {
                Block::Override { tags, .. } => {
                    for t in tags {
                        let arg = t.arg(0).unwrap_or("").trim();
                        match t.name.as_str() {
                            "fn" if arg.is_empty() => cur.family = base.family.clone(),
                            "fn" => cur.family = arg.trim_start_matches('@').to_string(),
                            "b" => {
                                cur.weight = arg.parse().map_or(base.weight, weight);
                            }
                            "i" => cur.italic = arg.parse::<i32>().map_or(base.italic, |i| i != 0),
                            "r" if arg.is_empty() => cur = base.clone(),
                            "r" => {
                                cur = doc.style(arg).map_or(base.clone(), |s| from_style(Some(s)))
                            }
                            _ => {}
                        }
                    }
                }
                Block::Text(t) => {
                    let chars = out.entry(cur.clone()).or_default();
                    let mut it = t.chars().peekable();
                    while let Some(c) = it.next() {
                        if c == '\\' && matches!(it.peek(), Some('N' | 'n' | 'h')) {
                            if it.next() == Some('h') {
                                chars.insert('\u{a0}');
                            }
                        } else {
                            chars.insert(c);
                        }
                    }
                }
                _ => {}
            }
        }
    }
    out.retain(|_, c| !c.is_empty());
    out
}

/// Where a used face was found.
#[cfg(feature = "fontdb")]
#[derive(Debug, Clone)]
pub struct Located {
    pub path: std::path::PathBuf,
    pub index: u32,
    /// The file's weight/style differ from the request (libass will fake them).
    pub synthetic: bool,
    pub missing_glyphs: Vec<char>,
}

/// Find the file for each used face in `db` (system fonts plus anything the
/// caller loaded, e.g. fonts attached to the video). `None` = not installed.
#[cfg(feature = "fontdb")]
pub fn locate(
    db: &fontdb::Database,
    used: &BTreeMap<FontUse, BTreeSet<char>>,
) -> Vec<(FontUse, Option<Located>)> {
    used.iter()
        .map(|(f, chars)| {
            let q = fontdb::Query {
                families: &[fontdb::Family::Name(&f.family)],
                weight: fontdb::Weight(f.weight),
                style: if f.italic {
                    fontdb::Style::Italic
                } else {
                    fontdb::Style::Normal
                },
                ..Default::default()
            };
            let found = db.query(&q).or_else(|| by_gdi_name(db, f)).and_then(|id| {
                let face = db.face(id)?;
                let fontdb::Source::File(path) = &face.source else {
                    return None;
                };
                let missing = db
                    .with_face_data(id, |data, i| {
                        ttf_parser::Face::parse(data, i).map_or_else(
                            |_| chars.iter().copied().collect(),
                            |ff| {
                                chars
                                    .iter()
                                    .copied()
                                    .filter(|&c| !c.is_whitespace() && ff.glyph_index(c).is_none())
                                    .collect()
                            },
                        )
                    })
                    .unwrap_or_default();
                Some(Located {
                    path: path.clone(),
                    index: face.index,
                    synthetic: (f.weight >= 700) != (face.weight.0 >= 600)
                        || f.italic != (face.style != fontdb::Style::Normal),
                    missing_glyphs: missing,
                })
            });
            (f.clone(), found)
        })
        .collect()
}

/// libass (like GDI) also matches the legacy family (name ID 1), the full
/// name (4) and the PostScript name (6); fontdb indexes only the typographic
/// family, so "Signika SemiBold" would be missed. Closest weight wins.
#[cfg(feature = "fontdb")]
fn by_gdi_name(db: &fontdb::Database, f: &FontUse) -> Option<fontdb::ID> {
    use ttf_parser::name_id::{FAMILY, FULL_NAME, POST_SCRIPT_NAME};
    db.faces()
        .filter(|face| {
            db.with_face_data(face.id, |data, i| {
                ttf_parser::Face::parse(data, i).is_ok_and(|ff| {
                    ff.names().into_iter().any(|n| {
                        matches!(n.name_id, FAMILY | FULL_NAME | POST_SCRIPT_NAME)
                            && n.to_string()
                                .is_some_and(|s| s.eq_ignore_ascii_case(&f.family))
                    })
                })
            })
            .unwrap_or(false)
        })
        .min_by_key(|face| {
            let italic_miss = f.italic != (face.style != fontdb::Style::Normal);
            (italic_miss, (face.weight.0 as i32 - f.weight as i32).abs())
        })
        .map(|face| face.id)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn doc(lines: &[&str]) -> Document {
        let mut s = String::from(
            "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n\
             Style: Default,Arial,40,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n\
             Style: Sign,@Bungee,40,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n\n\
             [Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
        );
        for l in lines {
            s.push_str(l);
            s.push('\n');
        }
        Document::parse(&s)
    }

    #[test]
    fn follows_overrides() {
        let d = doc(&[
            r"Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,ab{\b1}c{\i1\fnFoo}d{\r}e\Nf\hg",
            r"Dialogue: 0,0:00:00.00,0:00:01.00,Sign,,0,0,0,,x{\rDefault}y{\p1}m 0 0 l 1 1{\p0}{\b700\fn}z",
            r"Comment: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,{\fnHidden}q",
        ]);
        let u = used_fonts(&d);
        let get = |f: &str, w: u16, i: bool| -> String {
            u[&FontUse {
                family: f.into(),
                weight: w,
                italic: i,
            }]
                .iter()
                .collect()
        };
        assert_eq!(get("Arial", 400, false), "abefgy\u{a0}");
        assert_eq!(get("Arial", 700, false), "c");
        assert_eq!(get("Foo", 700, true), "d");
        assert_eq!(get("Bungee", 700, false), "xz");
        assert_eq!(u.len(), 4, "{u:?}");
    }
}
