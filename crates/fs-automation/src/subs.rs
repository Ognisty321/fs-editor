//! The Automation 4 subtitles object (Aegisub `auto4_lua_assfile.cpp`):
//! a flat 1-based vector `[info..., styles..., events...]` over the document.
//! Untouched lines keep their original entries, so committing writes them
//! back byte for byte.

use fs_core::ass::{InfoEntry, Section, SectionBody, SectionKind, StyleEntry};
use fs_core::model::{EventKind, fmt_f64};
use fs_core::text::RawLine;
use fs_core::{Color, Document, Event, EventEntry, Style, Time};
use mlua::{IntoLuaMulti, Lua, MultiValue, Table, UserData, UserDataMethods, Value};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

/// `[Script Info]` keys Aegisub keeps as project properties instead of
/// exposing them as info lines.
const HIDDEN_INFO: &[&str] = &[
    "automation scripts",
    "export filters",
    "export encoding",
    "last style storage",
    "audio file",
    "audio uri",
    "video file",
    "timecodes file",
    "keyframes file",
    "video zoom percent",
    "aegisub video zoom percent",
    "scroll position",
    "aegisub scroll position",
    "active line",
    "aegisub active line",
    "video position",
    "aegisub video position",
    "video ar mode",
    "video ar value",
    "collisions",
];

fn hidden_info(key: &str) -> bool {
    let k = key.to_ascii_lowercase();
    HIDDEN_INFO.contains(&k.as_str()) || k.starts_with("automation settings ")
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LuaEvent {
    pub event: Event,
    pub extra: Vec<(String, String)>,
}

#[derive(Clone, Debug)]
pub(crate) enum Line {
    Info {
        key: String,
        value: String,
        base: Option<InfoEntry>,
    },
    Style {
        base: Option<Arc<StyleEntry>>,
        style: Option<Style>,
    },
    Event {
        base: Option<Arc<EventEntry>>,
        event: Option<LuaEvent>,
    },
}

impl Line {
    fn class(&self) -> u8 {
        match self {
            Line::Info { .. } => 0,
            Line::Style { .. } => 1,
            Line::Event { .. } => 2,
        }
    }
    pub fn is_event(&self) -> bool {
        matches!(self, Line::Event { .. })
    }
}

// ---- Aegisub extradata ---------------------------------------------------------

fn inline_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'#'
            && i + 2 < b.len()
            && b[i + 1].is_ascii_hexdigit()
            && b[i + 2].is_ascii_hexdigit()
        {
            out.push(u8::from_str_radix(&s[i + 1..i + 3], 16).unwrap());
            i += 3;
            continue;
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn inline_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        if (c as u32) < 32 || matches!(c, '#' | ',' | ':' | '|') {
            out.push_str(&format!("#{:02X}", c as u32));
        } else {
            out.push(c);
        }
    }
    out
}

