//! One editor tab: a document with undo history, its file format and the
//! translation-mode row mapping. Grid rows are logical rows: in TL mode an
//! original/translation event pair is one row, otherwise one event.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use encoding_rs::Encoding;
use fs_core::ass::SectionKind;
use fs_core::history::Editor;
use fs_core::karaoke::Karaoke;
use fs_core::model::{AssVersion, EventField, Style};
use fs_core::ops::{self, Field, JoinMode, Rows, ShiftAmount, ShiftWhich, TextMatcher};
use fs_core::tags::{self, Line, Tag};
use fs_core::tlmode::{self, TlRow};
use fs_core::vfr::Framerate;
use fs_core::{Color, Document, Event, EventEntry, EventId, Time};
use fs_formats::{Format, LoadOptions, SaveOptions};

use crate::ffi::{
    ColumnInfo, FileInfo, FilterSpec, FindSpec, PostSpec, Row, ShiftSpec, StyleData, Syllable, Visual,
};

pub struct Session {
    pub(crate) ed: Editor,
    pub(crate) path: Option<PathBuf>,
    format: Format,
    encoding: &'static Encoding,
    bom: bool,
    /// Events as loaded / as last saved, for the grid's number-cell state.
    loaded: HashMap<EventId, Event>,
    saved: HashMap<EventId, Event>,
    rows: Vec<TlRow>,
    tl: bool,
    generation: u64,
    pub(crate) video_size: Option<(i32, i32)>,
    pub(crate) framerate: Option<Framerate>,
    pub(crate) keyframes: Vec<i64>,
}

impl Default for Session {
    fn default() -> Self {
        let mut s = Session {
            ed: Editor::new(Document::new()),
            path: None,
            format: Format::Ass,
            encoding: encoding_rs::UTF_8,
            bom: true,
            loaded: HashMap::new(),
            saved: HashMap::new(),
            rows: Vec::new(),
            tl: false,
            generation: 0,
            video_size: None,
            framerate: None,
            keyframes: Vec::new(),
        };
        // Document::new already holds the one empty line a new tab starts with
        s.ed.mark_saved();
        s.snapshot_loaded();
        s.refresh();
        s
    }
}

fn events_by_id(doc: &Document) -> HashMap<EventId, Event> {
    doc.events()
        .iter()
        .map(|e| (e.id, e.event.clone()))
        .collect()
}

fn field_of(code: u8) -> Field {
    match code {
        1 => Field::Style,
        2 => Field::Actor,
        3 => Field::Effect,
        _ => Field::Text,
    }
}

fn wraps(text: &str) -> String {
    let plain = tags::strip_tags(text);
    plain
        .split("\\N")
        .flat_map(|l| l.split("\\n"))
        .map(|l| l.replace("\\h", " ").chars().count().to_string())
        .collect::<Vec<_>>()
        .join("/")
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

/// `Dialogue: …` line of an event in the default v4+ field order.
fn event_line(ev: &Event) -> String {
    let fields: Vec<String> = EventField::default_format(AssVersion::Ass)
        .iter()
        .map(|f| ev.get_field(f))
        .collect();
    format!("{}: {}", ev.kind.key(), fields.join(","))
}

impl Session {
    fn snapshot_loaded(&mut self) {
        self.loaded = events_by_id(&self.ed.doc);
        self.saved = self.loaded.clone();
    }

    /// Recompute the row mapping after any change.
    pub(crate) fn refresh(&mut self) {
        self.tl = tlmode::is_tl_mode(&self.ed.doc);
        self.rows = tlmode::rows(&self.ed.doc);
        self.generation += 1;
    }

    fn edit<R>(&mut self, desc: &str, f: impl FnOnce(&mut Document) -> R) -> R {
        let r = self.ed.edit(desc, f);
        self.refresh();
        r
    }

    /// `amend` non-empty merges consecutive edits with the same key into one
    /// undo step (typing in the edit box).
    fn edit_amend<R>(&mut self, amend: &str, desc: &str, f: impl FnOnce(&mut Document) -> R) -> R {
        let r = if amend.is_empty() {
            self.ed.edit(desc, f)
        } else {
            self.ed.edit_amend(desc, amend, f)
        };
        self.refresh();
        r
    }

    fn tl_row(&self, i: usize) -> Option<TlRow> {
        self.rows.get(i).copied()
    }

    /// Main event of each row (timing, style, translation).
    pub(crate) fn main_events(&self, rows: &[usize]) -> Vec<usize> {
        rows.iter()
            .filter_map(|&r| self.tl_row(r))
            .map(|r| r.main())
            .collect()
    }

    /// Every event of each row (both halves of TL pairs).
    fn all_events(&self, rows: &[usize]) -> Vec<usize> {
        let mut v: Vec<usize> = rows
            .iter()
            .filter_map(|&r| self.tl_row(r))
            .flat_map(|r| std::iter::once(r.original).chain(r.translation))
            .collect();
        v.sort_unstable();
        v.dedup();
        v
    }

    /// Row containing event `ev`.
    fn row_of_event(&self, ev: usize) -> Option<usize> {
        self.rows
            .iter()
            .position(|r| r.original == ev || r.translation == Some(ev))
    }

    pub(crate) fn rows_of_events(&self, evs: &[usize]) -> Vec<usize> {
        if evs.len() <= 1 {
            return evs.iter().filter_map(|&e| self.row_of_event(e)).collect();
        }
        // event -> row built once; a search per event is O(rows × events)
        let mut row_of = vec![usize::MAX; self.ed.doc.event_count()];
        for (i, r) in self.rows.iter().enumerate() {
            for e in std::iter::once(r.original).chain(r.translation) {
                row_of[e] = i;
            }
        }
        let mut v: Vec<usize> = evs
            .iter()
            .filter_map(|&e| row_of.get(e).copied().filter(|&r| r != usize::MAX))
            .collect();
        v.dedup();
        v
    }

    fn style_or_default(&self, row: usize) -> String {
        self.tl_row(row)
            .or_else(|| self.rows.last().copied())
            .and_then(|r| self.ed.doc.event(r.main()))
            .map(|e| e.style.clone())
            .or_else(|| self.ed.doc.styles().next().map(|s| s.name.clone()))
            .unwrap_or_else(|| "Default".into())
    }

    // ---- file -------------------------------------------------------------

    pub fn open(&mut self, path: &str) -> Result<(), String> {
        self.open_as(path, "")
    }

    /// Open with a forced encoding; an empty or unknown label detects it.
    pub fn open_as(&mut self, path: &str, encoding: &str) -> Result<(), String> {
        let opts = LoadOptions {
            encoding: Encoding::for_label(encoding.as_bytes()),
            ..LoadOptions::default()
        };
        let l = fs_formats::load_file(Path::new(path), &opts).map_err(|e| format!("{path}: {e}"))?;
        self.load(l, Some(PathBuf::from(path)));
        Ok(())
    }

    /// Subtitle stream `index` of a video file, as an unsaved ASS script.
    pub fn open_embedded(&mut self, video: &str, index: i32) -> Result<(), String> {
        let (text, ext) = fs_media::extract_subtitles(Path::new(video), index)
            .map_err(|e| format!("{video}: {e}"))?;
        let mut l = fs_formats::load_bytes(text.as_bytes(), Some(ext), &LoadOptions::default())
            .map_err(|e| format!("{video}: {e}"))?;
        l.format = Format::Ass;
        self.load(l, None);
        Ok(())
    }

    fn load(&mut self, l: fs_formats::Loaded, path: Option<PathBuf>) {
        self.ed = Editor::new(l.doc);
        self.path = path;
        self.format = l.format;
        self.encoding = l.encoding;
        self.bom = l.bom;
        self.snapshot_loaded();
        self.refresh();
    }

    pub fn save(&mut self, path: &str) -> Result<(), String> {
        let p = PathBuf::from(path);
        // the extension of a new name picks the format (Save as…)
        let format = match &self.path {
            Some(old) if *old == p => self.format,
            _ => p
                .extension()
                .and_then(|e| e.to_str())
                .and_then(Format::from_name)
                .unwrap_or(self.format),
        };
        let opts = SaveOptions {
            encoding: self.encoding,
            bom: self.bom,
            fps: self
                .framerate
                .clone()
                .unwrap_or_else(|| Framerate::from_rational(24000, 1001, false).unwrap()),
            ..Default::default()
        };
        fs_formats::save_file(&self.ed.doc, &p, format, &opts)
            .map_err(|e| format!("{path}: {e}"))?;
        self.path = Some(p);
        self.format = format;
        self.ed.mark_saved();
        self.saved = events_by_id(&self.ed.doc);
        self.generation += 1;
        Ok(())
    }

    /// Write a backup copy as ASS (Aegisub's `.AUTOSAVE.ass`); path, format
    /// and the modified state stay as they are.
    pub fn autosave(&self, path: &str) -> Result<(), String> {
        fs_formats::save_file(
            &self.ed.doc,
            Path::new(path),
            Format::Ass,
            &SaveOptions::default(),
        )
        .map_err(|e| format!("{path}: {e}"))
    }

    pub fn save_translation(&self, path: &str) -> Result<(), String> {
        let doc = tlmode::translation_only(&self.ed.doc);
        let opts = SaveOptions {
            encoding: self.encoding,
            bom: self.bom,
            ..Default::default()
        };
        fs_formats::save_file(&doc, Path::new(path), Format::Ass, &opts)
            .map_err(|e| format!("{path}: {e}"))
    }

    /// Switch the format the file is saved in (Kainote "Conversion"). Text
    /// formats lose styling on save; the document itself stays ASS.
    pub fn convert(&mut self, format: &str) -> Result<(), String> {
        let f = Format::from_name(format).ok_or_else(|| format!("unknown format {format}"))?;
        self.format = f;
        if let Some(p) = &mut self.path {
            p.set_extension(f.extension());
        }
        self.generation += 1;
        Ok(())
    }

    pub fn file_info(&self) -> FileInfo {
        let (x, y) = self.ed.doc.play_res();
        let info = |k: &str| self.ed.doc.info(k).unwrap_or("").to_string();
        FileInfo {
            path: self
                .path
                .as_ref()
                .map(|p| p.to_string_lossy().into_owned())
                .unwrap_or_default(),
            format: self.format.name().into(),
            encoding: self.encoding.name().into(),
            tl_mode: self.tl,
            modified: self.ed.is_modified(),
            play_res_x: x,
            play_res_y: y,
            video_file: info("Video File"),
            audio_file: info("Audio File"),
            keyframes_file: info("Keyframes File"),
        }
    }

    /// Project data (opened video/audio paths) like Aegisub's Project Garbage:
    /// saved with the file but neither an undo step nor a reason to ask "save changes?".
    // shortcut: undoing past this point restores the old value with the document snapshot; harmless for paths.
    /// Project values (video, audio, keyframes, active line) live in Aegisub's
    /// `[Aegisub Project Garbage]` when the file has one, else in `[Script Info]` like Kainote.
    pub fn project_value(&self, key: &str) -> String {
        let d = &self.ed.doc;
        d.kv_get(SectionKind::ProjectGarbage, key)
            .or_else(|| d.info(key))
            .unwrap_or("")
            .to_string()
    }

    pub fn set_project_value(&mut self, key: &str, value: &str) {
        let d = &mut self.ed.doc;
        let kind = if d.kv_section(SectionKind::ProjectGarbage).is_some() {
            SectionKind::ProjectGarbage
        } else {
            SectionKind::ScriptInfo
        };
        if d.kv_get(kind, key).unwrap_or("") == value {
        } else if value.is_empty() {
            d.kv_remove(kind, key);
        } else {
            d.kv_set(kind, key, value);
        }
    }

    pub fn set_info_value(&mut self, key: &str, value: &str) {
        if self.info_value(key) == value {
            return;
        }
        let (k, v) = (key.to_string(), value.to_string());
        self.edit(&format!("Set {key}"), move |d| {
            if v.is_empty() {
                d.remove_info(&k);
            } else {
                d.set_info(&k, &v);
            }
        });
    }

    pub fn info_value(&self, key: &str) -> String {
        self.ed.doc.info(key).unwrap_or("").to_string()
    }

    /// Changes whenever the document or its saved state changes.
    pub fn generation(&self) -> u64 {
        self.generation
    }

    // ---- rows -------------------------------------------------------------

    pub fn row_count(&self) -> usize {
        self.rows.len()
    }

    pub fn column_info(&self) -> ColumnInfo {
        let mut ci = ColumnInfo::default();
        let (mut styles, mut actors, mut effects) = (HashSet::new(), HashSet::new(), HashSet::new());
        let events = self.ed.doc.events();
        let mut last = ("", "", "");
        for r in &self.rows {
            let ev = &events[r.main()].event;
            ci.any_layer |= ev.layer != 0;
            ci.any_margin_l |= ev.margin_l != 0;
            ci.any_margin_r |= ev.margin_r != 0;
            ci.any_margin_v |= ev.margin_v != 0;
            let cur = (
                ev.style.as_str(),
                tlmode::split_actor(&ev.actor).1,
                tlmode::strip_doubtful(&ev.effect),
            );
            // neighbouring lines mostly repeat these: hash only on a change
            if cur.0 != last.0 || styles.is_empty() {
                styles.insert(cur.0);
            }
            if cur.1 != last.1 {
                actors.insert(cur.1);
            }
            if cur.2 != last.2 {
                effects.insert(cur.2);
            }
            last = cur;
        }
        let owned = |set: HashSet<&str>| set.into_iter().filter(|s| !s.is_empty()).map(String::from).collect();
        ci.styles = styles.into_iter().map(String::from).collect();
        (ci.actors, ci.effects) = (owned(actors), owned(effects));
        ci
    }

    pub fn row(&self, i: usize) -> Row {
        let Some(r) = self.tl_row(i) else {
            return Row::default();
        };
        let doc = &self.ed.doc;
        let entry: &Arc<EventEntry> = &doc.events()[r.main()];
        let ev = &entry.event;
        let (markers, actor) = tlmode::split_actor(&ev.actor);
        let state = if self.loaded.get(&entry.id) == Some(ev) {
            0
        } else if self.saved.get(&entry.id) == Some(ev) {
            2
        } else {
            1
        };
        let (text, original) = if self.tl {
            (
                tlmode::translation_text(doc, r).to_string(),
                tlmode::original_text(doc, r).to_string(),
            )
        } else {
            (ev.text.clone(), String::new())
        };
        let shown = if self.tl && r.translation.is_none() {
            &original
        } else {
            &text
        };
        Row {
            start: ev.start.ms(),
            end: ev.end.ms(),
            layer: ev.layer,
            style: ev.style.clone(),
            actor: actor.to_string(),
            effect: tlmode::strip_doubtful(&ev.effect).to_string(),
            margin_l: ev.margin_l,
            margin_r: ev.margin_r,
            margin_v: ev.margin_v,
            cps: tags::cps(shown, ev.duration().ms()),
            wraps: wraps(shown),
            text,
            original,
            comment: ev.is_comment(),
            doubtful: tlmode::is_doubtful(&ev.effect),
            translated: r.translation.is_some(),
            state,
            markers: markers.concat(),
        }
    }

    /// Write every field of a row back (edit box commit).
    pub fn set_row(&mut self, i: usize, row: &Row, amend: &str) {
        let Some(r) = self.tl_row(i) else { return };
        let tl = self.tl;
        let row = row.clone();
        self.edit_amend(amend, "Edit line", move |d| {
            let main = r.main();
            let r = if tl && r.translation.is_none() && !row.text.is_empty() {
                tlmode::set_translation(d, r, &row.text)
            } else {
                r
            };
            let mut ev = d.event(r.main()).unwrap().clone();
            ev.start = Time(row.start);
            ev.end = Time(row.end);
            ev.layer = row.layer;
            ev.style = row.style.clone();
            let prefixes: Vec<&str> = tlmode::split_actor(&row.markers).0;
            ev.actor = tlmode::join_actor(&prefixes, &row.actor);
            ev.margin_l = row.margin_l;
            ev.margin_r = row.margin_r;
            ev.margin_v = row.margin_v;
            ev.effect = row.effect.clone();
            if row.doubtful && r.translation.is_some() {
                ev.effect.push_str(tlmode::DOUBTFUL_MARK);
            }
            ev.kind = if row.comment {
                fs_core::model::EventKind::Comment
            } else {
                fs_core::model::EventKind::Dialogue
            };
            if !tl || r.translation.is_some() {
                ev.text = row.text.clone();
            }
            if *d.event(r.main()).unwrap() != ev {
                *d.event_mut(r.main()).unwrap() = ev.clone();
            }
            if tl {
                // the original mirrors the line's fields but keeps the TL style
                let o = r.original;
                let mut orig = d.event(o).unwrap().clone();
                if r.translation.is_some() {
                    let style = orig.style.clone();
                    orig = Event {
                        text: row.original.clone(),
                        style,
                        effect: tlmode::strip_doubtful(&ev.effect).to_string(),
                        ..ev.clone()
                    };
                } else if o == main {
                    orig.text = row.original.clone();
                }
                if *d.event(o).unwrap() != orig {
                    *d.event_mut(o).unwrap() = orig;
                }
            }
        });
    }

    pub fn set_texts(&mut self, rows: &[usize], texts: &[String], description: &str) {
        let evs: Vec<(usize, String)> = rows
            .iter()
            .zip(texts)
            .filter_map(|(&r, t)| Some((*self.main_events(&[r]).first()?, t.clone())))
            .collect();
        if evs.is_empty() {
            return;
        }
        self.edit(description, move |d| {
            for (e, t) in evs {
                d.event_mut(e).unwrap().text = t;
            }
        });
    }

    pub fn set_rows_field(&mut self, rows: &[usize], field: &str, value: &str) {
        let evs = if field == "style" || field == "effect" {
            self.main_events(rows)
        } else {
            self.all_events(rows)
        };
        let (field, value) = (field.to_string(), value.to_string());
        self.edit(&format!("Set {field}"), move |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                let num = || value.trim().parse::<i32>().unwrap_or(0);
                match field.as_str() {
                    "style" => ev.style = value.clone(),
                    "actor" => {
                        let (p, _) = tlmode::split_actor(&ev.actor);
                        ev.actor = tlmode::join_actor(&p, &value);
                    }
                    "effect" => ev.effect = value.clone(),
                    "layer" => ev.layer = num(),
                    "margin_l" => ev.margin_l = num(),
                    "margin_r" => ev.margin_r = num(),
                    "margin_v" => ev.margin_v = num(),
                    "comment" => {
                        ev.kind = if value == "1" {
                            fs_core::model::EventKind::Comment
                        } else {
                            fs_core::model::EventKind::Dialogue
                        }
                    }
                    "start" => ev.start = Time(value.parse().unwrap_or(0)),
                    "end" => ev.end = Time(value.parse().unwrap_or(0)),
                    // "start,end" in one undo step (snap to scene)
                    "times" => {
                        if let Some((s, e)) = value.split_once(',') {
                            ev.start = Time(s.trim().parse().unwrap_or(0));
                            ev.end = Time(e.trim().parse().unwrap_or(0));
                        }
                    }
                    "markers" => {
                        let (_, a) = tlmode::split_actor(&ev.actor);
                        let a = a.to_string();
                        let p: Vec<&str> = tlmode::split_actor(&value).0;
                        ev.actor = tlmode::join_actor(&p, &a);
                    }
                    _ => {}
                }
            }
        });
    }

    /// Insert an empty line before row `at` (`at == row_count` appends).
    pub fn insert_row(&mut self, at: usize, start: i64, end: i64) -> usize {
        let style = self.style_or_default(at.saturating_sub(1));
        let ev_at = self
            .tl_row(at)
            .map(|r| r.original)
            .unwrap_or(self.ed.doc.event_count());
        self.edit("Insert line", |d| {
            d.insert_event(ev_at, Event::dialogue(Time(start), Time(end), &style, ""));
        });
        self.row_of_event(ev_at).unwrap_or(0)
    }

    pub fn duplicate_rows(&mut self, rows: &[usize]) -> Vec<usize> {
        let evs = self.all_events(rows);
        let new = self.edit("Duplicate lines", |d| ops::duplicate(d, &evs));
        self.rows_of_events(&new)
    }

    pub fn delete_rows(&mut self, rows: &[usize]) {
        let evs = self.all_events(rows);
        self.edit("Delete lines", |d| ops::delete_rows(d, &evs));
    }

    pub fn clear_text(&mut self, rows: &[usize]) {
        let evs = self.main_events(rows);
        self.edit("Delete text", |d| {
            for e in evs {
                d.event_mut(e).unwrap().text.clear();
            }
        });
    }

    /// Retime the whole script between frame rates (Aegisub "Transform
    /// Framerate"); `video` as a rate stands for the video's timecodes.
    pub fn change_fps(&mut self, from: &str, to: &str, video: &[i64]) -> Result<(), String> {
        let rate = |s: &str| {
            if s == "video" {
                Framerate::from_timecodes(video.to_vec())
            } else {
                Framerate::parse(s)
            }
            .map_err(|e| format!("{s}: {e}"))
        };
        let (from, to) = (rate(from)?, rate(to)?);
        self.edit("Change frame rate", |d| ops::change_fps(d, &from, &to));
        Ok(())
    }

    /// Aegisub's Export: a copy through the Fix Styles / Transform Framerate
    /// filters (empty `fps_to` skips it), in the path's format and `encoding`.
    /// The open file, its path and saved state stay as they are.
    #[allow(clippy::too_many_arguments)]
    pub fn export_copy(
        &self,
        path: &str,
        fix_styles: bool,
        fps_from: &str,
        fps_to: &str,
        video: &[i64],
        encoding: &str,
    ) -> Result<(), String> {
        self.export_doc(
            self.ed.doc.clone(),
            path,
            fix_styles,
            fps_from,
            fps_to,
            video,
            encoding,
        )
    }

    /// `export_copy` of an already transformed document (automation export filters).
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn export_doc(
        &self,
        mut doc: Document,
        path: &str,
        fix_styles: bool,
        fps_from: &str,
        fps_to: &str,
        video: &[i64],
        encoding: &str,
    ) -> Result<(), String> {
        if fix_styles {
            ops::fix_styles(&mut doc);
        }
        if !fps_to.is_empty() {
            let rate = |s: &str| {
                if s == "video" {
                    Framerate::from_timecodes(video.to_vec())
                } else {
                    Framerate::parse(s)
                }
                .map_err(|e| format!("{s}: {e}"))
            };
            ops::change_fps(&mut doc, &rate(fps_from)?, &rate(fps_to)?);
        }
        let p = Path::new(path);
        let format = p
            .extension()
            .and_then(|e| e.to_str())
            .and_then(Format::from_name)
            .unwrap_or(Format::Ass);
        let opts = SaveOptions {
            encoding: Encoding::for_label(encoding.as_bytes()).unwrap_or(self.encoding),
            bom: self.bom,
            fps: self
                .framerate
                .clone()
                .unwrap_or_else(|| Framerate::from_rational(24000, 1001, false).unwrap()),
            ..Default::default()
        };
        fs_formats::save_file(&doc, p, format, &opts).map_err(|e| format!("{path}: {e}"))
    }

    /// Aegisub "Recombine Lines"; returns the rows left of the selection.
    pub fn recombine_rows(&mut self, rows: &[usize]) -> Vec<usize> {
        // shortcut: not in TL mode (pairs would need recombining together); add when asked.
        if self.tl {
            return rows.to_vec();
        }
        let evs = self.main_events(rows);
        let left = self.edit("Recombine lines", |d| ops::recombine(d, &evs));
        self.rows_of_events(&left)
    }

    /// Aegisub "Split Lines (by karaoke)"; returns the new rows.
    pub fn split_by_karaoke(&mut self, rows: &[usize]) -> Vec<usize> {
        if self.tl {
            return Vec::new();
        }
        let evs = self.main_events(rows);
        let new = self.edit("Split by karaoke", |d| ops::split_by_karaoke(d, &evs));
        self.rows_of_events(&new)
    }

    /// 0 join, 1 keep first, 2 keep last, 3 as karaoke. Not in TL mode.
    pub fn join_rows(&mut self, rows: &[usize], mode: u8) -> i64 {
        // shortcut: TL pairs are not joined (both halves would need merging); add when asked.
        if self.tl || rows.len() < 2 {
            return -1;
        }
        let evs = self.main_events(rows);
        let last_text = evs
            .iter()
            .max()
            .and_then(|&e| self.ed.doc.event(e))
            .map(|e| e.text.clone())
            .unwrap_or_default();
        let jm = match mode {
            1 | 2 => JoinMode::KeepFirst,
            3 => JoinMode::AsKaraoke,
            _ => JoinMode::Concatenate { newline: false },
        };
        let r = self.edit("Join lines", |d| {
            let r = ops::join(d, &evs, jm);
            if mode == 2
                && let Some(i) = r
            {
                d.event_mut(i).unwrap().text = last_text;
            }
            r
        });
        r.and_then(|e| self.row_of_event(e))
            .map_or(-1, |r| r as i64)
    }

    /// Split at a byte position of the text; `at_ms < 0` estimates the time.
    pub fn split_row(&mut self, row: usize, byte_pos: usize, at_ms: i64) -> i64 {
        // shortcut: TL pairs are not split; add when asked.
        if self.tl {
            return -1;
        }
        let Some(r) = self.tl_row(row) else { return -1 };
        let at = (at_ms >= 0).then_some(Time(at_ms));
        let n = self.edit("Split line", |d| {
            let (start, end) = d.event(r.main()).map(|e| (e.start, e.end))?;
            let n = ops::split_at(d, r.main(), byte_pos, at)?;
            if at_ms == -2 {
                // Aegisub's "preserve times": both parts keep the line's times
                d.event_mut(r.main())?.end = end;
                d.event_mut(n)?.start = start;
            }
            Some(n)
        });
        n.and_then(|e| self.row_of_event(e))
            .map_or(-1, |r| r as i64)
    }

    pub fn swap_rows(&mut self, a: usize, b: usize) {
        let (Some(ra), Some(rb)) = (self.tl_row(a), self.tl_row(b)) else {
            return;
        };
        self.edit("Swap lines", |d| ops::swap_rows(d, ra.main(), rb.main()));
    }

    pub fn explode_row(
        &mut self,
        row: usize,
        texts: &[String],
        starts: &[i64],
        ends: &[i64],
    ) -> Vec<usize> {
        let n = texts.len().min(starts.len()).min(ends.len());
        let Some(r) = self.tl_row(row).filter(|_| n > 0 && !self.tl) else {
            return Vec::new();
        };
        let e = r.main();
        let base = self.ed.doc.event(e).unwrap().clone();
        self.edit("Split line", |d| {
            d.remove_event(e);
            for k in 0..n {
                let mut ev = base.clone();
                ev.text = texts[k].clone();
                ev.start = Time(starts[k]);
                ev.end = Time(ends[k]);
                d.insert_event(e + k, ev);
            }
        });
        self.rows_of_events(&(e..e + n).collect::<Vec<_>>())
    }

    /// Entries grouped per row, so TL pairs move together.
    fn groups(&self) -> Vec<Vec<Arc<EventEntry>>> {
        let ev = self.ed.doc.events();
        self.rows
            .iter()
            .map(|r| {
                std::iter::once(r.original)
                    .chain(r.translation)
                    .map(|i| ev[i].clone())
                    .collect()
            })
            .collect()
    }

    fn set_groups(&mut self, desc: &str, groups: Vec<Vec<Arc<EventEntry>>>) {
        self.edit(desc, move |d| {
            *d.event_entries_mut() = groups.into_iter().flatten().collect();
        });
    }

    /// Move rows up (`delta < 0`) or down (Alt+Up/Down, Alt+drag).
    pub fn move_rows(&mut self, rows: &[usize], delta: i64) -> Vec<usize> {
        let mut sel: Vec<usize> = rows
            .iter()
            .copied()
            .filter(|&r| r < self.rows.len())
            .collect();
        sel.sort_unstable();
        sel.dedup();
        let n = self.rows.len() as i64;
        if sel.is_empty() || delta == 0 {
            return sel;
        }
        let delta = delta.clamp(-(sel[0] as i64), n - 1 - *sel.last().unwrap() as i64);
        let mut groups: Vec<Option<Vec<Arc<EventEntry>>>> =
            self.groups().into_iter().map(Some).collect();
        let moving: Vec<Vec<Arc<EventEntry>>> =
            sel.iter().map(|&r| groups[r].take().unwrap()).collect();
        let rest: Vec<Vec<Arc<EventEntry>>> = groups.into_iter().flatten().collect();
        let first = (sel[0] as i64 + delta) as usize;
        let mut out = rest;
        let new: Vec<usize> = (first..first + moving.len()).collect();
        out.splice(first..first, moving);
        self.set_groups("Move lines", out);
        new
    }

    pub fn make_continuous(&mut self, rows: &[usize], next: bool) {
        let evs = self.main_events(rows);
        self.edit("Continuous times", |d| ops::make_continuous(d, &evs, next));
    }

    /// Sort by 0 start, 1 end, 2 style, 3 actor, 4 effect, 5 layer, 6 text.
    pub fn sort_rows(&mut self, key: u8, selected_only: bool, rows: &[usize]) {
        let doc = &self.ed.doc;
        let main = |g: &Vec<Arc<EventEntry>>| g.last().unwrap().event.clone();
        let cmp = |a: &Event, b: &Event| match key {
            1 => a.end.cmp(&b.end),
            2 => a.style.cmp(&b.style),
            3 => a.actor.cmp(&b.actor),
            4 => a.effect.cmp(&b.effect),
            5 => a.layer.cmp(&b.layer),
            6 => a.text.cmp(&b.text),
            _ => a.start.cmp(&b.start),
        };
        let _ = doc;
        let mut groups = self.groups();
        if selected_only {
            let mut sel: Vec<usize> = rows.iter().copied().filter(|&r| r < groups.len()).collect();
            sel.sort_unstable();
            sel.dedup();
            let mut items: Vec<_> = sel.iter().map(|&r| groups[r].clone()).collect();
            items.sort_by(|a, b| cmp(&main(a), &main(b)));
            for (r, g) in sel.into_iter().zip(items) {
                groups[r] = g;
            }
        } else {
            groups.sort_by(|a, b| cmp(&main(a), &main(b)));
        }
        self.set_groups("Sort lines", groups);
    }

    /// Rows picked by the Shift Times "Which lines" choice (0 all, 1 selected,
    /// 2 from the first selected, 3/4 starting at or after/before the active
    /// line, 5 the given styles).
    pub fn spec_rows(
        &self,
        lines: u8,
        selected: &[usize],
        active: usize,
        styles: &[String],
    ) -> Vec<usize> {
        let n = self.rows.len();
        let start_of = |r: usize| self.row(r).start;
        match lines {
            1 => selected.to_vec(),
            2 => (selected.iter().copied().min().unwrap_or(0)..n).collect(),
            3 => {
                let t = start_of(active);
                (0..n).filter(|&r| start_of(r) >= t).collect()
            }
            4 => {
                let t = start_of(active);
                (0..n).filter(|&r| start_of(r) <= t).collect()
            }
            5 => (0..n)
                .filter(|&r| styles.contains(&self.row(r).style))
                .collect(),
            _ => (0..n).collect(),
        }
    }

    /// Shift Times panel. `timecodes` (from the video) are needed for frames.
    pub fn shift(&mut self, spec: &ShiftSpec, timecodes: &[i64]) -> usize {
        let rows = self.spec_rows(spec.lines, &spec.selected, spec.active, &spec.styles);
        let evs = self.all_events(&rows);
        let which = match spec.which {
            1 => ShiftWhich::Start,
            2 => ShiftWhich::End,
            _ => ShiftWhich::Both,
        };
        let fr = if spec.frames {
            match Framerate::from_timecodes(timecodes.to_vec())
                .ok()
                .or_else(|| self.framerate.clone())
            {
                Some(f) => Some(f),
                None => return 0,
            }
        } else {
            None
        };
        let spec = spec.clone();
        self.edit("Shift times", move |d| {
            let before: Vec<i64> = evs
                .iter()
                .map(|&e| d.event(e).unwrap().start.ms())
                .collect();
            let amount = match &fr {
                Some(f) => ShiftAmount::Frames(spec.amount, f),
                None => ShiftAmount::Time(Time(spec.amount)),
            };
            let n = ops::shift_times(d, &Rows::Selected(evs.clone()), amount, which, None);
            if spec.tag_times && which == ShiftWhich::Start {
                // shortcut: only \move and \t times follow a start-only shift; \fade is left alone.
                for (k, &e) in evs.iter().enumerate() {
                    let delta = d.event(e).unwrap().start.ms() - before[k];
                    shift_tag_times(d.event_mut(e).unwrap(), -delta);
                }
            }
            match spec.end_fix {
                1 => fix_overlaps(d, &evs),
                2 => {
                    // shortcut: Kainote's "new times" heuristic approximated as 0.5 s + 50 ms per character.
                    for &e in &evs {
                        let ev = d.event_mut(e).unwrap();
                        let chars = tags::char_count(&ev.text, true, false) as i64;
                        ev.end = ev.start + Time((500 + 50 * chars).max(1000));
                    }
                }
                _ => {}
            }
            n
        })
    }

    pub fn post_process(&mut self, spec: &PostSpec, keyframes: &[i64], timecodes: &[i64]) {
        let evs = self.all_events(&spec.rows);
        let fr = Framerate::from_timecodes(timecodes.to_vec())
            .ok()
            .or_else(|| self.framerate.clone());
        let kf = keyframes.to_vec();
        let spec = spec.clone();
        self.edit("Post-process timing", move |d| {
            let opt = |v: i64| (v > 0).then_some(Time(v));
            let pp = ops::PostProcess {
                lead_in: opt(spec.lead_in),
                lead_out: opt(spec.lead_out),
                adjacent_threshold: opt(spec.adjacent),
                adjacent_bias: spec.bias,
                keyframes: None,
                kf_before_start: Time(spec.kf_before_start),
                kf_after_start: Time(spec.kf_after_start),
                kf_before_end: Time(spec.kf_before_end),
                kf_after_end: Time(spec.kf_after_end),
                styles: Vec::new(),
            };
            ops::post_process(d, &Rows::Selected(evs.clone()), &pp);
            // Kainote's second threshold: gaps still open move the next start back
            // to the previous end; keyframes snap last
            let pp = ops::PostProcess {
                lead_in: None,
                lead_out: None,
                adjacent_threshold: opt(spec.adjacent_start),
                adjacent_bias: 0.0,
                keyframes: match (&fr, spec.snap_keyframes && !kf.is_empty()) {
                    (Some(f), true) => Some((kf.as_slice(), f)),
                    _ => None,
                },
                ..pp
            };
            ops::post_process(d, &Rows::Selected(evs), &pp);
        });
    }

    /// Rows that stay visible under the filter.
    pub fn filter(&self, spec: &FilterSpec) -> Vec<usize> {
        (0..self.rows.len())
            .filter(|&i| {
                let r = self.row(i);
                let hidden = spec.hide_styles.contains(&r.style)
                    || spec.hide_rows.contains(&i)
                    || (spec.hide_comments && r.comment)
                    || (spec.only_doubtful && !r.doubtful)
                    || (spec.only_untranslated && r.translated && !r.text.trim().is_empty());
                hidden == spec.invert
            })
            .collect()
    }

    fn matcher(spec: &FindSpec) -> Result<TextMatcher, String> {
        let mut m = TextMatcher::new(
            &spec.pattern,
            spec.regex,
            spec.match_case,
            field_of(spec.field),
        )
        .map_err(|e| e.to_string())?;
        m.strip_tags = spec.skip_tags;
        Ok(m)
    }

    /// Next row after `from` (wrapping) whose field matches; -1 if none.
    pub fn find(&self, spec: &FindSpec, from: usize) -> Result<i64, String> {
        let m = Self::matcher(spec)?;
        let n = self.rows.len();
        let doc = &self.ed.doc;
        for k in 1..=n {
            let r = (from + k) % n;
            let row = self.rows[r];
            let hit = std::iter::once(row.main())
                .chain(self.tl.then_some(row.original))
                .filter_map(|e| doc.event(e))
                .any(|ev| (spec.include_comments || !ev.is_comment()) && m.is_match(ev));
            if hit && (spec.rows.is_empty() || spec.rows.contains(&r)) {
                return Ok(r as i64);
            }
        }
        Ok(-1)
    }

    pub fn replace_all(&mut self, spec: &FindSpec) -> Result<usize, String> {
        let m = Self::matcher(spec)?;
        let rows: Vec<usize> = if spec.rows.is_empty() {
            (0..self.rows.len()).collect()
        } else {
            spec.rows.clone()
        };
        let doc = &self.ed.doc;
        let evs: Vec<usize> = self
            .main_events(&rows)
            .into_iter()
            .filter(|&e| spec.include_comments || !doc.event(e).unwrap().is_comment())
            .collect();
        let r = ops::Replace {
            matcher: m,
            replacement: spec.replacement.clone(),
            first_only: false,
        };
        Ok(self.edit("Replace", |d| ops::replace(d, &Rows::Selected(evs), &r)))
    }

    /// Rows overlapping `row` in time (collision colouring).
    pub fn overlapping(&self, row: usize) -> Vec<usize> {
        let Some(r) = self.tl_row(row) else {
            return Vec::new();
        };
        let me = self.ed.doc.event(r.main()).unwrap();
        if me.is_comment() {
            return Vec::new();
        }
        (0..self.rows.len())
            .filter(|&i| i != row)
            .filter(|&i| {
                let e = self.ed.doc.event(self.rows[i].main()).unwrap();
                !e.is_comment() && e.start < me.end && me.start < e.end
            })
            .collect()
    }

    /// Rows visible at `ms` (grid tint, F2 select from video time).
    pub fn rows_at(&self, ms: i64) -> Vec<usize> {
        (0..self.rows.len())
            .filter(|&i| {
                let e = self.ed.doc.event(self.rows[i].main()).unwrap();
                !e.is_comment() && e.start.ms() <= ms && ms < e.end.ms()
            })
            .collect()
    }

    /// Font collector report (one line per face). Fonts attached to `media`
    /// are unpacked to a temp folder and searched too; a non-empty `dest`
    /// receives a copy of every found file.
    pub fn collect_fonts(
        &self,
        dirs: Vec<String>,
        media: &str,
        dest: &str,
    ) -> Result<String, String> {
        use std::fmt::Write;
        let mut db = fontdb::Database::new();
        db.load_system_fonts();
        for d in &dirs {
            db.load_fonts_dir(d);
        }
        if !media.is_empty() {
            let tmp = std::env::temp_dir().join("fs-editor-attached-fonts");
            let _ = std::fs::remove_dir_all(&tmp);
            std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
            for (name, data) in
                fs_media::attached_fonts(std::path::Path::new(media)).unwrap_or_default()
            {
                // attachment names come from the file: keep only the last path component
                let name = std::path::Path::new(&name)
                    .file_name()
                    .map(|n| n.to_owned())
                    .unwrap_or_default();
                if !name.is_empty() {
                    std::fs::write(tmp.join(name), data).map_err(|e| e.to_string())?;
                }
            }
            db.load_fonts_dir(&tmp);
        }
        if !dest.is_empty() {
            std::fs::create_dir_all(dest).map_err(|e| format!("{dest}: {e}"))?;
        }
        let mut out = String::new();
        for (f, found) in fs_core::fonts::locate(&db, &fs_core::fonts::used_fonts(&self.ed.doc)) {
            let face = format!(
                "{} ({}{})",
                f.family,
                f.weight,
                if f.italic { ", italic" } else { "" }
            );
            let Some(loc) = found else {
                let _ = writeln!(out, "MISSING  {face}");
                continue;
            };
            let tag = if loc.synthetic { "FAUX   " } else { "OK     " };
            let _ = write!(out, "{tag}  {face}: {}", loc.path.display());
            if !loc.missing_glyphs.is_empty() {
                let _ = write!(
                    out,
                    "  missing glyphs: {}",
                    loc.missing_glyphs.iter().collect::<String>()
                );
            }
            out.push('\n');
            if let (false, Some(name)) = (dest.is_empty(), loc.path.file_name()) {
                std::fs::copy(&loc.path, std::path::Path::new(dest).join(name))
                    .map_err(|e| format!("{}: {e}", loc.path.display()))?;
            }
        }
        Ok(out)
    }

    pub fn resample(&mut self, x: i32, y: i32) {
        let from = self.ed.doc.play_res();
        self.edit("Resample resolution", |d| {
            ops::resample(
                d,
                &ops::Resample {
                    from,
                    to: (x, y),
                    stretch: false,
                    margin: (0, 0, 0, 0),
                },
            );
            d.set_info("PlayResX", &x.to_string());
        });
    }

    /// Selected events as `Dialogue:` lines (clipboard).
    pub fn copy_rows(&self, rows: &[usize]) -> String {
        self.all_events(rows)
            .iter()
            .filter_map(|&e| self.ed.doc.event(e))
            .map(event_line)
            .collect::<Vec<_>>()
            .join("\n")
    }

    /// Paste `Dialogue:` lines (or plain text, one line per row) before row `at`.
    pub fn paste_rows(&mut self, at: usize, text: &str) -> Vec<usize> {
        let fmt = EventField::default_format(AssVersion::Ass)
            .iter()
            .map(|f| f.name().to_string())
            .collect::<Vec<_>>()
            .join(", ");
        let parsed = Document::parse(&format!("[Events]\nFormat: {fmt}\n{text}\n"));
        let mut evs: Vec<Event> = (0..parsed.event_count())
            .map(|i| parsed.event(i).unwrap().clone())
            .collect();
        if evs.is_empty() {
            let style = self.style_or_default(at.saturating_sub(1));
            evs = text
                .lines()
                .filter(|l| !l.trim().is_empty())
                .map(|l| Event::dialogue(Time(0), Time(5000), &style, l))
                .collect();
        }
        let ev_at = self
            .tl_row(at)
            .map(|r| r.original)
            .unwrap_or(self.ed.doc.event_count());
        let n = evs.len();
        self.edit("Paste lines", |d| {
            for (k, ev) in evs.into_iter().enumerate() {
                d.insert_event(ev_at + k, ev);
            }
        });
        self.rows_of_events(&(ev_at..ev_at + n).collect::<Vec<_>>())
    }

    /// Aegisub's "Paste lines over": clipboard line k goes onto `rows[k]`,
    /// copying only the `fields` bits (Aegisub's order: 1 comment, 2 layer,
    /// 4 start, 8 end, 16 style, 32 actor, 64/128/256 margins L/R/V,
    /// 512 effect, 1024 text). Returns the rows changed.
    pub fn paste_over(&mut self, rows: &[usize], text: &str, fields: u16) -> Vec<usize> {
        let fmt = EventField::default_format(AssVersion::Ass)
            .iter()
            .map(|f| f.name().to_string())
            .collect::<Vec<_>>()
            .join(", ");
        // a line that is not a Dialogue/Comment line is plain text (Aegisub: End 0)
        let pasted: Vec<Event> = text
            .lines()
            .filter(|l| !l.trim().is_empty())
            .map(|l| {
                let d = Document::parse(&format!("[Events]\nFormat: {fmt}\n{}\n", l.trim()));
                d.event(0)
                    .cloned()
                    .unwrap_or_else(|| Event::dialogue(Time(0), Time(0), "", l.trim()))
            })
            .collect();
        let n = pasted.len().min(rows.len());
        let evs = self.main_events(&rows[..n]);
        self.edit("Paste lines over", |d| {
            for (src, &e) in pasted.iter().zip(&evs) {
                let ev = d.event_mut(e).unwrap();
                let on = |bit: u16| fields & bit != 0;
                if on(1) {
                    ev.kind = src.kind;
                }
                if on(2) {
                    ev.layer = src.layer;
                }
                if on(4) {
                    ev.start = src.start;
                }
                if on(8) {
                    ev.end = src.end;
                }
                if on(16) {
                    ev.style.clone_from(&src.style);
                }
                if on(32) {
                    ev.actor.clone_from(&src.actor);
                }
                if on(64) {
                    ev.margin_l = src.margin_l;
                }
                if on(128) {
                    ev.margin_r = src.margin_r;
                }
                if on(256) {
                    ev.margin_v = src.margin_v;
                }
                if on(512) {
                    ev.effect.clone_from(&src.effect);
                }
                if on(1024) {
                    ev.text.clone_from(&src.text);
                }
            }
        });
        rows[..n].to_vec()
    }

    // ---- history ----------------------------------------------------------

    pub fn undo(&mut self) -> bool {
        let r = self.ed.undo().is_some();
        self.refresh();
        r
    }

    pub fn redo(&mut self) -> bool {
        let r = self.ed.redo().is_some();
        self.refresh();
        r
    }

    pub fn can_undo(&self) -> bool {
        self.ed.can_undo()
    }

    pub fn can_redo(&self) -> bool {
        self.ed.can_redo()
    }

    /// Undo descriptions, most recent first.
    pub fn history(&self) -> Vec<String> {
        self.ed.undo_list().map(str::to_string).collect()
    }

    pub fn undo_to(&mut self, steps: usize) {
        for _ in 0..steps {
            if self.ed.undo().is_none() {
                break;
            }
        }
        self.refresh();
    }

    // ---- styles -----------------------------------------------------------

    pub fn attachments(&self) -> Vec<crate::ffi::AttachmentRow> {
        self.ed
            .doc
            .attachments()
            .into_iter()
            .map(|a| crate::ffi::AttachmentRow {
                font: a.font,
                name: a.name,
                size: a.size,
            })
            .collect()
    }

    pub fn attach_file(&mut self, font: bool, path: &str) -> Result<String, String> {
        let data = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
        let file = Path::new(path)
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        let mut stored = String::new();
        self.edit("Attach file", |d| stored = d.attach(font, &file, &data));
        Ok(stored)
    }

    pub fn extract_attachment(&self, font: bool, name: &str, path: &str) -> Result<(), String> {
        let data = self
            .ed
            .doc
            .attachment_data(font, name)
            .ok_or_else(|| format!("no attachment {name}"))?;
        std::fs::write(path, data).map_err(|e| format!("{path}: {e}"))
    }

    pub fn remove_attachment(&mut self, font: bool, name: &str) {
        let name = name.to_string();
        self.edit("Delete attachment", move |d| {
            d.remove_attachment(font, &name);
        });
    }

    pub fn styles(&self) -> Vec<StyleData> {
        self.ed.doc.styles().map(style_data).collect()
    }

    /// Replace style `index`, or add a new one when `index < 0`. Renaming
    /// updates the lines that used the old name.
    pub fn set_style(&mut self, index: i64, style: &StyleData) {
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
            if old != style.name {
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

    /// Delete styles no line uses; returns their names (no undo step when none).
    pub fn clean_styles(&mut self) -> Vec<String> {
        let mut probe = self.ed.doc.clone();
        if ops::remove_unused_styles(&mut probe).is_empty() {
            return Vec::new();
        }
        self.edit("Delete unused styles", ops::remove_unused_styles)
    }

    pub fn delete_style(&mut self, index: usize) {
        self.edit("Delete style", |d| {
            d.remove_style(index);
        });
    }

    /// The script libass renders: in TL mode with "hide original" only the
    /// translations are shown.
    /// The script for libass. `at_ms >= 0` keeps only the lines shown at that
    /// time: Aegisub sends just those after an edit, and the whole file once
    /// another frame is wanted.
    pub fn script_for_render(&self, hide_original: bool, at_ms: i64) -> String {
        let mut doc = if self.tl && hide_original {
            tlmode::translation_only(&self.ed.doc)
        } else if at_ms >= 0 {
            self.ed.doc.clone() // sections are shared, only the kept lines are copied
        } else {
            return self.ed.doc.to_ass_string();
        };
        if at_ms >= 0 {
            // libass reads centiseconds: keep lines within 10 ms of the frame time
            let (t, slack) = (Time(at_ms), Time(10));
            doc.event_entries_mut().retain(|e| {
                let e = &e.event;
                !e.is_comment() && e.start <= t + slack && e.end + slack > t
            });
        }
        doc.to_ass_string()
    }

    // ---- translation mode -------------------------------------------------

    pub fn tl_enable(&mut self, on: bool) {
        self.edit(
            if on {
                "Translation mode on"
            } else {
                "Translation mode off"
            },
            |d| {
                if on {
                    tlmode::enable(d)
                } else {
                    tlmode::disable(d)
                }
            },
        );
    }

    pub fn set_doubtful(&mut self, rows: &[usize], on: bool) {
        let rows: Vec<usize> = rows.to_vec();
        let tl_rows = self.rows.clone();
        self.edit("Not confirmed", move |d| {
            // setting doubtful may insert an original event, so walk backwards
            for &r in rows.iter().rev() {
                if let Some(&row) = tl_rows.get(r) {
                    tlmode::set_doubtful(d, row, on);
                }
            }
        });
    }

    pub fn next_doubtful(&self, from: usize) -> i64 {
        tlmode::next_doubtful(&self.ed.doc, from).map_or(-1, |r| r as i64)
    }

    pub fn next_untranslated(&self, from: usize) -> i64 {
        tlmode::next_untranslated(&self.ed.doc, from).map_or(-1, |r| r as i64)
    }

    pub fn paste_translation(&mut self, first: usize, text: &str) -> usize {
        let lines: Vec<String> = text.lines().map(str::to_string).collect();
        self.edit("Paste translation", |d| {
            let refs: Vec<&str> = lines.iter().map(String::as_str).collect();
            tlmode::paste_translation(d, first, &refs)
        })
    }

    // ---- visual tools -----------------------------------------------------

    pub fn visual(&self, row: usize) -> Visual {
        let Some(r) = self.tl_row(row) else {
            return Visual::default();
        };
        crate::visual::of_event(&self.ed.doc, self.ed.doc.event(r.main()).unwrap())
    }

    /// Set override tags (`\pos(1,2)\frz30`) on the first block of each row.
    pub fn set_tags(&mut self, rows: &[usize], tags_text: &str, amend: &str) {
        let evs = self.main_events(rows);
        let new = tags::parse_tags(tags_text);
        self.edit_amend(amend, "Visual typesetting", move |d| {
            for e in evs {
                let mut line = Line::parse(&d.event(e).unwrap().text);
                for t in &new {
                    line.set_line_tag(t.clone());
                }
                let text = line.to_text();
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    /// Kainote's all-tags tool: rewrite `tag` in the rows from `base` (their
    /// texts when the drag started). `mode`: 0 add, 1 set, 2 multiply,
    /// 3/4 gradient over characters up/down, 5/6 gradient over lines up/down.
    pub fn adjust_tag(
        &mut self,
        rows: &[usize],
        base: &[String],
        tag: &str,
        mode: u8,
        delta: f64,
        amend: &str,
    ) {
        use fs_core::tag_adjust::{TagAdjust, adjust_tag};
        let mode = match mode {
            1 => TagAdjust::Set,
            2 => TagAdjust::Multiply,
            3 | 4 => TagAdjust::GradientText {
                increasing: mode == 3,
            },
            5 | 6 => TagAdjust::GradientLine {
                increasing: mode == 5,
            },
            _ => TagAdjust::Add,
        };
        let evs = self.main_events(rows);
        let base = base.to_vec();
        let tag = tag.to_string();
        self.edit_amend(amend, "Change tags", move |d| {
            let n = evs.len().min(base.len());
            for (k, &e) in evs.iter().take(n).enumerate() {
                let style = d.style(&d.event(e).unwrap().style).cloned();
                let text = adjust_tag(&base[k], style.as_ref(), &tag, mode, delta, k, n);
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    /// Remove tags by name (`pos,move`).
    pub fn remove_tags(&mut self, rows: &[usize], names: &str, amend: &str) {
        let evs = self.main_events(rows);
        let names: Vec<String> = names.split(',').map(|s| s.trim().to_string()).collect();
        self.edit_amend(amend, "Visual typesetting", move |d| {
            let n: Vec<&str> = names.iter().map(String::as_str).collect();
            for e in evs {
                let mut line = Line::parse(&d.event(e).unwrap().text);
                line.remove_tags(&n);
                let text = line.to_text();
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    /// Replace the line's `\p` drawing (adding `{\p1}` when it has none).
    pub fn set_drawing(&mut self, row: usize, drawing: &str, amend: &str) {
        let Some(r) = self.tl_row(row) else { return };
        let drawing = drawing.to_string();
        self.edit_amend(amend, "Vector drawing", move |d| {
            let e = r.main();
            let mut line = Line::parse(&d.event(e).unwrap().text);
            match line.blocks.iter_mut().find_map(|b| match b {
                tags::Block::Drawing { commands, .. } => Some(commands),
                _ => None,
            }) {
                Some(c) => *c = drawing,
                None => {
                    line.blocks.retain(|b| !matches!(b, tags::Block::Text(_)));
                    line.set_line_tag(Tag::simple("p", "1"));
                    line.blocks.push(tags::Block::Drawing {
                        scale: 1,
                        commands: drawing,
                    });
                }
            }
            d.event_mut(e).unwrap().text = line.to_text();
        });
    }

    /// `what` bits: 1 \pos, 2 \move start, 4 \move end, 8 clips, 16 drawings, 32 \org.
    pub fn shift_positions(&mut self, rows: &[usize], dx: f64, dy: f64, what: u8, amend: &str) {
        let evs = self.main_events(rows);
        let what = ops::MoveTargets {
            pos: what & 1 != 0,
            move_start: what & 2 != 0,
            move_end: what & 4 != 0,
            clip: what & 8 != 0,
            drawing: what & 16 != 0,
            org: what & 32 != 0,
        };
        self.edit_amend(amend, "Shift positions", move |d| {
            ops::translate_positions(d, &Rows::Selected(evs), dx, dy, what)
        });
    }

    // ---- karaoke ----------------------------------------------------------

    fn kara(&self, row: usize) -> Option<(usize, Karaoke)> {
        let e = self.tl_row(row)?.main();
        Some((e, Karaoke::from_event(self.ed.doc.event(e)?)))
    }

    pub fn syllables(&self, row: usize) -> Vec<Syllable> {
        let Some((_, k)) = self.kara(row) else {
            return Vec::new();
        };
        (0..k.syls.len())
            .map(|i| Syllable {
                start: k.syl_start(i).ms(),
                end: k.syl_end(i).ms(),
                text: k.syl_display(i),
            })
            .collect()
    }

    fn kara_edit(&mut self, row: usize, desc: &str, f: impl FnOnce(&mut Karaoke)) {
        let Some((e, mut k)) = self.kara(row) else {
            return;
        };
        f(&mut k);
        self.edit(desc, |d| k.apply(d.event_mut(e).unwrap()));
    }

    pub fn kara_split(&mut self, row: usize, syl: usize, char_pos: usize) {
        self.kara_edit(row, "Karaoke split", |k| k.split(syl, char_pos));
    }

    pub fn kara_join(&mut self, row: usize, syl: usize) {
        self.kara_edit(row, "Karaoke join", |k| k.join(syl));
    }

    /// Move syllable boundary `index` (1..n-1) to `ms`; `shift_rest` moves
    /// the following boundaries by the same amount (right-drag).
    pub fn kara_set_boundary(&mut self, row: usize, index: usize, ms: i64, shift_rest: bool) {
        self.kara_edit(row, "Karaoke timing", |k| {
            let markers = k.markers();
            let old = markers.get(index).copied().unwrap_or(Time(ms));
            let got = k.set_marker(index, Time(ms));
            if shift_rest {
                let delta = got - old;
                for (j, &m) in markers.iter().enumerate().skip(index + 1) {
                    k.set_marker(j, m + delta);
                }
            }
        });
    }

    pub fn set_video_context(&mut self, w: i32, h: i32, timecodes: &[i64], keyframes: &[i64]) {
        self.video_size = (w > 0 && h > 0).then_some((w, h));
        self.framerate = Framerate::from_timecodes(timecodes.to_vec()).ok();
        self.keyframes = keyframes.to_vec();
    }
}

/// Keep `\move`/`\t` absolute timing when only the line start moved by -`delta`.
fn shift_tag_times(ev: &mut Event, delta: i64) {
    let mut line = Line::parse(&ev.text);
    line.for_each_tag_mut(|t| {
        let idx: &[usize] = match t.name.as_str() {
            "move" if t.args.len() >= 6 => &[4, 5],
            "t" if t.args.len() >= 3 => &[0, 1],
            _ => &[],
        };
        if idx.is_empty() {
            return;
        }
        let mut a = t.args.clone();
        for &i in idx {
            if let Some(v) = t.arg_f64(i) {
                a[i] = ((v as i64) + delta).max(0).to_string();
            }
        }
        t.set_args(a);
    });
    ev.text = line.to_text();
}

/// End each line no later than the next line of the same style starts.
fn fix_overlaps(d: &mut Document, evs: &[usize]) {
    for &e in evs {
        let me = d.event(e).unwrap().clone();
        let next = (0..d.event_count())
            .filter_map(|j| d.event(j))
            .filter(|o| o.style == me.style && !o.is_comment() && o.start > me.start)
            .map(|o| o.start)
            .min();
        if let Some(n) = next
            && me.end > n
        {
            d.event_mut(e).unwrap().end = n;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn session(text: &str) -> Session {
        let dir = std::env::temp_dir().join(format!("fsb-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        // one file per call: tests run in parallel and share the fixtures
        static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let p = dir.join(format!(
            "{}.ass",
            N.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::write(&p, text).unwrap();
        let mut s = Session::default();
        s.open(p.to_str().unwrap()).unwrap();
        s
    }

    const ASS: &str = "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,one\nDialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,two\nDialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,three\n";

    #[test]
    fn column_info_lists_distinct_values() {
        let mut s = session(ASS);
        s.set_rows_field(&[1], "actor", "Ann");
        let ci = s.column_info();
        assert_eq!((ci.styles, ci.actors, ci.effects), (vec!["Default".to_string()], vec!["Ann".to_string()], vec![]));
        assert!(!ci.any_layer && !ci.any_margin_v);
    }

    #[test]
    fn render_script_of_one_frame_keeps_its_lines() {
        let s = session(ASS);
        let text = s.script_for_render(false, 3500);
        assert!(text.contains(",,two") && !text.contains(",,one") && !text.contains(",,three"));
        assert!(text.contains("Style: Default"));
        // the end of a line (centisecond-rounded) still counts within 10 ms
        assert!(s.script_for_render(false, 1995).contains(",,one"));
        assert_eq!(s.script_for_render(false, -1), s.ed.doc.to_ass_string());
    }

    #[test]
    fn open_as_forces_the_encoding() {
        let p = std::env::temp_dir().join(format!("fsb-cp1251-{}.srt", std::process::id()));
        let (bytes, _, _) = encoding_rs::WINDOWS_1251.encode("1\n00:00:01,000 --> 00:00:02,000\nПривет\n");
        std::fs::write(&p, &bytes).unwrap();
        let mut s = Session::default();
        s.open_as(p.to_str().unwrap(), "windows-1251").unwrap();
        assert_eq!(s.row(0).text, "Привет");
        std::fs::remove_file(p).ok();
    }

    #[test]
    fn split_row_estimates_or_keeps_times() {
        let mut s = session(ASS);
        assert_eq!(s.split_row(2, 2, -2), 3);
        let (a, b) = (s.row(2), s.row(3));
        assert_eq!((a.text.as_str(), a.start, a.end), ("th", 5000, 6000));
        assert_eq!((b.text.as_str(), b.start, b.end), ("ree", 5000, 6000));
        assert_eq!(s.split_row(0, 1, -1), 1);
        assert_eq!((s.row(0).end, s.row(1).start), (1333, 1333));
    }

    #[test]
    fn export_copy_filters_without_touching_the_file() {
        let s = session(ASS);
        let path = s.file_info().path;
        let out = std::env::temp_dir().join(format!("fsb-export-{}.srt", std::process::id()));
        s.export_copy(out.to_str().unwrap(), true, "23.976", "25", &[], "UTF-8")
            .unwrap();
        let srt = std::fs::read_to_string(&out).unwrap();
        // 1 s at 23.976 fps is 0.959 s at 25 fps
        assert!(srt.contains("00:00:00,95"), "{srt}");
        assert_eq!(s.file_info().path, path);
        assert!(!s.file_info().modified);
        std::fs::remove_file(out).unwrap();
    }

    #[test]
    fn project_values_follow_the_files_section() {
        // Kainote style: [Script Info]
        let mut s = session(ASS);
        s.set_project_value("Video File", "a.mkv");
        assert_eq!(s.info_value("Video File"), "a.mkv");
        // Aegisub style: [Aegisub Project Garbage]
        let aegi = ASS.replace(
            "\n[V4+ Styles]",
            "\n[Aegisub Project Garbage]\nVideo File: b.mkv\n\n[V4+ Styles]",
        );
        let mut s = session(&aegi);
        assert_eq!(s.project_value("Video File"), "b.mkv");
        s.set_project_value("Keyframes File", "k.txt");
        s.set_project_value("Video File", "");
        assert_eq!(s.project_value("Keyframes File"), "k.txt");
        assert_eq!(s.project_value("Video File"), "");
        assert_eq!(s.info_value("Keyframes File"), "");
    }

    #[test]
    fn rows_edit_undo_and_state() {
        let mut s = session(ASS);
        assert_eq!(s.row_count(), 3);
        let mut r = s.row(1);
        assert_eq!((r.text.as_str(), r.state), ("two", 0));
        r.text = "TWO".into();
        s.set_row(1, &r, "");
        assert_eq!((s.row(1).text.as_str(), s.row(1).state), ("TWO", 1));
        assert!(s.undo());
        assert_eq!(s.row(1).text, "two");
        assert_eq!(s.move_rows(&[2], -2), vec![0]);
        assert_eq!(s.row(0).text, "three");
    }

    #[test]
    fn paste_over_copies_only_chosen_fields() {
        let mut s = session(ASS);
        let before = s.row(1);
        let clip = "Dialogue: 5,0:00:09.00,0:00:10.00,Other,Bob,0,0,0,,new text\nplain";
        let rows = s.paste_over(&[1, 0], clip, 4 | 1024);
        assert_eq!(rows, [1, 0]);
        let r = s.row(1);
        assert_eq!(
            (r.start, r.end, r.text.as_str()),
            (9000, before.end, "new text")
        );
        assert_eq!(
            (r.layer, r.style.as_str()),
            (before.layer, before.style.as_str())
        );
        assert_eq!((s.row(0).start, s.row(0).text.as_str()), (0, "plain"));
    }

    #[test]
    fn adjust_tag_rewrites_from_base_in_one_step() {
        let mut s = session(ASS);
        let base = vec![s.row(0).text];
        s.adjust_tag(&[0], &base, "bord", 1, 3.0, "drag");
        s.adjust_tag(&[0], &base, "bord", 1, 5.0, "drag");
        assert!(s.row(0).text.starts_with(r"{\bord5}"));
        s.undo();
        assert_eq!(s.row(0).text, base[0]);
    }

    #[test]
    fn autosave_keeps_path_and_modified_state() {
        let mut s = session(ASS);
        let path = s.file_info().path;
        let mut r = s.row(0);
        r.text = "edited".into();
        s.set_row(0, &r, "");
        let out = std::env::temp_dir().join(format!("fsb-autosave-{}.ass", std::process::id()));
        s.autosave(out.to_str().unwrap()).unwrap();
        assert!(s.file_info().modified);
        assert_eq!(s.file_info().path, path);
        assert!(std::fs::read_to_string(&out).unwrap().contains(",edited"));
        std::fs::remove_file(out).unwrap();
    }

    #[test]
    fn continuity_thresholds_move_end_or_start() {
        let times = |s: &Session| {
            (0..3)
                .map(|i| (s.row(i).start, s.row(i).end))
                .collect::<Vec<_>>()
        };
        let mut s = session(ASS);
        let spec = PostSpec {
            adjacent_start: 1000,
            rows: vec![0, 1, 2],
            ..Default::default()
        };
        s.post_process(&spec, &[], &[]);
        assert_eq!(times(&s), [(1000, 2000), (2000, 4000), (4000, 6000)]);
        let mut s = session(ASS);
        let spec = PostSpec {
            adjacent: 1000,
            bias: 1.0,
            adjacent_start: 1000,
            rows: vec![0, 1],
            ..Default::default()
        };
        s.post_process(&spec, &[], &[]);
        assert_eq!(times(&s)[..2], [(1000, 3000), (3000, 4000)]);
    }

    #[test]
    fn opens_embedded_srt_as_untitled_ass() {
        let mut s = Session::default();
        let mkv = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/media/subs.mkv");
        s.open_embedded(mkv, 1).unwrap();
        assert_eq!((s.row_count(), s.path.is_none()), (2, true));
        assert_eq!(s.row(1).text, r"Second {\i1}line");
        assert!(s.open_embedded(mkv, 5).is_err());
    }

    #[test]
    fn project_values_do_not_dirty() {
        let mut s = session(ASS);
        s.set_project_value("Video File", "a.mkv");
        assert_eq!(s.info_value("Video File"), "a.mkv");
        assert!(!s.file_info().modified && !s.can_undo());
        s.set_project_value("Video File", "");
        assert_eq!(s.info_value("Video File"), "");
        s.set_info_value("Title", "x");
        assert!(s.file_info().modified && s.can_undo());
    }

    #[test]
    fn rename_style_moves_reset_tags() {
        assert_eq!(
            rename_reset_tags(r"{\rSign\b1}a \rSign {\rSigns}{\rSign}", "Sign", "Title").as_deref(),
            Some(r"{\rTitle\b1}a \rSign {\rSigns}{\rTitle}")
        );
        assert_eq!(rename_reset_tags(r"{\r}x", "Sign", "Title"), None);
    }

    #[test]
    fn translation_rows_and_shift() {
        let mut s = session(ASS);
        s.tl_enable(true);
        let mut r = s.row(0);
        r.text = "jeden".into();
        s.set_row(0, &r, "");
        assert_eq!(s.row_count(), 3);
        let r = s.row(0);
        assert_eq!(
            (r.original.as_str(), r.text.as_str(), r.translated),
            ("one", "jeden", true)
        );
        let spec = ShiftSpec {
            amount: 500,
            lines: 0,
            ..Default::default()
        };
        s.shift(&spec, &[]);
        assert_eq!((s.row(0).start, s.row(2).start), (1500, 5500));
        // both halves of the pair moved
        assert!(
            s.ed.doc
                .events()
                .iter()
                .all(|e| e.event.start.ms() % 1000 == 500)
        );
        s.set_doubtful(&[1], true);
        assert!(s.row(1).doubtful);
        assert_eq!(
            s.filter(&FilterSpec {
                only_doubtful: true,
                ..Default::default()
            }),
            vec![1]
        );
    }

    #[test]
    fn visual_tags_and_karaoke() {
        let mut s = session(ASS);
        s.set_tags(&[0], "\\pos(100,200)", "");
        assert_eq!(s.row(0).text, "{\\pos(100,200)}one");
        let v = s.visual(0);
        assert!(v.has_pos && v.x == 100.0 && v.y == 200.0 && v.align == 2);
        let v = s.visual(1);
        assert_eq!((v.x, v.y), (960.0, 1070.0));
        let mut r = s.row(2);
        r.text = "{\\k50}ka{\\k50}ra".into();
        s.set_row(2, &r, "");
        let syl = s.syllables(2);
        assert_eq!(syl.len(), 2);
        assert_eq!((syl[1].start, syl[1].end), (5500, 6000));
        s.kara_set_boundary(2, 1, 5300, false);
        assert_eq!(s.syllables(2)[0].end, 5300);
    }

    /// cargo test --release -p fs-bridge bench_large -- --ignored --nocapture
    #[test]
    #[ignore]
    fn bench_large() {
        use std::time::Instant;
        let mut text = ASS.split("Dialogue:").next().unwrap().to_string();
        for i in 0..100_000 {
            let t = i * 2;
            text += &format!(
                "Dialogue: 0,{}:{:02}:{:02}.00,{}:{:02}:{:02}.50,Default,,0,0,0,,{{\\k20}}Line {{\\i1}}number{{\\i0}} {i} with some text\n",
                t / 3600, t / 60 % 60, t % 60, t / 3600, t / 60 % 60, t % 60
            );
        }
        let t0 = Instant::now();
        std::hint::black_box(Document::parse(&text));
        eprintln!("parse: {:?}", t0.elapsed());
        let t0 = Instant::now();
        let mut s = session(&text);
        eprintln!("open 100k: {:?}", t0.elapsed());
        let t0 = Instant::now();
        for k in 0..100 {
            let mut r = s.row(50_000);
            r.text += &k.to_string();
            s.set_row(50_000, &r, "typing");
        }
        eprintln!("100 keystrokes: {:?}", t0.elapsed() / 100);
        let t0 = Instant::now();
        for i in 0..60 {
            std::hint::black_box(s.row(50_000 + i));
        }
        eprintln!("60 rows: {:?}", t0.elapsed());
        let t0 = Instant::now();
        s.undo();
        eprintln!("undo: {:?}", t0.elapsed());
        let all: Vec<usize> = (0..s.row_count()).collect();
        let mut time = |what: &str, f: &mut dyn FnMut(&mut Session)| {
            let t0 = Instant::now();
            f(&mut s);
            eprintln!("{what}: {:?}", t0.elapsed());
        };
        time("script_for_render", &mut |s| drop(std::hint::black_box(s.script_for_render(false, -1))));
        time("script_for_render at", &mut |s| drop(std::hint::black_box(s.script_for_render(false, 100_000))));
        time("copy all", &mut |s| drop(std::hint::black_box(s.copy_rows(&all))));
        time("set style all", &mut |s| s.set_rows_field(&all, "style", "Other"));
        time("sort by end", &mut |s| s.sort_rows(1, false, &[]));
        time("column_info", &mut |s| drop(std::hint::black_box(s.column_info())));
        time("rows_at", &mut |s| drop(std::hint::black_box(s.rows_at(100_000_000))));
        let half: Vec<usize> = (0..50_000).map(|i| i * 2).collect();
        time("duplicate 50k", &mut |s| drop(s.duplicate_rows(&half)));
        time("delete 50k", &mut |s| s.delete_rows(&half));
        time("move 50k", &mut |s| drop(s.move_rows(&half, 1)));
        let out = std::env::temp_dir().join(format!("fsb-bench-{}.ass", std::process::id()));
        let t0 = Instant::now();
        s.save(out.to_str().unwrap()).unwrap();
        eprintln!("save: {:?}", t0.elapsed());
        std::fs::remove_file(out).ok();
    }
}