fn uu_decode(s: &str) -> String {
    let b: Vec<u8> = s.bytes().filter(|c| !matches!(c, b'\r' | b'\n')).collect();
    let mut out = Vec::new();
    let mut pos = 0;
    while pos + 1 < b.len() {
        let n = (b.len() - pos).min(4);
        let mut src = [0u8; 4];
        for i in 0..n {
            src[i] = b[pos + i].wrapping_sub(33);
        }
        out.push(src[0] << 2 | src[1] >> 4);
        if n > 2 {
            out.push((src[1] & 0xF) << 4 | src[2] >> 2);
        }
        if n > 3 {
            out.push((src[2] & 0x3) << 6 | src[3]);
        }
        pos += 4;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// `id -> (key, value)` from `[Aegisub Extradata]`.
fn extradata(doc: &Document) -> HashMap<u32, (String, String)> {
    let mut map = HashMap::new();
    for l in doc.raw_section(SectionKind::Extradata).unwrap_or(&[]) {
        let Some(rest) = l.text.strip_prefix("Data:") else {
            continue;
        };
        let mut it = rest.trim_start().splitn(3, ',');
        let (Some(id), Some(key), Some(val)) = (it.next(), it.next(), it.next()) else {
            continue;
        };
        let Ok(id) = id.trim().parse() else { continue };
        let value = match val.split_at_checked(1) {
            Some(("u", v)) => uu_decode(v),
            Some(("e", v)) => inline_decode(v),
            _ => inline_decode(val),
        };
        map.insert(id, (inline_decode(key), value));
    }
    map
}

/// Split `{=1=2}text` into extradata ids and the text.
fn split_extra_ids(text: &str) -> (Vec<u32>, &str) {
    if let Some(rest) = text.strip_prefix("{=")
        && let Some(end) = rest.find('}')
    {
        let ids: Option<Vec<u32>> = rest[..end].split('=').map(|s| s.parse().ok()).collect();
        if let Some(ids) = ids {
            return (ids, &rest[end + 1..]);
        }
    }
    (Vec::new(), text)
}

fn lua_event(e: &Event, extra: &HashMap<u32, (String, String)>) -> LuaEvent {
    let (ids, text) = split_extra_ids(&e.text);
    let mut ev = e.clone();
    ev.text = text.to_string();
    LuaEvent {
        event: ev,
        extra: ids.iter().filter_map(|i| extra.get(i).cloned()).collect(),
    }
}

// ---- subtitles state -----------------------------------------------------------

pub(crate) struct State {
    pub doc: Document,
    extra: HashMap<u32, (String, String)>,
    pub lines: Vec<Line>,
    can_modify: bool,
    can_set_undo: bool,
    modified: bool,
    pub any_modified: bool,
    pub info_touched: bool,
    pub undo_points: Vec<(String, Vec<Line>, bool)>,
}

impl State {
    pub fn new(doc: &Document, can_modify: bool, can_set_undo: bool) -> State {
        let extra = extradata(doc);
        let mut lines = Vec::new();
        if let Some(info) = doc.info_section() {
            for e in &info.entries {
                if !hidden_info(&e.key) {
                    lines.push(Line::Info {
                        key: e.key.clone(),
                        value: e.value.clone(),
                        base: Some(e.clone()),
                    });
                }
            }
        }
        if let Some(st) = doc.styles_section() {
            lines.extend(st.entries.iter().map(|e| Line::Style {
                base: Some(e.clone()),
                style: None,
            }));
        }
        lines.extend(doc.events().iter().map(|e| Line::Event {
            base: Some(e.clone()),
            event: None,
        }));
        State {
            doc: doc.clone(),
            extra,
            lines,
            can_modify,
            can_set_undo,
            modified: false,
            any_modified: false,
            info_touched: false,
            undo_points: Vec::new(),
        }
    }

    pub fn first_event_index(&self) -> usize {
        self.lines
            .iter()
            .position(Line::is_event)
            .unwrap_or(self.lines.len())
    }

    fn touch(&mut self, line: &Line) {
        self.modified = true;
        self.any_modified = true;
        if line.class() == 0 {
            self.info_touched = true;
        }
    }

    /// Build the document for a lines vector.
    pub fn build(&self, lines: &[Line], info_touched: bool) -> Document {
        let mut doc = self.doc.clone();
        let le = doc.default_ending;
        let mut extra = self.extra.clone();
        let mut new_data: Vec<String> = Vec::new();

        if info_touched {
            let mut entries: Vec<InfoEntry> = Vec::new();
            for l in lines {
                if let Line::Info { key, value, base } = l {
                    entries.push(match base {
                        Some(b) if &b.key == key && &b.value == value => b.clone(),
                        Some(b) => InfoEntry {
                            key: key.clone(),
                            value: value.clone(),
                            orig: None,
                            ..b.clone()
                        },
                        None => InfoEntry {
                            leading: Vec::new(),
                            key: key.clone(),
                            value: value.clone(),
                            orig: None,
                            ending: le,
                        },
                    });
                }
            }
            match doc
                .sections
                .iter()
                .position(|s| s.kind == SectionKind::ScriptInfo)
            {
                Some(i) => {
                    if let SectionBody::Info(info) = &mut Arc::make_mut(&mut doc.sections[i]).body {
                        // shortcut: project-property keys move after the visible ones
                        // when a script edits [Script Info]; harmless for readers.
                        let hidden: Vec<InfoEntry> = info
                            .entries
                            .drain(..)
                            .filter(|e| hidden_info(&e.key))
                            .collect();
                        info.entries = entries;
                        info.entries.extend(hidden);
                    }
                }
                None => {
                    for e in entries {
                        doc.set_info(&e.key, &e.value);
                    }
                }
            }
        }

        let styles: Vec<Arc<StyleEntry>> = lines
            .iter()
            .filter_map(|l| match l {
                Line::Style { base, style } => Some(match (base, style) {
                    (Some(b), None) => b.clone(),
                    (Some(b), Some(s)) if &b.style == s => b.clone(),
                    (Some(b), Some(s)) => Arc::new(StyleEntry {
                        leading: b.leading.clone(),
                        style: s.clone(),
                        orig: None,
                        ending: b.ending,
                    }),
                    (None, s) => Arc::new(StyleEntry {
                        leading: Vec::new(),
                        style: s.clone().unwrap_or_default(),
                        orig: None,
                        ending: le,
                    }),
                }),
                _ => None,
            })
            .collect();
        if !styles.is_empty() || doc.styles_section().is_some() {
            doc.ensure_styles().entries = styles;
        }

        let mut events: Vec<Arc<EventEntry>> = Vec::new();
        for l in lines {
            let Line::Event { base, event } = l else {
                continue;
            };
            let Some(le_ev) = event else {
                events.push(base.clone().expect("event line without data"));
                continue;
            };
            if let Some(b) = base
                && lua_event(&b.event, &self.extra) == *le_ev
            {
                events.push(b.clone());
                continue;
            }
            let mut ev = le_ev.event.clone();
            if !le_ev.extra.is_empty() {
                let mut ids: Vec<u32> = le_ev
                    .extra
                    .iter()
                    .map(|kv| match extra.iter().find(|(_, v)| *v == kv) {
                        Some((id, _)) => *id,
                        None => {
                            let id = extra.keys().max().map_or(0, |m| m + 1);
                            extra.insert(id, kv.clone());
                            new_data.push(format!(
                                "Data: {},{},e{}",
                                id,
                                inline_encode(&kv.0),
                                inline_encode(&kv.1)
                            ));
                            id
                        }
                    })
                    .collect();
                ids.sort_unstable();
                ids.dedup();
                let prefix: String = ids.iter().map(|i| format!("={i}")).collect();
                ev.text = format!("{{{prefix}}}{}", ev.text);
            }
            events.push(match base {
                Some(b) => Arc::new(EventEntry {
                    id: b.id,
                    leading: b.leading.clone(),
                    event: ev,
                    orig: None,
                    ending: b.ending,
                }),
                None => doc.new_entry(ev),
            });
        }
        if !events.is_empty() || doc.events_section().is_some() {
            doc.ensure_events().entries = events;
        }

        if !new_data.is_empty() {
            let i = match doc
                .sections
                .iter()
                .position(|s| s.kind == SectionKind::Extradata)
            {
                Some(i) => i,
                None => {
                    doc.sections.push(Arc::new(Section {
                        kind: SectionKind::Extradata,
                        header: RawLine::new("[Aegisub Extradata]", le),
                        body: SectionBody::Raw(Vec::new()),
                    }));
                    doc.sections.len() - 1
                }
            };
            if let SectionBody::Raw(l) = &mut Arc::make_mut(&mut doc.sections[i]).body {
                // keep a trailing blank line last
                let at = l
                    .iter()
                    .rposition(|r| !r.text.trim().is_empty())
                    .map_or(0, |p| p + 1);
                for (k, d) in new_data.into_iter().enumerate() {
                    l.insert(at + k, RawLine::new(d, le));
                }
            }
        }
        doc
    }
}

// ---- Lua <-> line tables --------------------------------------------------------

type Res<T> = std::result::Result<T, String>;

fn ass_color(c: Color) -> String {
    format!("{}&", c.to_style_string())
}

/// agi::Color parsing (style colours and dialog values): `&HAABBGGRR&`,
/// `&HBBGGRR&`, decimal, `#RRGGBB`, `#RGB`, `rgb(r,g,b)`, `rgba(r,g,b,a)`.
/// Unparseable input is black, as in Aegisub.
pub(crate) fn parse_color(s: &str) -> Color {
    let s = s.trim();
    let hex = s.trim_start_matches('&');
    if let Some(h) = hex.strip_prefix(['H', 'h']) {
        let h = h.trim_end_matches('&');
        if matches!(h.len(), 6 | 8)
            && let Ok(v) = u32::from_str_radix(h, 16)
        {
            return Color::from_abgr(v);
        }
        return Color::default();
    }
    if s.starts_with('#') {
        return Color::parse_html(s)
            .filter(|_| s.len() != 9)
            .unwrap_or_default();
    }
    for (prefix, n) in [("rgba(", 4), ("rgb(", 3)] {
        if let Some(r) = s.strip_prefix(prefix).and_then(|r| r.strip_suffix(')')) {
            let v: Vec<u8> = r.split(',').filter_map(|x| x.trim().parse().ok()).collect();
            if v.len() == n {
                return Color::rgba(v[0], v[1], v[2], v.get(3).copied().unwrap_or(0));
            }
            return Color::default();
        }
    }
    s.parse::<u32>().map(Color::from_abgr).unwrap_or_default()
}

fn field_err(f: &str, class: &str, ty: &str) -> String {
    format!("Invalid or missing field '{f}' in '{class}' class subtitle line (expected {ty})")
}

fn get_str(t: &Table, f: &str, class: &str) -> Res<String> {
    match t.raw_get::<Value>(f).map_err(|e| e.to_string())? {
        Value::String(s) => Ok(s.to_string_lossy()),
        Value::Integer(i) => Ok(i.to_string()),
        Value::Number(n) => Ok(fmt_f64(n)),
        _ => Err(field_err(f, class, "string")),
    }
}

fn get_num(t: &Table, f: &str, class: &str) -> Res<f64> {
    match t.raw_get::<Value>(f).map_err(|e| e.to_string())? {
        Value::Integer(i) => Ok(i as f64),
        Value::Number(n) => Ok(n),
        Value::String(s) => s
            .to_str()
            .ok()
            .and_then(|s| s.trim().parse().ok())
            .ok_or_else(|| field_err(f, class, "number")),
        _ => Err(field_err(f, class, "number")),
    }
}

fn get_int(t: &Table, f: &str, class: &str) -> Res<i64> {
    get_num(t, f, class).map(|n| n.trunc() as i64)
}

fn get_bool(t: &Table, f: &str, class: &str) -> Res<bool> {
    match t.raw_get::<Value>(f).map_err(|e| e.to_string())? {
        Value::Boolean(b) => Ok(b),
        _ => Err(field_err(f, class, "boolean")),
    }
}

fn style_raw(s: &Style) -> String {
    format!(
        "Style: {},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
        s.name,
        s.font,
        fmt_f64(s.size),
        s.primary.to_style_string(),
        s.secondary.to_style_string(),
        s.outline_color.to_style_string(),
        s.back.to_style_string(),
        if s.is_bold() { -1 } else { 0 },
        if s.italic { -1 } else { 0 },
        if s.underline { -1 } else { 0 },
        if s.strikeout { -1 } else { 0 },
        fmt_f64(s.scale_x),
        fmt_f64(s.scale_y),
        fmt_f64(s.spacing),
        fmt_f64(s.angle),
        s.border_style,
        fmt_f64(s.outline),
        fmt_f64(s.shadow),
        s.alignment,
        s.margin_l,
        s.margin_r,
        s.margin_v,
        s.encoding
    )
}

fn event_raw(e: &Event) -> String {
    format!(
        "{}: {},{},{},{},{},{},{},{},{},{}",
        e.kind.key(),
        e.layer,
        e.start.clamp_ass().to_ass(),
        e.end.clamp_ass().to_ass(),
        e.style,
        e.actor,
        e.margin_l,
        e.margin_r,
        e.margin_v,
        e.effect,
        e.text
    )
}

pub(crate) fn style_table(lua: &Lua, s: &Style) -> mlua::Result<Table> {
    let t = lua.create_table()?;
    t.set("class", "style")?;
    t.set("section", "[V4+ Styles]")?;
    t.set("raw", style_raw(s))?;
    t.set("name", s.name.as_str())?;
    t.set("fontname", s.font.as_str())?;
    t.set("fontsize", s.size)?;
    t.set("color1", ass_color(s.primary))?;
    t.set("color2", ass_color(s.secondary))?;
    t.set("color3", ass_color(s.outline_color))?;
    t.set("color4", ass_color(s.back))?;
    t.set("bold", s.is_bold())?;
    t.set("italic", s.italic)?;
    t.set("underline", s.underline)?;
    t.set("strikeout", s.strikeout)?;
    t.set("scale_x", s.scale_x)?;
    t.set("scale_y", s.scale_y)?;
    t.set("spacing", s.spacing)?;
    t.set("angle", s.angle)?;
    t.set("borderstyle", s.border_style)?;
    t.set("outline", s.outline)?;
    t.set("shadow", s.shadow)?;
    t.set("align", s.alignment)?;
    t.set("margin_l", s.margin_l)?;
    t.set("margin_r", s.margin_r)?;
    t.set("margin_t", s.margin_v)?;
    t.set("margin_b", s.margin_v)?;
    t.set("encoding", s.encoding)?;
    t.set("relative_to", 2)?;
    Ok(t)
}

fn event_table(lua: &Lua, e: &LuaEvent) -> mlua::Result<Table> {
    let ev = &e.event;
    let t = lua.create_table()?;
    t.set("class", "dialogue")?;
    t.set("section", "[Events]")?;
    t.set("comment", ev.is_comment())?;
    t.set("layer", ev.layer)?;
    t.set("start_time", ev.start.0)?;
    t.set("end_time", ev.end.0)?;
    t.set("style", ev.style.as_str())?;
    t.set("actor", ev.actor.as_str())?;
    t.set("margin_l", ev.margin_l)?;
    t.set("margin_r", ev.margin_r)?;
    t.set("margin_t", ev.margin_v)?;
    t.set("margin_b", ev.margin_v)?;
    t.set("effect", ev.effect.as_str())?;
    t.set("text", ev.text.as_str())?;
    let x = lua.create_table()?;
    for (k, v) in &e.extra {
        x.set(k.as_str(), v.as_str())?;
    }
    t.set("extra", x)?;
    let mut raw_ev = ev.clone();
    if !e.extra.is_empty() {
        // ids are assigned on commit; show the shape Aegisub uses
        raw_ev.text = format!("{{={}}}{}", e.extra.len(), ev.text);
    }
    t.set("raw", event_raw(&raw_ev).replace(['\r', '\n'], ""))?;
    Ok(t)
}

fn line_table(lua: &Lua, st: &State, l: &Line) -> mlua::Result<Table> {
    match l {
        Line::Info { key, value, .. } => {
            let t = lua.create_table()?;
            t.set("class", "info")?;
            t.set("section", "[Script Info]")?;
            t.set("key", key.as_str())?;
            t.set("value", value.as_str())?;
            t.set("raw", format!("{key}: {value}"))?;
            Ok(t)
        }
        Line::Style { base, style } => {
            let s = style
                .as_ref()
                .unwrap_or_else(|| &base.as_ref().unwrap().style);
            style_table(lua, s)
        }
        Line::Event { base, event } => match event {
            Some(e) => event_table(lua, e),
            None => event_table(lua, &lua_event(&base.as_ref().unwrap().event, &st.extra)),
        },
    }
}

/// Lua table → style (the `class` was already checked). `base` supplies
/// fields Aegisub doesn't expose (explicit weights, v4++ MarginB, …).
pub(crate) fn table_style(t: &Table, base: Option<&Style>) -> Res<Style> {
    let c = "style";
    let mut s = base.cloned().unwrap_or_default();
    s.name = get_str(t, "name", c)?;
    s.font = get_str(t, "fontname", c)?;
    s.size = get_num(t, "fontsize", c)?;
    s.primary = parse_color(&get_str(t, "color1", c)?);
    s.secondary = parse_color(&get_str(t, "color2", c)?);
    s.outline_color = parse_color(&get_str(t, "color3", c)?);
    s.back = parse_color(&get_str(t, "color4", c)?);
    let bold = get_bool(t, "bold", c)?;
    if bold != s.is_bold() {
        s.bold = if bold { -1 } else { 0 };
    }
    s.italic = get_bool(t, "italic", c)?;
    s.underline = get_bool(t, "underline", c)?;
    s.strikeout = get_bool(t, "strikeout", c)?;
    s.scale_x = get_num(t, "scale_x", c)?;
    s.scale_y = get_num(t, "scale_y", c)?;
    s.spacing = get_num(t, "spacing", c)?;
    s.angle = get_num(t, "angle", c)?;
    s.border_style = get_int(t, "borderstyle", c)? as i32;
    s.outline = get_num(t, "outline", c)?;
    s.shadow = get_num(t, "shadow", c)?;
    s.alignment = get_int(t, "align", c)?.clamp(1, 9) as u8;
    s.margin_l = get_int(t, "margin_l", c)? as i32;
    s.margin_r = get_int(t, "margin_r", c)? as i32;
    let mv = get_int(t, "margin_t", c)? as i32;
    if mv != s.margin_v {
        s.margin_b = mv;
    }
    s.margin_v = mv;
    s.encoding = get_int(t, "encoding", c)? as i32;
    Ok(s)
}

fn table_event(t: &Table, base: Option<&Event>) -> Res<LuaEvent> {
    let c = "dialogue";
    let mut e = base.cloned().unwrap_or_default();
    e.kind = if get_bool(t, "comment", c)? {
        EventKind::Comment
    } else {
        EventKind::Dialogue
    };
    e.layer = get_int(t, "layer", c)? as i32;
    e.start = Time(get_int(t, "start_time", c)?.clamp(0, 35_999_994));
    e.end = Time(get_int(t, "end_time", c)?.clamp(0, 35_999_994));
    e.style = get_str(t, "style", c)?;
    e.actor = get_str(t, "actor", c)?;
    e.margin_l = get_int(t, "margin_l", c)? as i32;
    e.margin_r = get_int(t, "margin_r", c)? as i32;
    let mv = get_int(t, "margin_t", c)? as i32;
    if mv != e.margin_v {
        e.margin_b = mv;
    }
    e.margin_v = mv;
    e.effect = get_str(t, "effect", c)?;
    e.text = get_str(t, "text", c)?;
    let mut extra = Vec::new();
    match t.raw_get::<Value>("extra").map_err(|e| e.to_string())? {
        Value::Nil => {}
        Value::Table(x) => {
            for pair in x.pairs::<Value, Value>() {
                let (k, v) = pair.map_err(|e| e.to_string())?;
                let Value::String(k) = k else { continue };
                let v = match v {
                    Value::String(s) => s.to_string_lossy(),
                    Value::Integer(i) => i.to_string(),
                    Value::Number(n) => fmt_f64(n),
                    _ => "<not a string>".into(),
                };
                extra.push((k.to_string_lossy(), v));
            }
            extra.sort();
        }
        _ => return Err("dialogue extradata must be a table".into()),
    }
    Ok(LuaEvent { event: e, extra })
}

/// Convert a Lua value to a line; `base` is the line it replaces (if any).
fn value_line(v: &Value, base: Option<&Line>) -> Res<Line> {
    let Value::Table(t) = v else {
        return Err("Can't convert a non-table value to AssEntry".into());
    };
    let class = match t.raw_get::<Value>("class").map_err(|e| e.to_string())? {
        Value::String(s) => s.to_string_lossy().to_lowercase(),
        _ => return Err("Table lacks 'class' field, can't convert to AssEntry".into()),
    };
    match class.as_str() {
        "info" => {
            let key = get_str(t, "key", "info")?;
            let value = get_str(t, "value", "info")?;
            let base = match base {
                Some(Line::Info { base, .. }) => base.clone(),
                _ => None,
            };
            Ok(Line::Info { key, value, base })
        }
        "style" => {
            let (b, cur) = match base {
                Some(Line::Style { base, style }) => (
                    base.clone(),
                    style.clone().or(base.as_ref().map(|b| b.style.clone())),
                ),
                _ => (None, None),
            };
            Ok(Line::Style {
                style: Some(table_style(t, cur.as_ref())?),
                base: b,
            })
        }
        "dialogue" => {
            let (b, cur) = match base {
                Some(Line::Event { base, event }) => (
                    base.clone(),
                    event
                        .as_ref()
                        .map(|e| e.event.clone())
                        .or(base.as_ref().map(|b| b.event.clone())),
                ),
                _ => (None, None),
            };
            Ok(Line::Event {
                event: Some(table_event(t, cur.as_ref())?),
                base: b,
            })
        }
        other => Err(format!("Found line with unknown class: {other}")),
    }
}

// ---- karaoke ----------------------------------------------------------------------

struct KSyl {
    start: i64,
    duration: i64,
    tag: String,
    text: String,
    stripped: String,
}

/// Aegisub `AssKaraoke::ParseSyllables` (no normalisation, no auto-split).
fn karaoke(e: &Event) -> Vec<KSyl> {
    use fs_core::tags::{Block, Line as TagLine};
    let mut syls = Vec::new();
    let mut cur = KSyl {
        start: 0,
        duration: 0,
        tag: "\\k".into(),
        text: String::new(),
        stripped: String::new(),
    };
    for b in &TagLine::parse(&e.text).blocks {
        match b {
            Block::Text(t) => {
                cur.text.push_str(t);
                cur.stripped.push_str(t);
            }
            Block::Comment(c) => {
                cur.text.push('{');
                cur.text.push_str(c);
                cur.text.push('}');
            }
            Block::Drawing { commands, .. } => cur.text.push_str(commands),
            Block::Override { tags, .. } => {
                let mut in_tag = false;
                for t in tags {
                    if t.name.to_ascii_lowercase().starts_with('k') {
                        if in_tag {
                            cur.text.push('}');
                            in_tag = false;
                        }
                        let tag = if t.name == "K" { "kf" } else { t.name.as_str() };
                        if cur.duration > 0 || !cur.stripped.is_empty() {
                            let start = cur.start + cur.duration;
                            syls.push(std::mem::replace(
                                &mut cur,
                                KSyl {
                                    start,
                                    duration: 0,
                                    tag: String::new(),
                                    text: String::new(),
                                    stripped: String::new(),
                                },
                            ));
                        } else {
                            cur.start += cur.duration;
                        }
                        cur.tag = format!("\\{tag}");
                        cur.duration = t.arg_f64(0).unwrap_or(0.0).trunc() as i64 * 10;
                    } else {
                        if !in_tag {
                            cur.text.push('{');
                        }
                        in_tag = true;
                        t.write(&mut cur.text);
                    }
                }
                if in_tag {
                    cur.text.push('}');
                }
            }
        }
    }
    syls.push(cur);
    syls
}

// ---- the Lua handle ------------------------------------------------------------------

/// Shared with the runner, which takes the state when the run ends so a
/// stored subtitles object becomes invalid.
#[derive(Clone)]
pub(crate) struct Handle(pub Arc<Mutex<Option<State>>>);

type Ret = mlua::Result<MultiValue>;

fn ok(lua: &Lua, v: impl mlua::IntoLuaMulti) -> Ret {
    let mut mv = v.into_lua_multi(lua)?;
    mv.push_front(Value::Boolean(true));
    Ok(mv)
}

fn fail(lua: &Lua, msg: impl AsRef<str>) -> Ret {
    (false, msg.as_ref()).into_lua_multi(lua)
}

fn to_index(v: &Value) -> Option<i64> {
    match v {
        Value::Integer(i) => Some(*i),
        Value::Number(n) => Some(n.trunc() as i64),
        Value::String(s) => s
            .to_str()
            .ok()?
            .trim()
            .parse::<f64>()
            .ok()
            .map(|n| n.trunc() as i64),
        _ => None,
    }
}

impl Handle {
    fn with<R>(
        &self,
        lua: &Lua,
        modify: bool,
        f: impl FnOnce(&mut State) -> Res<R>,
    ) -> mlua::Result<Res<R>> {
        let _ = lua;
        let mut g = self.0.lock().unwrap_or_else(|e| e.into_inner());
        let Some(st) = g.as_mut() else {
            return Ok(Err("Subtitles object is no longer valid".into()));
        };
        if modify && !st.can_modify {
            return Ok(Err(
                "Attempt to modify subtitles in read-only feature context.".into(),
            ));
        }
        Ok(f(st))
    }

    fn reply<R: mlua::IntoLuaMulti>(lua: &Lua, r: Res<R>) -> Ret {
        match r {
            Ok(v) => ok(lua, v),
            Err(e) => fail(lua, e),
        }
    }

    fn append_lines(st: &mut State, vals: &[Value]) -> Res<()> {
        for v in vals {
            let line = value_line(v, None)?;
            let class = line.class();
            let at = st
                .lines
                .iter()
                .rposition(|l| l.class() == class)
                .map_or(st.lines.len(), |p| p + 1);
            st.touch(&line);
            st.lines.insert(at, line);
        }
        Ok(())
    }

    fn insert_lines(st: &mut State, at: i64, vals: &[Value]) -> Res<()> {
        if at < 1 || at as usize > st.lines.len() + 1 {
            return Err("bad argument #1 to 'insert' (Out of range line index)".into());
        }
        let at = at as usize - 1;
        let lines = vals
            .iter()
            .map(|v| value_line(v, None))
            .collect::<Res<Vec<_>>>()?;
        lines.iter().for_each(|l| st.touch(l));
        st.lines.splice(at..at, lines);
        Ok(())
    }

    fn delete_lines(st: &mut State, mut ids: Vec<i64>) -> Res<()> {
        let n = st.lines.len() as i64;
        for &i in &ids {
            if i < 1 || i > n {
                return Err(format!(
                    "bad argument to 'delete' (Out of range line index {i})"
                ));
            }
        }
        ids.sort_unstable();
        ids.dedup();
        // one pass: removing line by line moved the rest of the file for every id
        let mut gone = ids.into_iter().map(|i| i as usize - 1).peekable();
        let old = std::mem::take(&mut st.lines);
        st.lines.reserve(old.len());
        for (k, l) in old.into_iter().enumerate() {
            if gone.next_if_eq(&k).is_some() {
                st.touch(&l);
            } else {
                st.lines.push(l);
            }
        }
        Ok(())
    }
}

impl UserData for Handle {
    fn add_methods<M: UserDataMethods<Self>>(m: &mut M) {
        m.add_method("len", |lua, h, ()| {
            let r = h.with(lua, false, |st| Ok(st.lines.len()))?;
            Handle::reply(lua, r)
        });
        m.add_method("get", |lua, h, i: Value| {
            let g = h.0.lock().unwrap_or_else(|e| e.into_inner());
            let Some(st) = g.as_ref() else {
                return fail(lua, "Subtitles object is no longer valid");
            };
            let i = to_index(&i).unwrap_or(0);
            if i < 1 || i as usize > st.lines.len() {
                return fail(
                    lua,
                    format!("Requested out-of-range line from subtitle file: {i}"),
                );
            }
            ok(lua, line_table(lua, st, &st.lines[i as usize - 1])?)
        });
        m.add_method("set", |lua, h, (i, v): (Value, Value)| {
            let i = to_index(&i).unwrap_or(0);
            let r = h.with(lua, true, |st| {
                if i == 0 {
                    return Handle::append_lines(st, &[v]);
                }
                if i < 0 {
                    return Handle::insert_lines(st, -i, &[v]);
                }
                if v.is_nil() {
                    return Handle::delete_lines(st, vec![i]);
                }
                if i as usize > st.lines.len() {
                    return Err(format!("Out of range line index {i}"));
                }
                let idx = i as usize - 1;
                let line = value_line(&v, Some(&st.lines[idx]))?;
                let old = st.lines[idx].clone();
                st.touch(&old);
                st.touch(&line);
                st.lines[idx] = line;
                Ok(())
            })?;
            Handle::reply(lua, r)
        });
        m.add_method("delete", |lua, h, args: MultiValue| {
            let mut ids = Vec::new();
            for (k, a) in args.iter().enumerate() {
                match a {
                    Value::Table(t) => {
                        for v in t.sequence_values::<Value>() {
                            ids.push(to_index(&v?).unwrap_or(0));
                        }
                    }
                    v => match to_index(v) {
                        Some(i) => ids.push(i),
                        None => {
                            return fail(
                                lua,
                                format!("bad argument #{} to 'delete' (number expected)", k + 1),
                            );
                        }
                    },
                }
            }
            let r = h.with(lua, true, |st| Handle::delete_lines(st, ids))?;
            Handle::reply(lua, r)
        });
        m.add_method("deleterange", |lua, h, (a, b): (Value, Value)| {
            let (Some(a), Some(b)) = (to_index(&a), to_index(&b)) else {
                return fail(lua, "bad argument to 'deleterange' (number expected)");
            };
            if a < 0 || b < 0 {
                return fail(lua, "bad argument to 'deleterange' (must be >= 0)");
            }
            let r = h.with(lua, true, |st| {
                let a = a.max(1) as usize;
                let b = (b as usize).min(st.lines.len());
                if a <= b {
                    let removed: Vec<Line> = st.lines.drain(a - 1..b).collect();
                    for l in &removed {
                        st.touch(l);
                    }
                }
                Ok(())
            })?;
            Handle::reply(lua, r)
        });
        m.add_method("insert", |lua, h, args: MultiValue| {
            let mut it = args.into_iter();
            let at = it.next().as_ref().and_then(to_index).unwrap_or(0);
            let vals: Vec<Value> = it.collect();
            let r = h.with(lua, true, |st| Handle::insert_lines(st, at, &vals))?;
            Handle::reply(lua, r)
        });
        m.add_method("append", |lua, h, args: MultiValue| {
            let vals: Vec<Value> = args.into_iter().collect();
            let r = h.with(lua, true, |st| Handle::append_lines(st, &vals))?;
            Handle::reply(lua, r)
        });
        m.add_method("script_resolution", |lua, h, ()| {
            let r = h.with(lua, false, |st| {
                let get = |k: &str| {
                    st.lines.iter().find_map(|l| match l {
                        Line::Info { key, value, .. } if key.eq_ignore_ascii_case(k) => {
                            value.trim().parse::<i64>().ok()
                        }
                        _ => None,
                    })
                };
                let (mut x, mut y) = (get("PlayResX").unwrap_or(0), get("PlayResY").unwrap_or(0));
                if x == 0 && y == 0 {
                    (x, y) = (384, 288);
                } else if x == 0 {
                    x = if y == 1024 { 1280 } else { y * 4 / 3 };
                } else if y == 0 {
                    y = if x == 1280 { 1024 } else { x * 3 / 4 };
                }
                Ok((x, y))
            })?;
            Handle::reply(lua, r)
        });
        m.add_method("parse_karaoke", |lua, h, line: Value| {
            let r = h.with(lua, false, |_| match value_line(&line, None)? {
                Line::Event { event: Some(e), .. } => Ok(e.event),
                _ => Err("bad argument #1 to 'parse_karaoke_data' (Subtitle line must be a dialogue line)".into()),
            })?;
            let e = match r {
                Ok(e) => e,
                Err(msg) => return fail(lua, msg),
            };
            let out = lua.create_table()?;
            let syl = |start: i64, dur: i64, tag: &str, text: &str, stripped: &str| -> mlua::Result<Table> {
                let t = lua.create_table()?;
                t.set("duration", dur)?;
                t.set("start_time", start)?;
                t.set("end_time", start + dur)?;
                t.set("tag", tag)?;
                t.set("text", text)?;
                t.set("text_stripped", stripped)?;
                Ok(t)
            };
            out.raw_set(0, syl(0, 0, "", "", "")?)?;
            for (i, s) in karaoke(&e).iter().enumerate() {
                out.raw_set(i + 1, syl(s.start, s.duration, &s.tag, &s.text, &s.stripped)?)?;
            }
            ok(lua, out)
        });
        m.add_method("set_undo_point", |lua, h, desc: Value| {
            let desc = match desc {
                Value::String(s) => s.to_string_lossy(),
                Value::Integer(i) => i.to_string(),
                Value::Number(n) => fmt_f64(n),
                _ => String::new(),
            };
            let mut g = h.0.lock().unwrap_or_else(|e| e.into_inner());
            let Some(st) = g.as_mut() else {
                return fail(lua, "Subtitles object is no longer valid");
            };
            if !st.can_set_undo {
                return fail(
                    lua,
                    "Attempt to set an undo point in a context where it makes no sense to do so.",
                );
            }
            if st.modified {
                let snap = (desc, st.lines.clone(), st.info_touched);
                st.undo_points.push(snap);
                st.modified = false;
            }
            ok(lua, ())
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn colors_and_extradata_codec() {
        assert_eq!(parse_color("&H80FF0000&"), Color::rgba(0, 0, 255, 0x80));
        assert_eq!(parse_color("&H0000FF"), Color::rgb(255, 0, 0));
        assert_eq!(parse_color("#00FF00"), Color::rgb(0, 255, 0));
        assert_eq!(parse_color("rgb(1,2,3)"), Color::rgb(1, 2, 3));
        assert_eq!(parse_color("#00FF0080"), Color::default());
        assert_eq!(parse_color("junk"), Color::default());
        assert_eq!(inline_decode(&inline_encode("a,b:c|#\n")), "a,b:c|#\n");
        assert_eq!(split_extra_ids("{=3=10}x"), (vec![3, 10], "x"));
        assert_eq!(split_extra_ids("{\\b1}x").0, Vec::<u32>::new());
    }

    #[test]
    fn karaoke_syllables_match_aegisub() {
        let e = Event {
            text: r"{\k10}ka{\K20\b1}ra{\kf0}{\k30}oke".into(),
            ..Default::default()
        };
        let s = karaoke(&e);
        let tags: Vec<_> = s
            .iter()
            .map(|s| (s.tag.as_str(), s.start, s.duration, s.text.as_str()))
            .collect();
        assert_eq!(
            tags,
            vec![
                ("\\k", 0, 100, "ka"),
                ("\\kf", 100, 200, "{\\b1}ra"),
                ("\\k", 300, 300, "oke"),
            ]
        );
    }
}
