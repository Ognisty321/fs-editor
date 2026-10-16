//! One editor tab: a document with undo history, its file format and the
//! translation-mode row mapping. Grid rows are logical rows: in TL mode an
//! original/translation event pair is one row, otherwise one event.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};

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
use fs_formats::{Format, LoadOptions, SaveOptions, TextImport};

use crate::ffi::{
    ColumnInfo, ComparePair, ConvertSpec, ExportSpec, FileInfo, FilterSpec, FindHit, FindMatch,
    FindSpec, PostSpec, ResampleSpec, Row, RowSpan, ShiftSpec, StyleData, Syllable, Visual,
};

pub struct Session {
    pub(crate) ed: Editor,
    pub(crate) path: Option<PathBuf>,
    format: Format,
    /// The format of the file at `path`; a conversion away from it saves under a new name.
    disk_format: Format,
    encoding: &'static Encoding,
    bom: bool,
    /// Content hashes of the events as loaded / as last saved, for the grid's number-cell state
    /// (hashes, so the old lines are not kept alive).
    loaded: HashMap<EventId, u64>,
    saved: HashMap<EventId, u64>,
    rows: Vec<TlRow>,
    tl: bool,
    generation: u64,
    /// Cached [`Session::column_info`]; every change drops it except typing that
    /// leaves the column fields alone.
    col_info: Option<ColumnInfo>,
    pub(crate) video_size: Option<(i32, i32)>,
    /// The video's own pixel aspect (`0/0` unknown), for aegisub.video_size's ratio.
    pub(crate) video_sar: (i32, i32),
    pub(crate) framerate: Option<Framerate>,
    /// The rate a file in frames (MicroDVD) was read in, to write it in the same.
    subs_fps: Option<Framerate>,
    /// Kainote's conversion FPS, for a file in frames without its own rate or a video's.
    default_fps: Option<Framerate>,
    pub(crate) keyframes: Vec<i64>,
    /// Aegisub's `Properties.automation_settings`: each export filter's settings as the file
    /// last exported with them, by inline-string-encoded filter name. Kept for the file while it
    /// is open, never written into it (as in Aegisub).
    pub(crate) filter_settings: HashMap<String, String>,
    /// The format and path from before each conversion step still to undo, and from before each
    /// one undone, to redo: undo takes the format back with the lines.
    convert_undo: Vec<(Format, Option<PathBuf>)>,
    convert_redo: Vec<(Format, Option<PathBuf>)>,
}

/// The undo step of a conversion.
const CONVERT: &str = "Convert";

impl Default for Session {
    fn default() -> Self {
        let mut s = Session {
            ed: Editor::new(Document::new()),
            path: None,
            format: Format::Ass,
            disk_format: Format::Ass,
            encoding: encoding_rs::UTF_8,
            bom: true,
            loaded: HashMap::new(),
            saved: HashMap::new(),
            rows: Vec::new(),
            tl: false,
            generation: 0,
            col_info: None,
            video_size: None,
            video_sar: (0, 0),
            framerate: None,
            subs_fps: None,
            default_fps: None,
            keyframes: Vec::new(),
            filter_settings: HashMap::new(),
            convert_undo: Vec::new(),
            convert_redo: Vec::new(),
        };
        // Document::new already holds the one empty line a new tab starts with
        s.ed.mark_saved();
        s.snapshot_loaded();
        s.refresh();
        s
    }
}

/// Transform Framerate's rates: "video" is the video's timecodes, else a rate as typed.
pub(crate) fn export_rate(s: &str, video: &[i64]) -> Result<Framerate, String> {
    if s == "video" {
        Framerate::from_timecodes(video.to_vec())
    } else {
        Framerate::parse(s)
    }
    .map_err(|e| format!("{s}: {e}"))
}

fn event_hash(ev: &fs_core::Event) -> u64 {
    use std::hash::{BuildHasher, BuildHasherDefault};
    BuildHasherDefault::<std::collections::hash_map::DefaultHasher>::default().hash_one(ev)
}

fn events_by_id(doc: &Document) -> HashMap<EventId, u64> {
    doc.events()
        .iter()
        .map(|e| (e.id, event_hash(&e.event)))
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

/// Byte offset of UTF-16 position `pos` in `s` (Qt's), its end past it.
fn byte_at(s: &str, pos: usize) -> usize {
    let mut u = 0;
    for (i, c) in s.char_indices() {
        if u >= pos {
            return i;
        }
        u += c.len_utf16();
    }
    s.len()
}

fn utf16_len(s: &str) -> usize {
    s.chars().map(char::len_utf16).sum()
}

fn wraps(text: &str) -> String {
    tags::visible_text(text)
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

/// The fields of a row's main event that [`Session::column_info`] reads.
fn column_key(ev: &Event) -> (bool, bool, bool, bool, &str, &str, &str) {
    (
        ev.layer != 0,
        ev.margin_l != 0,
        ev.margin_r != 0,
        ev.margin_v != 0,
        ev.style.as_str(),
        tlmode::split_actor(&ev.actor).1,
        tlmode::strip_doubtful(&ev.effect),
    )
}

/// The text libass gets: without the uuencoded attachments, whose fonts the renderer
/// registers once (`Renderer::load_subs`) instead of decoding them on every edit.
/// What `Session::render_subs` hands to the render thread.
pub struct RenderSubs {
    pub(crate) doc: Document,
    tl_only: bool,
    at_ms: i64,
}

/// A copy of the subtitles to autosave off the GUI thread: O(1), the lines are shared.
pub struct Backup(Document);

impl Backup {
    /// Write it as ASS (Aegisub's `.AUTOSAVE.ass`).
    pub fn write(&self, path: &str) -> Result<(), String> {
        fs_formats::save_file(
            &self.0,
            Path::new(path),
            Format::Ass,
            &SaveOptions::default(),
        )
        .map(drop)
        .map_err(|e| format!("{path}: {e}"))
    }
}

impl RenderSubs {
    /// The script for libass. `at_ms >= 0` keeps only the lines shown at that
    /// time: Aegisub sends just those after an edit, and the whole file once
    /// another frame is wanted.
    pub(crate) fn script(&self) -> String {
        let mut doc = if self.tl_only {
            tlmode::translation_only(&self.doc)
        } else {
            self.doc.clone() // sections are shared, only the kept lines are copied
        };
        if self.at_ms >= 0 {
            // libass reads centiseconds: keep lines within 10 ms of the frame time
            let (t, slack) = (Time(self.at_ms), Time(10));
            doc.event_entries_mut().retain(|e| {
                let e = &e.event;
                !e.is_comment() && e.start <= t + slack && e.end + slack > t
            });
        }
        render_script(doc)
    }
}

pub(crate) fn render_script(mut doc: Document) -> String {
    (doc.sections).retain(|s| !matches!(s.kind, SectionKind::Fonts | SectionKind::Graphics));
    doc.to_ass_string()
}

/// `Dialogue: …` line of an event in the default v4+ field order.
fn event_line(ev: &Event) -> String {
    let fields: Vec<String> = EventField::default_format(AssVersion::Ass)
        .iter()
        .map(|f| ev.get_field(f))
        .collect();
    format!("{}: {}", ev.kind.key(), fields.join(","))
}

// Kainote's trees: a comment line marked `[tree_description]` (its text describes the tree) folds
// the lines after it marked `[tree_closed]` or `[tree_opened]`, up to the first one without.
const TREE_DESCRIPTION: &str = "[tree_description]";
const TREE_OPENED: &str = "[tree_opened]";
const TREE_CLOSED: &str = "[tree_closed]";

fn tree_state(actor: &str) -> Option<&'static str> {
    if !actor.starts_with('[') {
        return None; // nearly every line: no need to look further
    }
    tlmode::split_actor(actor)
        .0
        .into_iter()
        .find(|p| p.starts_with("[tree_"))
}

/// `actor` with its tree mark set to `state` (first, where Kainote writes it) or taken off.
fn with_tree_state(actor: &str, state: Option<&'static str>) -> String {
    let (marks, name) = tlmode::split_actor(actor);
    let mut out: Vec<&str> = state.into_iter().collect();
    out.extend(marks.into_iter().filter(|p| !p.starts_with("[tree_")));
    tlmode::join_actor(&out, name)
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
        self.col_info = None;
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

    /// (translation, original) of each translated row: timing works on the translations, the
    /// line's own times, and the originals take theirs after ([`sync_originals`]).
    fn tl_pairs(&self, rows: &[usize]) -> Vec<(usize, usize)> {
        rows.iter()
            .filter_map(|&r| self.tl_row(r))
            .filter_map(|r| Some((r.translation?, r.original)))
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
        self.open_with(path, &LoadOptions::default())
    }

    /// Open with a forced encoding (an empty or unknown label detects it); plain text is read
    /// with Aegisub's text import options.
    pub fn open_as(
        &mut self,
        path: &str,
        encoding: &str,
        actor_separator: &str,
        comment_starter: &str,
        include_blank: bool,
    ) -> Result<(), String> {
        let opts = LoadOptions {
            encoding: Encoding::for_label(encoding.as_bytes()),
            text: TextImport {
                actor_separator: actor_separator.into(),
                comment_starter: comment_starter.into(),
                include_blank,
            },
            ..LoadOptions::default()
        };
        self.open_with(path, &opts)
    }

    fn open_with(&mut self, path: &str, opts: &LoadOptions) -> Result<(), String> {
        // frames in the video's rate when the file names none (Aegisub offers it)
        let opts = &LoadOptions {
            fps: self.framerate.clone().unwrap_or_else(|| opts.fps.clone()),
            ..opts.clone()
        };
        let l = match fs_formats::load_file(Path::new(path), opts) {
            // plain text as Aegisub imports it: a new script, so saving never overwrites the text
            Err(fs_formats::Error::UnknownFormat)
                if path.to_ascii_lowercase().ends_with(".txt") =>
            {
                let bytes = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
                let l = fs_formats::load_plain_text(&bytes, opts)
                    .map_err(|e| format!("{path}: {e}"))?;
                self.load(l, None);
                return Ok(());
            }
            l => l.map_err(|e| format!("{path}: {e}"))?,
        };
        let fps = (l.format == Format::MicroDvd).then(|| {
            (l.declared_fps.and_then(|v| Framerate::from_fps(v).ok())).unwrap_or(opts.fps.clone())
        });
        self.load(l, Some(PathBuf::from(path)));
        self.subs_fps = fps;
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
        let limit = self.ed.limit;
        self.ed = Editor::new(l.doc);
        self.ed.limit = limit;
        self.subs_fps = None;
        self.filter_settings.clear();
        self.path = path;
        self.format = l.format;
        self.disk_format = l.format;
        self.encoding = l.encoding;
        self.bom = l.bom;
        self.snapshot_loaded();
        self.refresh();
    }

    /// The format a save to `p` writes: the extension of a new name picks it (Save as…).
    fn format_for(&self, p: &Path) -> Format {
        match &self.path {
            Some(old) if old == p => self.format,
            _ => p
                .extension()
                .and_then(|e| e.to_str())
                .and_then(Format::from_name)
                .unwrap_or(self.format),
        }
    }

    pub fn save(&mut self, path: &str) -> Result<(), String> {
        let p = PathBuf::from(path);
        let format = self.format_for(&p);
        (self.encoding, self.bom) =
            fs_formats::save_file(&self.ed.doc, &p, format, &self.save_options())
                .map_err(|e| format!("{path}: {e}"))?;
        self.path = Some(p);
        self.format = format;
        self.disk_format = format;
        self.ed.mark_saved();
        self.saved = events_by_id(&self.ed.doc);
        self.generation += 1;
        Ok(())
    }

    fn save_options(&self) -> SaveOptions {
        SaveOptions {
            encoding: self.encoding,
            bom: self.bom,
            // frames as they were read, so the lines no one moved keep theirs
            fps: (self.subs_fps.clone())
                .or_else(|| self.framerate.clone())
                .or_else(|| self.default_fps.clone())
                .unwrap_or_else(|| Framerate::from_rational(24000, 1001, false).unwrap()),
            ..Default::default()
        }
    }

    /// Whether the format a save to `path` writes holds all of the subtitles (Aegisub's CanSave).
    pub fn lossless(&self, path: &str) -> bool {
        let format = self.format_for(Path::new(path));
        fs_formats::lossless(&self.ed.doc, format, &self.save_options())
    }

    /// Write a backup copy as ASS (Aegisub's `.AUTOSAVE.ass`); path, format
    /// and the modified state stay as they are.
    pub fn autosave(&self, path: &str) -> Result<(), String> {
        self.backup().write(path)
    }

    /// The subtitles as they are now, for an autosave written on another thread.
    pub fn backup(&self) -> Box<Backup> {
        Box::new(Backup(self.ed.doc.clone()))
    }

    pub fn save_translation(&self, path: &str) -> Result<(), String> {
        let doc = tlmode::translation_only(&self.ed.doc);
        let opts = SaveOptions {
            encoding: self.encoding,
            bom: self.bom,
            ..Default::default()
        };
        fs_formats::save_file(&doc, Path::new(path), Format::Ass, &opts)
            .map(drop)
            .map_err(|e| format!("{path}: {e}"))
    }

    /// Switch the format the file is saved in (Kainote "Conversion"): the lines become what it
    /// holds, as one undo step, and the next Save asks for a name.
    pub fn convert(&mut self, format: &str) -> Result<(), String> {
        self.convert_with(format, &ConvertSpec::default())
    }

    /// Kainote's frame rate for files in frames with no rate of their own or a video's (0: 23.976).
    pub fn set_default_fps(&mut self, fps: f64) {
        self.default_fps = (fps > 0.0).then(|| Framerate::from_fps(fps).ok()).flatten();
    }

    /// A conversion with Kainote's settings, all in its one undo step.
    pub fn convert_with(&mut self, format: &str, spec: &ConvertSpec) -> Result<(), String> {
        let f = Format::from_name(format).ok_or_else(|| format!("unknown format {format}"))?;
        if self.tl {
            return Err("translation mode is on: turn it off to convert".into());
        }
        let opts = self.save_options();
        let before = (self.format, self.path.clone());
        let g = self.ed.generation();
        let to_ass = f.is_ass() && !self.format.is_ass();
        let ends = (spec.new_ends && f != Format::Tmp) || self.format == Format::Tmp;
        let style = (to_ass && !spec.style.is_empty())
            .then(|| {
                Document::parse(&format!("{CATALOG_HEAD}{}\n", spec.style))
                    .styles()
                    .next()
                    .cloned()
            })
            .flatten();
        if !f.is_ass() || to_ass || ends {
            self.edit(CONVERT, |d| {
                fs_formats::reduce(d, f, &opts);
                if to_ass {
                    if spec.res_x > 0 && spec.res_y > 0 {
                        d.set_info("PlayResX", &spec.res_x.to_string());
                        d.set_info("PlayResY", &spec.res_y.to_string());
                    }
                    if let Some(mut st) = style {
                        st.name = "Default".into();
                        match d.style_index("Default").and_then(|i| d.style_mut(i)) {
                            Some(old) => *old = st,
                            None => {
                                d.add_style(st);
                            }
                        }
                    }
                    if !spec.tags.is_empty() {
                        for i in 0..d.event_count() {
                            if let Some(e) = d.event_mut(i) {
                                e.text.insert_str(0, &spec.tags);
                            }
                        }
                    }
                }
                if ends {
                    fs_core::ops::new_end_times(d, i64::from(spec.ms_per_char));
                }
            });
        }
        // a conversion that changed no line (to ASS) has no step to undo
        if self.ed.generation() != g {
            self.convert_undo.push(before);
        }
        self.format = f;
        if let Some(p) = &mut self.path {
            p.set_extension(f.extension());
        }
        self.generation += 1;
        Ok(())
    }

    /// Forget the file the subtitles came from (a recovered autosave): Save asks for a name, and
    /// they count as modified until then.
    pub fn detach(&mut self) {
        self.path = None;
        self.ed.mark_unsaved();
        self.generation += 1;
    }

    /// Whether the format was converted since the file was read or saved.
    pub fn converted(&self) -> bool {
        self.format != self.disk_format
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
            modified: self.ed.is_modified() || self.converted(),
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

    /// For the untouched starting document: like a project value, neither an undo step nor a
    /// reason to ask "save changes?".
    pub fn set_info_quietly(&mut self, key: &str, value: &str) {
        self.ed.doc.set_info(key, value);
        self.refresh();
    }

    pub fn set_info_value(&mut self, key: &str, value: &str, amend: &str) {
        if self.info_value(key) == value {
            return;
        }
        let (k, v) = (key.to_string(), value.to_string());
        self.edit_amend(amend, &format!("Set {key}"), move |d| {
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

    /// Rows whose start and end match non-comment lines of `other` none of
    /// which has the same text (comments are not compared).
    /// Kainote's comparison: walks `rows` and `other_rows` in order and pairs each line with
    /// the next one on the other side, which must have the same times and/or style when
    /// asked (comments are skipped); returns the pairs whose texts differ.
    pub fn compare<'a>(
        &'a self,
        other: &'a Session,
        rows: &[usize],
        other_rows: &[usize],
        by_times: bool,
        by_styles: bool,
    ) -> Vec<ComparePair> {
        // Kainote walks the other rows from the last pair on, which rescans the rest for every
        // line without a partner; the positions by pairing key find the same partner in log time.
        let key = |c: &(Time, Time, &'a str, &'a str)| {
            (by_times.then_some((c.0, c.1)), by_styles.then_some(c.2))
        };
        let mut at: HashMap<_, Vec<usize>> = HashMap::new();
        for (k, &j) in other_rows.iter().enumerate() {
            if let Some(b) = other.compared(j) {
                at.entry(key(&b)).or_default().push(k);
            }
        }
        let mut out = Vec::new();
        let mut next = 0;
        for &i in rows {
            let Some(a) = self.compared(i) else { continue };
            let Some(ks) = at.get(&key(&a)) else { continue };
            let Some(&k) = ks.get(ks.partition_point(|&k| k < next)) else {
                continue;
            };
            let j = other_rows[k];
            let b = other.compared(j).expect("indexed above");
            next = k + 1;
            if a.3 != b.3 {
                let (spans, other_spans) = diff_spans(a.3, b.3);
                out.push(ComparePair {
                    row: i,
                    other: j,
                    spans,
                    other_spans,
                });
            }
        }
        out
    }

    /// Times, style and shown text of a non-comment row.
    fn compared(&self, i: usize) -> Option<(Time, Time, &str, &str)> {
        let r = *self.rows.get(i)?;
        let ev = &self.ed.doc.events()[r.main()].event;
        let text = if self.tl {
            tlmode::translation_text(&self.ed.doc, r)
        } else {
            &ev.text
        };
        (!ev.is_comment()).then_some((ev.start, ev.end, ev.style.as_str(), text))
    }

    pub fn column_info(&mut self) -> ColumnInfo {
        let ci = self.col_info.take().unwrap_or_else(|| self.scan_columns());
        self.col_info = Some(ci.clone());
        ci
    }

    fn scan_columns(&self) -> ColumnInfo {
        let mut ci = ColumnInfo::default();
        let (mut styles, mut actors, mut effects) =
            (HashSet::new(), HashSet::new(), HashSet::new());
        let events = self.ed.doc.events();
        let mut last = ("", "", "");
        for r in &self.rows {
            let (layer, ml, mr, mv, style, actor, effect) = column_key(&events[r.main()].event);
            ci.any_layer |= layer;
            ci.any_margin_l |= ml;
            ci.any_margin_r |= mr;
            ci.any_margin_v |= mv;
            let cur = (style, actor, effect);
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
        let owned = |set: HashSet<&str>| {
            set.into_iter()
                .filter(|s| !s.is_empty())
                .map(String::from)
                .collect()
        };
        ci.styles = styles.into_iter().map(String::from).collect();
        (ci.actors, ci.effects) = (owned(actors), owned(effects));
        ci
    }

    pub fn row_spans(&self) -> Vec<RowSpan> {
        let events = self.ed.doc.events();
        self.rows
            .iter()
            .map(|r| {
                let ev = &events[r.main()].event;
                RowSpan {
                    start: ev.start.ms(),
                    end: ev.end.ms(),
                    comment: ev.is_comment(),
                }
            })
            .collect()
    }

    pub fn row(&self, i: usize) -> Row {
        let Some(r) = self.tl_row(i) else {
            return Row::default();
        };
        let doc = &self.ed.doc;
        let entry: &Arc<EventEntry> = &doc.events()[r.main()];
        let ev = &entry.event;
        let (markers, actor) = tlmode::split_actor(&ev.actor);
        let hash = event_hash(ev);
        let same = |m: &HashMap<EventId, u64>| m.get(&entry.id) == Some(&hash);
        let state = if same(&self.loaded) {
            0
        } else if same(&self.saved) {
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
        // typing leaves the columns alone: keep their info (a rescan of 100k lines per key)
        let old = self.ed.doc.event(r.main()).cloned();
        let (n, ci) = (self.rows.len(), self.col_info.take());
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
        let new = self.tl_row(i).and_then(|r| self.ed.doc.event(r.main()));
        if self.rows.len() == n && old.as_ref().map(column_key) == new.map(column_key) {
            self.col_info = ci;
        }
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
        self.set_rows_values(rows, field, &vec![value.to_string(); rows.len()], "");
    }

    /// `field` of each of `rows` set to its value in `values` (the edit box's change to every
    /// selected line, Aegisub's SetSelectedRows); edits with the same non-empty `amend` make one
    /// undo step.
    pub fn set_rows_values(&mut self, rows: &[usize], field: &str, values: &[String], amend: &str) {
        let main = matches!(field, "style" | "effect" | "text");
        let mut evs = Vec::with_capacity(rows.len());
        for (k, r) in rows.iter().enumerate().take(values.len()) {
            let Some(r) = self.tl_row(*r) else { continue };
            if main {
                evs.push((r.main(), k));
            } else {
                evs.push((r.original, k));
                evs.extend(r.translation.map(|t| (t, k)));
            }
        }
        let (field, values) = (field.to_string(), values.to_vec());
        self.edit_amend(amend, &format!("Set {field}"), move |d| {
            for (e, k) in evs {
                let (ev, value) = (d.event_mut(e).unwrap(), &values[k]);
                let num = || value.trim().parse::<i32>().unwrap_or(0);
                match field.as_str() {
                    "text" => ev.text = value.clone(),
                    "style" => ev.style = value.clone(),
                    "actor" => {
                        let (p, _) = tlmode::split_actor(&ev.actor);
                        ev.actor = tlmode::join_actor(&p, value);
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
                        let p: Vec<&str> = tlmode::split_actor(value).0;
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
        self.insert_styled(at, start, end, &style)
    }

    /// A new empty line before row `row` (after it with `after`) in its style (Aegisub's
    /// subtitle/insert), timed `start`..`end`; `fit` cuts that to the time the other lines leave.
    pub fn insert_beside(
        &mut self,
        row: usize,
        after: bool,
        start: i64,
        end: i64,
        fit: bool,
    ) -> usize {
        let (mut start, mut end) = (start.max(0), end.max(0));
        if fit {
            for e in self.ed.doc.events().iter().map(|e| &e.event) {
                if after && e.start.0 >= start {
                    end = end.min(e.start.0);
                }
                if !after && e.end.0 <= end {
                    start = start.max(e.end.0);
                }
            }
        }
        let style = self.style_or_default(row);
        self.insert_styled(row + usize::from(after), start, end, &style)
    }

    fn insert_styled(&mut self, at: usize, start: i64, end: i64, style: &str) -> usize {
        let ev_at = self
            .tl_row(at)
            .map(|r| r.original)
            .unwrap_or(self.ed.doc.event_count());
        self.edit("Insert line", |d| {
            d.insert_event(ev_at, Event::dialogue(Time(start), Time(end), style, ""));
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
        self.edit("Delete lines", |d| {
            ops::delete_rows(d, &evs);
            // none left: a new empty one to type into (Aegisub)
            if d.event_count() == 0 {
                d.push_event(Event::dialogue(Time(0), Time(5000), "Default", ""));
            }
        });
    }

    /// The tree mark of the row's line; in translator mode both halves of a pair carry it, so it
    /// stays when the mode is turned off (the translations are kept).
    fn row_tree(&self, row: usize) -> Option<&'static str> {
        let r = self.tl_row(row)?;
        tree_state(&self.ed.doc.event(r.main())?.actor)
    }

    /// Rows that closed trees fold away. A line marked closed with no description above it (the
    /// description was deleted) stays in view.
    pub fn tree_hidden(&self) -> Vec<usize> {
        let mut inside = false;
        let mut out = Vec::new();
        for r in 0..self.rows.len() {
            match self.row_tree(r) {
                Some(TREE_DESCRIPTION) => inside = true,
                Some(TREE_CLOSED) if inside => out.push(r),
                Some(TREE_OPENED | TREE_CLOSED) => {}
                _ => inside = false,
            }
        }
        out
    }

    /// The rows of the tree `row` describes; empty when it describes none.
    pub fn tree_members(&self, row: usize) -> Vec<usize> {
        if self.row_tree(row) != Some(TREE_DESCRIPTION) {
            return Vec::new();
        }
        (row + 1..self.rows.len())
            .take_while(|&r| matches!(self.row_tree(r), Some(TREE_OPENED | TREE_CLOSED)))
            .collect()
    }

    /// Kainote's "Make tree": each run of the rows (those in no tree yet) gets a comment line
    /// before it holding `description`, and folds closed. Returns the description rows.
    pub fn make_tree(&mut self, rows: &[usize], description: &str) -> Vec<usize> {
        let mut rows: Vec<usize> = rows
            .iter()
            .copied()
            .filter(|&r| r < self.rows.len() && self.row_tree(r).is_none())
            .collect();
        rows.sort_unstable();
        rows.dedup();
        let doc = &self.ed.doc;
        // before the run's first row: ahead of the original of a translator's pair
        let heads: Vec<(usize, Event)> = rows
            .chunk_by(|a, b| b - a == 1)
            .map(|run| {
                let r = self.rows[run[0]];
                let style = &doc.event(r.main()).unwrap().style;
                let mut ev = Event::dialogue(Time(0), Time(0), style, description);
                ev.kind = fs_core::model::EventKind::Comment;
                ev.actor = TREE_DESCRIPTION.into();
                (r.original, ev)
            })
            .collect();
        let evs = self.all_events(&rows);
        if heads.is_empty() {
            return Vec::new();
        }
        let made = self.edit("Make tree", move |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.actor = with_tree_state(&ev.actor, Some(TREE_CLOSED));
            }
            d.insert_events(heads)
        });
        self.rows_of_events(&made)
    }

    /// Kainote's tree "Add lines": the rows in no tree join the tree `head` describes, those above
    /// it at its start and those below at its end, in order, folded as it is. Returns the head's
    /// new row, -1 when no row joined.
    pub fn tree_add_lines(&mut self, head: usize, rows: &[usize]) -> i64 {
        if self.row_tree(head) != Some(TREE_DESCRIPTION) {
            return -1;
        }
        let members = self.tree_members(head);
        let state = members
            .first()
            .and_then(|&m| self.row_tree(m))
            .unwrap_or(TREE_OPENED);
        let mut adding: Vec<usize> = rows
            .iter()
            .copied()
            .filter(|&r| r < self.rows.len() && self.row_tree(r).is_none())
            .collect();
        adding.sort_unstable();
        adding.dedup();
        if adding.is_empty() {
            return -1;
        }
        let end = head + 1 + members.len(); // the first row after the tree
        let mut groups: Vec<Option<Vec<Arc<EventEntry>>>> =
            self.groups().into_iter().map(Some).collect();
        let (mut above, mut below) = (Vec::new(), Vec::new());
        for &r in &adding {
            let mut g = groups[r].take().unwrap();
            for e in &mut g {
                let ev = &mut Arc::make_mut(e).event;
                ev.actor = with_tree_state(&ev.actor, Some(state));
            }
            if r < head {
                above.push(g)
            } else {
                below.push(g)
            }
        }
        let new_head = head - above.len();
        let mut out = Vec::with_capacity(groups.len());
        for (r, g) in groups.into_iter().enumerate() {
            if r == end {
                out.append(&mut below);
            }
            out.extend(g);
            if r == head {
                out.append(&mut above);
            }
        }
        out.append(&mut below); // a tree that ends the file
        self.set_groups("Add lines to tree", out);
        new_head as i64
    }

    /// Open a closed tree, or close an open one (a click on its description).
    pub fn toggle_tree(&mut self, row: usize) {
        let members = self.tree_members(row);
        let Some(&first) = members.first() else {
            return;
        };
        let closed = self.row_tree(first) == Some(TREE_CLOSED);
        let evs = self.all_events(&members);
        let (to, desc) = if closed {
            (TREE_OPENED, "Open tree")
        } else {
            (TREE_CLOSED, "Close tree")
        };
        self.edit(desc, move |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.actor = with_tree_state(&ev.actor, Some(to));
            }
        });
    }

    /// Undo a tree: its lines lose the mark and the description line goes.
    pub fn remove_tree(&mut self, row: usize) {
        if self.row_tree(row) != Some(TREE_DESCRIPTION) {
            return;
        }
        let evs = self.all_events(&self.tree_members(row));
        let head = self.all_events(&[row]);
        self.edit("Remove tree", move |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.actor = with_tree_state(&ev.actor, None);
            }
            d.remove_events(&head);
        });
    }

    pub fn clear_text(&mut self, rows: &[usize]) {
        let evs = self.main_events(rows);
        self.edit("Delete text", |d| {
            for e in evs {
                d.event_mut(e).unwrap().text.clear();
            }
        });
    }

    /// Aegisub's "Clear text, keep override tags": only the `{\...}` blocks stay; comments
    /// and drawings go with the text.
    pub fn clear_text_keep_tags(&mut self, rows: &[usize]) {
        let evs = self.main_events(rows);
        self.edit("Delete text", |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                ev.text = override_blocks(&ev.text);
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

    /// Kainote's "Set FPS from video": every time of the script stretched about `anchor`, so
    /// that `from` lands on `to` (subtitles timed to another frame rate).
    pub fn stretch_times(&mut self, anchor: i64, from: i64, to: i64) {
        if from == anchor {
            return;
        }
        let k = (to - anchor) as f64 / (from - anchor) as f64;
        let at = |t: Time| {
            Time(
                (anchor as f64 + (t.ms() - anchor) as f64 * k)
                    .round()
                    .max(0.0) as i64,
            )
        };
        self.edit("Set FPS from video", |d| {
            for i in 0..d.events().len() {
                let ev = d.event_mut(i).unwrap();
                (ev.start, ev.end) = (at(ev.start), at(ev.end));
            }
        });
    }

    /// Aegisub's Export: a copy through the Fix Styles / Transform Framerate steps, in the
    /// format `path`'s name says and `spec.encoding`. The open file, its path and saved state
    /// stay as they are.
    pub fn export_copy(&self, path: &str, spec: &ExportSpec, video: &[i64]) -> Result<(), String> {
        self.export_doc(self.ed.doc.clone(), path, spec, video)
    }

    /// `export_copy` of an already transformed document (automation export filters).
    pub(crate) fn export_doc(
        &self,
        mut doc: Document,
        path: &str,
        spec: &ExportSpec,
        video: &[i64],
    ) -> Result<(), String> {
        let rate = |s: &str| export_rate(s, video);
        if spec.fix_styles {
            ops::fix_styles(&mut doc);
        }
        if !spec.fps_to.is_empty() {
            ops::change_fps(&mut doc, &rate(&spec.fps_from)?, &rate(&spec.fps_to)?);
        }
        let p = Path::new(path);
        let ext = p.extension().and_then(|e| e.to_str()).unwrap_or("");
        let opts = SaveOptions {
            encoding: Encoding::for_label(spec.encoding.as_bytes()).unwrap_or(self.encoding),
            bom: self.bom,
            fps: self
                .framerate
                .clone()
                .unwrap_or_else(|| Framerate::from_rational(24000, 1001, false).unwrap()),
            ..Default::default()
        };
        let name = (p.file_name().and_then(|n| n.to_str()))
            .unwrap_or("")
            .to_ascii_lowercase();
        let write = |bytes: Vec<u8>| std::fs::write(p, bytes).map_err(|e| format!("{path}: {e}"));
        // the rate of Encore's and TranStation's SMPTE times, drop-frame as asked
        let smpte = || {
            if spec.smpte_fps == "video" {
                return rate("video");
            }
            let fps = &spec.smpte_fps;
            match fps.split_once('/') {
                Some((n, d)) => n.trim().parse().ok().zip(d.trim().parse().ok()),
                None => fps.trim().parse().ok().map(|n| (n, 1)),
            }
            .ok_or_else(|| format!("{fps}: invalid frame rate"))
            .and_then(|(n, d)| {
                Framerate::from_rational(n, d, spec.smpte_drop).map_err(|e| e.to_string())
            })
        };
        // Aegisub's write-only formats, picked by the end of the name as there
        if name.ends_with(".encore.txt") {
            let text = fs_formats::export::write_encore(&doc, &smpte()?);
            return write(fs_formats::encoding::encode(
                &text,
                encoding_rs::UTF_8,
                true,
            ));
        }
        if name.ends_with(".transtation.txt") {
            let text = fs_formats::export::write_transtation(&doc, &smpte()?);
            return write(fs_formats::encoding::encode(&text, opts.encoding, opts.bom));
        }
        if ext.eq_ignore_ascii_case("stl") {
            let offset: Vec<u8> = (spec.ebu_timecode_offset.split(':'))
                .map(|v| v.trim().parse().unwrap_or(0))
                .collect();
            let settings = fs_formats::export::EbuSettings {
                tv_standard: spec.ebu_tv_standard,
                text_encoding: spec.ebu_text_encoding,
                max_line_length: spec.ebu_max_line_length.max(1) as usize,
                line_wrapping: spec.ebu_line_wrapping,
                translate_alignments: spec.ebu_translate_alignments,
                timecode_offset: std::array::from_fn(|k| offset.get(k).copied().unwrap_or(0)),
                inclusive_end_times: spec.ebu_inclusive_end_times,
                display_standard: spec.ebu_display_standard,
            };
            return write(fs_formats::export::write_ebu_stl(&doc, &settings)?);
        }
        if ext.eq_ignore_ascii_case("txt") {
            // Aegisub's plain text export; TMP and MPL2 come from converting the subtitles
            let text = fs_formats::write_plain_text(&doc);
            let bytes = fs_formats::encoding::encode(&text, opts.encoding, opts.bom);
            return std::fs::write(p, bytes).map_err(|e| format!("{path}: {e}"));
        }
        let format = Format::from_name(ext).unwrap_or(Format::Ass);
        fs_formats::save_file(&doc, p, format, &opts)
            .map(drop)
            .map_err(|e| format!("{path}: {e}"))
    }

    /// Aegisub "Recombine Lines"; returns the rows left of the selection.
    pub fn recombine_rows(&mut self, rows: &[usize]) -> Vec<usize> {
        // not in translator mode: it makes and drops lines by the texts' overlaps, which the
        // original and the translation do not share (Aegisub has no such mode)
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

    /// 0 join, 1 keep first, 2 keep last, 3 as karaoke. In translator mode the originals join
    /// as the translations do, into the first row's pair.
    pub fn join_rows(&mut self, rows: &[usize], mode: u8) -> i64 {
        if rows.len() < 2 {
            return -1;
        }
        if self.tl {
            return self.join_pairs(rows, mode);
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

    /// The rows joined pair by pair: originals with originals, translations with translations
    /// (an untranslated row's empty), timed by the translations.
    fn join_pairs(&mut self, rows: &[usize], mode: u8) -> i64 {
        let mut sel: Vec<TlRow> = rows.iter().filter_map(|&r| self.tl_row(r)).collect();
        sel.sort_by_key(|r| r.original);
        sel.dedup();
        if sel.len() < 2 {
            return -1;
        }
        let doc = &self.ed.doc;
        let main = |r: &TlRow| doc.event(r.main()).unwrap();
        let originals: Vec<Event> = (sel.iter())
            .map(|r| Event {
                start: main(r).start,
                end: main(r).end,
                ..doc.event(r.original).unwrap().clone()
            })
            .collect();
        let translations: Vec<Event> = (sel.iter())
            .map(|r| match r.translation {
                Some(_) => main(r).clone(),
                None => Event {
                    text: String::new(),
                    ..main(r).clone()
                },
            })
            .collect();
        let translated = sel.iter().any(|r| r.translation.is_some());
        let mut o = join_apart(originals, mode);
        let t = translated.then(|| join_apart(translations, mode));
        let style = tlmode::tl_style(doc).unwrap_or_else(|| tlmode::DEFAULT_TL_STYLE.into());
        let first = sel[0];
        let gone: Vec<usize> = (sel[1..].iter())
            .flat_map(|r| std::iter::once(r.original).chain(r.translation))
            .collect();
        self.edit("Join lines", move |d| {
            d.remove_events(&gone); // all after the first pair
            let Some(t) = t else {
                *d.event_mut(first.original).unwrap() = o;
                return;
            };
            o.style = style;
            o.effect = tlmode::strip_doubtful(&o.effect).to_string();
            match first.translation {
                Some(ti) => {
                    *d.event_mut(first.original).unwrap() = o;
                    *d.event_mut(ti).unwrap() = t;
                }
                None => {
                    // the first row was untranslated: it becomes a pair
                    *d.event_mut(first.original).unwrap() = t;
                    d.insert_event(first.original, o);
                }
            }
        });
        self.row_of_event(first.original).map_or(-1, |r| r as i64)
    }

    /// Split at a byte position of the text (of the original when `original`, in translator
    /// mode); `at_ms` -1 estimates the time, -2 keeps both parts' times. A translator's pair
    /// splits the half at the cursor; the other half stays whole on the first part, as typed.
    pub fn split_row(&mut self, row: usize, byte_pos: usize, at_ms: i64, original: bool) -> i64 {
        let Some(r) = self.tl_row(row) else { return -1 };
        let at = (at_ms >= 0).then_some(Time(at_ms));
        if self.tl {
            match r.translation {
                Some(t) => return self.split_pair(r.original, t, byte_pos, at_ms, original),
                // an untranslated row has no translation to split
                None if !original => return -1,
                None => {}
            }
        }
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

    fn split_pair(
        &mut self,
        o: usize,
        t: usize,
        byte_pos: usize,
        at_ms: i64,
        original: bool,
    ) -> i64 {
        let doc = &self.ed.doc;
        let (oe, te) = (doc.event(o).unwrap(), doc.event(t).unwrap());
        let mut d = Document::empty();
        d.push_event(if original { oe } else { te }.clone());
        if ops::split_at(&mut d, 0, byte_pos, (at_ms >= 0).then_some(Time(at_ms))).is_none() {
            return -1;
        }
        let (a, b) = (d.event(0).unwrap().clone(), d.event(1).unwrap().clone());
        let whole = |ev: &Event| {
            let first = Event {
                start: a.start,
                end: a.end,
                ..ev.clone()
            };
            let second = Event {
                start: b.start,
                end: b.end,
                text: String::new(),
                ..ev.clone()
            };
            (first, second)
        };
        let ((o1, o2), (t1, t2)) = if original {
            ((a.clone(), b.clone()), whole(te))
        } else {
            (whole(oe), (a.clone(), b.clone()))
        };
        let times = (te.start, te.end);
        let mut parts = [o1, t1, o2, t2];
        if at_ms == -2 {
            for e in &mut parts {
                (e.start, e.end) = times;
            }
        }
        let [o1, t1, o2, t2] = parts;
        self.edit("Split line", move |d| {
            *d.event_mut(o).unwrap() = o1;
            *d.event_mut(t).unwrap() = t1;
            d.insert_events(vec![(t + 1, o2), (t + 1, t2)]);
        });
        self.row_of_event(t + 1).map_or(-1, |r| r as i64)
    }

    pub fn swap_rows(&mut self, a: usize, b: usize) {
        let (Some(ra), Some(rb)) = (self.tl_row(a), self.tl_row(b)) else {
            return;
        };
        self.edit("Swap lines", |d| ops::swap_rows(d, ra.main(), rb.main()));
    }

    pub fn explode_rows(
        &mut self,
        rows: &[usize],
        counts: &[usize],
        texts: &[String],
        starts: &[i64],
        ends: &[i64],
    ) -> Vec<usize> {
        let n = texts.len().min(starts.len()).min(ends.len());
        // (row, its parts), by row
        let mut jobs = Vec::new();
        let mut at = 0;
        for (&row, &count) in rows.iter().zip(counts) {
            let parts = at.min(n)..(at + count).min(n);
            at += count;
            if let Some(r) = self.tl_row(row).filter(|_| !parts.is_empty()) {
                jobs.push((r, parts));
            }
        }
        jobs.sort_by_key(|j| j.0.original);
        jobs.dedup_by_key(|j| j.0.original);
        let gone: Vec<usize> = (jobs.iter())
            .flat_map(|(r, _)| std::iter::once(r.original).chain(r.translation))
            .collect();
        // the parts go where their line was, once the lines before it are gone too
        let doc = &self.ed.doc;
        let (mut parts, mut removed) = (Vec::new(), 0);
        for (r, range) in jobs {
            let at = r.original - removed;
            removed += 1 + usize::from(r.translation.is_some());
            let base = doc.event(r.main()).unwrap();
            let original = r.translation.map(|_| doc.event(r.original).unwrap());
            for (i, p) in range.enumerate() {
                let (start, end) = (Time(starts[p]), Time(ends[p]));
                if let Some(o) = original {
                    // a translator's pair: the original goes with each copy of the whole line, and
                    // with the first piece of a split text
                    let text = if i == 0 || texts[p] == base.text {
                        o.text.clone()
                    } else {
                        String::new()
                    };
                    parts.push((
                        at,
                        Event {
                            start,
                            end,
                            text,
                            ..o.clone()
                        },
                    ));
                }
                parts.push((
                    at,
                    Event {
                        start,
                        end,
                        text: texts[p].clone(),
                        ..base.clone()
                    },
                ));
            }
        }
        if parts.is_empty() {
            return Vec::new();
        }
        let made = self.edit("Split lines", |d| {
            d.remove_events(&gone);
            d.insert_events(parts)
        });
        self.rows_of_events(&made)
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
        // the rows move as one block: it can reach the end even when they had gaps
        let delta = delta.clamp(-(sel[0] as i64), n - sel.len() as i64 - sel[0] as i64);
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

    /// Aegisub's "Change End" / "Change Start": a row ends at the next row's start (or starts
    /// at the previous row's end) when that row is selected too; a single row uses its neighbour.
    pub fn make_continuous(&mut self, rows: &[usize], next: bool) {
        let mut rows = rows.to_vec();
        rows.sort_unstable();
        rows.dedup();
        if let [r] = rows[..] {
            rows = if next {
                vec![r, r + 1]
            } else {
                r.checked_sub(1).into_iter().chain([r]).collect()
            };
        }
        let pairs: Vec<(usize, usize)> = rows
            .windows(2)
            .filter(|w| w[1] == w[0] + 1)
            .filter_map(|w| Some((self.tl_row(w[0])?.main(), self.tl_row(w[1])?.main())))
            .collect();
        let tl = self.tl_pairs(&rows);
        if !pairs.is_empty() {
            self.edit("Continuous times", |d| {
                ops::make_continuous(d, &pairs, next);
                sync_originals(d, &tl);
            });
        }
    }

    /// Sort by 0 start, 1 end, 2 style, 3 actor, 4 effect, 5 layer, 6 text.
    pub fn sort_rows(&mut self, key: u8, selected_only: bool, rows: &[usize]) {
        // compared in place: a copy of both events per comparison was most of a sort's time
        fn main(g: &[Arc<EventEntry>]) -> &Event {
            &g.last().unwrap().event
        }
        let cmp = |a: &Event, b: &Event| match key {
            1 => a.end.cmp(&b.end),
            2 => a.style.cmp(&b.style),
            3 => a.actor.cmp(&b.actor),
            4 => a.effect.cmp(&b.effect),
            5 => a.layer.cmp(&b.layer),
            6 => a.text.cmp(&b.text),
            _ => a.start.cmp(&b.start),
        };
        let mut groups = self.groups();
        if selected_only {
            let mut sel: Vec<usize> = rows.iter().copied().filter(|&r| r < groups.len()).collect();
            sel.sort_unstable();
            sel.dedup();
            let mut items: Vec<_> = sel.iter().map(|&r| groups[r].clone()).collect();
            items.sort_by(|a, b| cmp(main(a), main(b)));
            for (r, g) in sel.into_iter().zip(items) {
                groups[r] = g;
            }
        } else {
            // in order already (Kainote's sort on opening a sorted file): no empty undo step
            if groups
                .windows(2)
                .all(|w| cmp(main(&w[0]), main(&w[1])).is_le())
            {
                return;
            }
            groups.sort_by(|a, b| cmp(main(a), main(b)));
        }
        self.set_groups("Sort lines", groups);
    }

    /// Rows picked by the Shift Times "Which lines" choice (0 all, 1 selected,
    /// 2 from the first selected, 3/4 starting at or after/before the first
    /// selected line (Kainote), 5 the given styles).
    pub fn spec_rows(
        &self,
        lines: u8,
        selected: &[usize],
        active: usize,
        styles: &[String],
    ) -> Vec<usize> {
        let n = self.rows.len();
        let start_of = |r: usize| self.row(r).start;
        let first = selected.iter().copied().min();
        match lines {
            1 => selected.to_vec(),
            2 => (first.unwrap_or(active)..n).collect(),
            3 => {
                let t = start_of(first.unwrap_or(active));
                (0..n).filter(|&r| start_of(r) >= t).collect()
            }
            4 => {
                let t = start_of(first.unwrap_or(active));
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
        // the end fixes compare lines: the rows' own, so a pair is not one line and the next
        let (mains, tl) = (self.main_events(&rows), self.tl_pairs(&rows));
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
            let times = |d: &Document, e: usize| d.event(e).map(|ev| (ev.start, ev.end));
            let old: Vec<_> = mains.iter().map(|&e| times(d, e)).collect();
            let before: Vec<i64> = evs
                .iter()
                .map(|&e| d.event(e).unwrap().start.ms())
                .collect();
            let amount = match &fr {
                Some(f) => ShiftAmount::Frames(spec.amount, f),
                None => ShiftAmount::Time(Time(spec.amount)),
            };
            ops::shift_times(d, &Rows::Selected(evs.clone()), amount, which, None);
            if spec.tag_times && which == ShiftWhich::Start {
                // the tags' times keep their place on the video
                for (k, &e) in evs.iter().enumerate() {
                    let delta = d.event(e).unwrap().start.ms() - before[k];
                    shift_tag_times(d.event_mut(e).unwrap(), -delta);
                }
            }
            match spec.end_fix {
                1 => fix_overlaps(d, &mains),
                2 => {
                    new_end_times(d, &mains);
                    fix_overlaps(d, &mains);
                }
                _ => {}
            }
            sync_originals(d, &tl);
            // lines, not events: a translation pair is one
            (mains.iter().zip(&old))
                .filter(|&(&e, o)| times(d, e) != *o)
                .count()
        })
    }

    pub fn snap_to_video(&mut self, rows: &[usize], start: i64, end: i64, set_start: bool) {
        let evs = self.all_events(rows);
        let (start, end) = (Time(start), Time(end));
        self.edit("Snap to video", |d| {
            for e in evs {
                let ev = d.event_mut(e).unwrap();
                if set_start || ev.start > start {
                    ev.start = start;
                }
                if !set_start || ev.end < end {
                    ev.end = end;
                }
            }
        });
    }

    pub fn post_process(&mut self, spec: &PostSpec, keyframes: &[i64], timecodes: &[i64]) {
        // the rows' own events: with both halves of a pair, one line's original became the
        // "next line" of the one before
        let (evs, tl) = (self.main_events(&spec.rows), self.tl_pairs(&spec.rows));
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
                adjacent_overlap: opt(spec.overlap),
                adjacent_bias: spec.bias,
                keyframes: match (&fr, spec.snap_keyframes && !kf.is_empty()) {
                    (Some(f), true) => Some((kf.as_slice(), f)),
                    _ => None,
                },
                kf_before_start: Time(spec.kf_before_start),
                kf_after_start: Time(spec.kf_after_start),
                kf_before_end: Time(spec.kf_before_end),
                kf_after_end: Time(spec.kf_after_end),
                styles: Vec::new(),
            };
            ops::post_process(d, &Rows::Selected(evs), &pp);
            sync_originals(d, &tl);
        });
    }

    /// The rows' lasting ids (their main event's, which translating or turning translator mode
    /// off keeps), for remembering lines across edits that move them.
    pub fn row_ids(&self, rows: &[usize]) -> Vec<u64> {
        (rows.iter())
            .filter_map(|&r| self.ed.doc.event_id(self.tl_row(r)?.main()))
            .collect()
    }

    /// The rows, in order, that hold any of `ids` ([`Session::row_ids`]).
    pub fn rows_of_ids(&self, ids: &[u64]) -> Vec<usize> {
        if ids.is_empty() {
            return Vec::new();
        }
        let want: HashSet<u64> = ids.iter().copied().collect();
        let doc = &self.ed.doc;
        (0..self.rows.len())
            .filter(|&i| {
                let r = self.rows[i];
                (std::iter::once(r.original).chain(r.translation))
                    .any(|e| doc.event_id(e).is_some_and(|id| want.contains(&id)))
            })
            .collect()
    }

    /// Rows the grid shows. Reads the events directly: building a `Row` per
    /// line (text copies, CPS) made this the slowest step of an edit.
    pub fn filter(&self, spec: &FilterSpec) -> Vec<usize> {
        let doc = &self.ed.doc;
        let mut hide_rows = spec.hide_rows.clone();
        hide_rows.sort_unstable();
        (0..self.rows.len())
            .filter(|&i| {
                let r = self.rows[i];
                let ev = &doc.events()[r.main()].event;
                let untranslated = || {
                    let text = if self.tl {
                        tlmode::translation_text(doc, r)
                    } else {
                        &ev.text
                    };
                    r.translation.is_some() && !text.trim().is_empty()
                };
                let hidden = spec.hide_styles.contains(&ev.style)
                    || hide_rows.binary_search(&i).is_ok()
                    || (spec.hide_comments && ev.is_comment())
                    || (spec.only_doubtful && !tlmode::is_doubtful(&ev.effect))
                    || (spec.only_untranslated && untranslated());
                hidden == spec.invert
            })
            .collect()
    }

    fn matcher(spec: &FindSpec) -> Result<TextMatcher, String> {
        // shortcut: with Skip text the start and end are each block's; the line's if people
        // combine the two
        let anchored = spec.at_start || spec.at_end;
        let pattern = if anchored {
            ops::anchored(&spec.pattern, spec.regex, spec.at_start, spec.at_end)
        } else {
            spec.pattern.clone()
        };
        let mut m = TextMatcher::new(
            &pattern,
            spec.regex || anchored,
            spec.match_case,
            field_of(spec.field),
        )
        .map_err(|e| e.to_string())?;
        m.strip_tags = spec.skip_tags;
        m.only_tags = spec.skip_text;
        m.is_regex = spec.regex;
        Ok(m)
    }

    /// Find next and Replace have something to look for: Aegisub does nothing for an empty
    /// search, Kainote's start / end of text find the field's ends.
    fn searchable(spec: &FindSpec) -> bool {
        !spec.pattern.is_empty() || spec.at_start || spec.at_end
    }

    /// The events row `r` is searched in, with whether each is the original: the text of both
    /// halves of a translation row, the original first (Kainote), else the line.
    fn search_events(&self, spec: &FindSpec, r: usize) -> impl Iterator<Item = (usize, bool)> {
        let row = self.rows[r];
        let both = self.tl && field_of(spec.field) == Field::Text;
        let first = if both {
            (row.original, true)
        } else {
            (row.main(), false)
        };
        std::iter::once(first).chain(row.translation.filter(|_| both).map(|t| (t, false)))
    }

    /// Whether row `r` is searched for `spec` (comments) and its field matches.
    fn row_hit(&self, m: &TextMatcher, spec: &FindSpec, r: usize) -> bool {
        let ev = |e| self.ed.doc.event(e).unwrap();
        (spec.include_comments || !ev(self.rows[r].main()).is_comment())
            && self.search_events(spec, r).any(|(e, _)| m.is_match(ev(e)))
    }

    pub fn find_next(
        &self,
        spec: &FindSpec,
        row: usize,
        original: bool,
        pos: usize,
    ) -> Result<FindHit, String> {
        let m = Self::matcher(spec)?;
        let n = self.rows.len();
        let none = FindHit {
            row: -1,
            ..Default::default()
        };
        if n == 0 || !Self::searchable(spec) {
            return Ok(none);
        }
        // the rows to search, marked once ("from selected" passes most of the file)
        let mut only = vec![spec.rows.is_empty(); n];
        for &r in spec.rows.iter().filter(|&&r| r < n) {
            only[r] = true;
        }
        // on from `pos`, through the other rows, and around to the start of `row` itself
        for k in 0..=n {
            let r = (row.min(n - 1) + k) % n;
            if !only[r]
                || !(spec.include_comments
                    || !self.ed.doc.event(self.rows[r].main()).unwrap().is_comment())
            {
                continue;
            }
            for (e, orig) in self.search_events(spec, r) {
                if k == 0 && orig && !original {
                    continue;
                }
                let ev = self.ed.doc.event(e).unwrap();
                let text = m.field_value(ev);
                let from = if k == 0 && orig == original {
                    byte_at(text, pos)
                } else {
                    0
                };
                // most rows have no match, and asking whether is the quickest
                if from == 0 && !m.is_match(ev) {
                    continue;
                }
                if let Some(at) = m.find_at(text, from) {
                    let start = utf16_len(&text[..at.start]);
                    return Ok(FindHit {
                        row: r as i64,
                        original: orig,
                        start,
                        end: start + utf16_len(&text[at]),
                    });
                }
            }
        }
        Ok(none)
    }

    pub fn replace_hit(&mut self, spec: &FindSpec, hit: &FindHit) -> Result<i64, String> {
        let m = Self::matcher(spec)?;
        if !Self::searchable(spec) || hit.row < 0 || hit.row as usize >= self.rows.len() {
            return Ok(-1);
        }
        let Some((e, _)) = self
            .search_events(spec, hit.row as usize)
            .find(|&(_, o)| o == hit.original)
        else {
            return Ok(-1);
        };
        let text = m.field_value(self.ed.doc.event(e).unwrap());
        let at = byte_at(text, hit.start)..byte_at(text, hit.end);
        let r = ops::Replace {
            matcher: m,
            replacement: spec.replacement.clone(),
            first_only: true,
            keep_case: false,
        };
        let Some((new, end)) = r.replace_one(text, at) else {
            return Ok(-1);
        };
        let end = utf16_len(&new[..end]) as i64;
        self.edit("Replace", |d| {
            r.matcher.set_field(d.event_mut(e).unwrap(), new)
        });
        Ok(end)
    }

    /// Every row whose field matches, in order (Kainote's Find all).
    pub fn find_all(&self, spec: &FindSpec) -> Result<Vec<usize>, String> {
        let m = Self::matcher(spec)?;
        let n = self.rows.len();
        Ok(if spec.rows.is_empty() {
            (0..n).filter(|&r| self.row_hit(&m, spec, r)).collect()
        } else {
            let mut rows: Vec<usize> = spec.rows.iter().copied().filter(|&r| r < n).collect();
            rows.sort_unstable();
            rows.dedup();
            rows.retain(|&r| self.row_hit(&m, spec, r));
            rows
        })
    }

    pub fn find_matches(&self, spec: &FindSpec, max: usize) -> Result<Vec<FindMatch>, String> {
        let m = Self::matcher(spec)?;
        let mut out = Vec::new();
        for r in self.find_all(spec)? {
            for (e, original) in self.search_events(spec, r) {
                let text = m.field_value(self.ed.doc.event(e).unwrap());
                let mut from = 0;
                while let Some(at) = m.find_at(text, from) {
                    if out.len() == max {
                        return Ok(out);
                    }
                    let start = utf16_len(&text[..at.start]);
                    out.push(FindMatch {
                        row: r,
                        original,
                        start,
                        end: start + utf16_len(&text[at.clone()]),
                        text: text.into(),
                    });
                    // an empty match (^, \b) is passed by a character
                    from = match text[at.end..].chars().next() {
                        Some(c) if at.is_empty() => at.end + c.len_utf8(),
                        None if at.is_empty() => break,
                        _ => at.end,
                    };
                }
            }
        }
        Ok(out)
    }

    /// The events `spec` replaces in and how.
    fn replace_job(
        &self,
        spec: &FindSpec,
        keep_case: bool,
    ) -> Result<(Rows, ops::Replace), String> {
        let m = Self::matcher(spec)?;
        let n = if Self::searchable(spec) {
            self.rows.len()
        } else {
            0
        };
        let rows: Vec<usize> = if spec.rows.is_empty() {
            (0..n).collect()
        } else {
            spec.rows.iter().copied().filter(|&r| r < n).collect()
        };
        let doc = &self.ed.doc;
        let evs: Vec<usize> = rows
            .into_iter()
            .filter(|&r| {
                spec.include_comments || !doc.event(self.rows[r].main()).unwrap().is_comment()
            })
            .flat_map(|r| self.search_events(spec, r))
            .map(|(e, _)| e)
            .collect();
        let r = ops::Replace {
            matcher: m,
            replacement: spec.replacement.clone(),
            first_only: false,
            keep_case,
        };
        Ok((Rows::Selected(evs), r))
    }

    pub fn replace_all(&mut self, spec: &FindSpec) -> Result<usize, String> {
        let (rows, r) = self.replace_job(spec, false)?;
        Ok(self.edit("Replace", |d| ops::replace(d, &rows, &r)))
    }

    /// Kainote's minor error corrector: every rule in turn, one undo step. A rule that
    /// ignores case writes its replacement in the case of each match ("Sie" -> "Się").
    pub fn replace_rules(&mut self, specs: &[FindSpec]) -> Result<usize, String> {
        let jobs = specs
            .iter()
            .map(|s| {
                self.replace_job(s, !s.match_case)
                    .map_err(|e| format!("{}: {e}", s.pattern))
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(self.edit("Correct minor errors", |d| {
            jobs.iter().map(|(rows, r)| ops::replace(d, rows, r)).sum()
        }))
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
        let events = self.ed.doc.events();
        (0..self.rows.len())
            .filter(|&i| i != row)
            .filter(|&i| {
                let e = &events[self.rows[i].main()].event;
                !e.is_comment() && e.start < me.end && me.start < e.end
            })
            .collect()
    }

    /// Rows visible at `ms` (grid tint, F2 select from video time).
    pub fn rows_at(&self, ms: i64) -> Vec<usize> {
        let events = self.ed.doc.events();
        (0..self.rows.len())
            .filter(|&i| {
                let e = &events[self.rows[i].main()].event;
                !e.is_comment() && e.start.ms() <= ms && ms < e.end.ms()
            })
            .collect()
    }

    /// Kainote's line for a time: the first dialogue with text showing at `ms` (end included),
    /// else the one starting nearest it, the earlier on a tie; -1 when there is none.
    pub fn row_near(&self, ms: i64) -> i64 {
        let events = self.ed.doc.events();
        let mut best: Option<(i64, usize)> = None;
        for (i, r) in self.rows.iter().enumerate() {
            let e = &events[r.main()].event;
            if e.is_comment() || e.text.is_empty() {
                continue;
            }
            if e.start.ms() <= ms && ms <= e.end.ms() {
                return i as i64;
            }
            let d = (e.start.ms() - ms).abs();
            if best.is_none_or(|(bd, _)| d < bd) {
                best = Some((d, i));
            }
        }
        best.map_or(-1, |(_, i)| i as i64)
    }

    pub fn font_uses(&self) -> Box<FontUses> {
        Box::new(FontUses(fs_core::fonts::used_fonts(&self.ed.doc)))
    }

    pub fn resample(&mut self, spec: &ResampleSpec) {
        let matrices = !spec.from_matrix.is_empty() && !spec.to_matrix.is_empty();
        let r = ops::Resample {
            from: (spec.from_x, spec.from_y),
            to: (spec.to_x, spec.to_y),
            margin: [spec.margin_l, spec.margin_r, spec.margin_t, spec.margin_b],
            ar_mode: match spec.ar_mode {
                1 => ops::ArMode::AddBorder,
                2 => ops::ArMode::RemoveBorder,
                3 => ops::ArMode::Manual,
                _ => ops::ArMode::Stretch,
            },
            matrix: matrices.then(|| (spec.from_matrix.clone(), spec.to_matrix.clone())),
        };
        self.edit("Resample resolution", |d| ops::resample(d, &r));
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
        let style = self.style_or_default(at.saturating_sub(1));
        let evs: Vec<Event> = pasted_lines(text)
            .into_iter()
            .map(|p| p.unwrap_or_else(|l| Event::dialogue(Time(0), Time(5000), &style, l)))
            .collect();
        let ev_at = self
            .tl_row(at)
            .map(|r| r.original)
            .unwrap_or(self.ed.doc.event_count());
        let made = self.edit("Paste lines", |d| {
            d.insert_events(evs.into_iter().map(|ev| (ev_at, ev)).collect())
        });
        self.rows_of_events(&made)
    }

    /// Aegisub's "Paste lines over": clipboard line k goes onto `rows[k]`,
    /// copying only the `fields` bits (Aegisub's order: 1 comment, 2 layer,
    /// 4 start, 8 end, 16 style, 32 actor, 64/128/256 margins L/R/V,
    /// 512 effect, 1024 text). Returns the rows changed.
    pub fn paste_over(&mut self, rows: &[usize], text: &str, fields: u16) -> Vec<usize> {
        // plain text has End 0 here (Aegisub)
        let pasted: Vec<Event> = pasted_lines(text)
            .into_iter()
            .map(|p| p.unwrap_or_else(|l| Event::dialogue(Time(0), Time(0), "", l)))
            .collect();
        let n = pasted.len().min(rows.len());
        let (evs, tl) = (self.main_events(&rows[..n]), self.tl_pairs(&rows[..n]));
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
            sync_originals(d, &tl);
        });
        rows[..n].to_vec()
    }

    // ---- history ----------------------------------------------------------

    pub fn undo(&mut self) -> bool {
        let r = self.undo_step();
        self.refresh();
        r
    }

    /// One step back, a conversion's format and path with it.
    fn undo_step(&mut self) -> bool {
        if self.ed.undo_description() == Some(CONVERT)
            && let Some(prev) = self.convert_undo.pop()
        {
            let now = (self.format, self.path.take());
            (self.format, self.path) = prev;
            self.convert_redo.push(now);
        }
        self.ed.undo().is_some()
    }

    pub fn redo(&mut self) -> bool {
        if self.ed.redo_description() == Some(CONVERT)
            && let Some(next) = self.convert_redo.pop()
        {
            self.convert_undo.push((self.format, self.path.take()));
            (self.format, self.path) = next;
        }
        let r = self.ed.redo().is_some();
        self.refresh();
        r
    }

    /// The grid's selection, kept with each undo step (Aegisub's): undo brings back the one from
    /// before the edit, redo the one after it.
    pub fn set_selection(&mut self, rows: &[usize], active: i64) {
        let id = |r: usize| {
            self.tl_row(r)
                .and_then(|t| self.ed.doc.event_id(t.original))
        };
        self.ed.selection = fs_core::history::Selection {
            active: usize::try_from(active).ok().and_then(id),
            selected: rows.iter().filter_map(|&r| id(r)).collect(),
        };
    }

    /// Rows of the selection [`Session::set_selection`] kept, as undo or redo left it.
    pub fn selected_rows(&self) -> Vec<usize> {
        let ids: std::collections::HashSet<EventId> =
            self.ed.selection.selected.iter().copied().collect();
        let evs: Vec<usize> = (self.ed.doc.events().iter().enumerate())
            .filter(|(_, e)| ids.contains(&e.id))
            .map(|(i, _)| i)
            .collect();
        self.rows_of_events(&evs)
    }

    /// The active row of that selection, -1 if its line is gone.
    pub fn selected_active(&self) -> i64 {
        (self.ed.selection.active)
            .and_then(|id| self.ed.doc.index_of_id(id))
            .and_then(|e| self.row_of_event(e))
            .map_or(-1, |r| r as i64)
    }

    pub fn can_undo(&self) -> bool {
        self.ed.can_undo()
    }

    /// Aegisub's Limits/Undo Levels, kept for the files opened in this session later; the
    /// steps over it go with the next edit.
    pub fn set_undo_limit(&mut self, steps: usize) {
        self.ed.limit = steps.max(2);
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
            if !self.undo_step() {
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

    /// Aegisub's "Attach fonts to current subtitles": the files go into [Fonts] in one step,
    /// except those attached already. Returns how many went in.
    pub fn attach_fonts(&mut self, paths: &[String]) -> Result<usize, String> {
        let mut have: HashSet<String> = self
            .ed
            .doc
            .attachments()
            .into_iter()
            .filter(|a| a.font)
            .map(|a| fs_core::attachments::file_name(&a.name).to_lowercase())
            .collect();
        let mut files = Vec::new();
        for p in paths {
            let name = Path::new(p)
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            if !name.is_empty() && have.insert(name.to_lowercase()) {
                files.push((name, std::fs::read(p).map_err(|e| format!("{p}: {e}"))?));
            }
        }
        if !files.is_empty() {
            self.edit("Attach fonts", |d| {
                for (name, data) in &files {
                    d.attach(true, name, data);
                }
            });
        }
        Ok(files.len())
    }

    pub fn extract_attachment(&self, font: bool, name: &str, path: &str) -> Result<(), String> {
        let data = self
            .ed
            .doc
            .attachment_data(font, name)
            .ok_or_else(|| format!("no attachment {name}"))?;
        std::fs::write(path, data).map_err(|e| format!("{path}: {e}"))
    }

    /// An attachment's decoded bytes; empty when there is none.
    pub fn attachment_data(&self, font: bool, name: &str) -> Vec<u8> {
        self.ed.doc.attachment_data(font, name).unwrap_or_default()
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

    /// The script libass renders: in TL mode with "hide original" only the
    /// translations are shown.
    /// The subtitles for the video, taken in O(1) (the sections are shared); the render
    /// thread turns them into libass's script, so an edit never waits for a frame.
    pub fn render_subs(&self, hide_original: bool, at_ms: i64) -> Box<RenderSubs> {
        Box::new(RenderSubs {
            doc: self.ed.doc.clone(),
            tl_only: self.tl && hide_original,
            at_ms,
        })
    }

    /// The row's line alone for libass to measure: moved by the script's size into the middle of
    /// a script three times as large, so nothing of it is cut at the screen's edges, without its
    /// clips and wrapping as wide as before. Returns the script, the time in it to render, its
    /// size, where the line's anchor is in it at that time and where in the real script.
    #[allow(clippy::type_complexity)] // one caller, which takes it apart at once
    pub(crate) fn measure_script(
        &self,
        row: usize,
        at_ms: i64,
    ) -> Option<(String, i64, (i32, i32), (f64, f64), (f64, f64))> {
        let e = *self.main_events(&[row]).first()?;
        let mut doc = self.ed.doc.clone();
        let ev = doc.event(e)?.clone();
        let (w, h) = doc.play_res();
        let v = crate::visual::of_event(&doc, &ev);
        let dur = (ev.end.0 - ev.start.0).max(1);
        let at = (at_ms - ev.start.0).clamp(0, dur - 1);
        let (mut x, mut y) = (v.x, v.y);
        if v.has_move {
            // libass: no times (or none past 0) is the whole line
            let (t1, t2) = if v.t1 <= 0 && v.t2 <= 0 {
                (0, dur)
            } else {
                (v.t1, v.t2)
            };
            let u = if at <= t1 {
                0.0
            } else if at >= t2 {
                1.0
            } else {
                (at - t1) as f64 / (t2 - t1) as f64
            };
            x += (v.x2 - v.x) * u;
            y += (v.y2 - v.y) * u;
        }
        let (dx, dy) = (w as f64, h as f64);
        let mut line = Line::parse(&ops::transform_place(
            &ev.text,
            &|a, b| (a + dx, b + dy),
            false,
            (v.x, v.y),
        ));
        line.remove_tags(&["clip", "iclip"]);
        let style = doc.style(&ev.style);
        let eff = |m: i32, s: Option<i32>| if m != 0 { m } else { s.unwrap_or(0) };
        let mut one = ev.clone();
        one.text = line.to_text();
        one.kind = fs_core::EventKind::Dialogue;
        one.margin_l = eff(ev.margin_l, style.map(|s| s.margin_l)) + w;
        one.margin_r = eff(ev.margin_r, style.map(|s| s.margin_r)) + w;
        (one.start, one.end) = (Time(0), Time(dur));
        doc.retain_events(|_| false);
        let entry = doc.new_entry(one);
        doc.event_entries_mut().push(entry);
        for (k, n) in [("PlayResX", w), ("PlayResY", h)] {
            doc.set_info(k, &(3 * n).to_string());
        }
        for k in ["LayoutResX", "LayoutResY"] {
            if let Some(n) = doc.info(k).and_then(|v| v.trim().parse::<i32>().ok()) {
                doc.set_info(k, &(3 * n).to_string());
            }
        }
        Some((
            render_script(doc),
            at,
            (3 * w, 3 * h),
            (x + dx, y + dy),
            (x, y),
        ))
    }

    pub fn script_for_render(&self, hide_original: bool, at_ms: i64) -> String {
        self.render_subs(hide_original, at_ms).script()
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

    pub fn tl_shift(&mut self, first: usize, count: usize, op: u8) {
        use tlmode::Shift::*;
        let ops = [
            DeleteTranslation,
            InsertTranslation,
            JoinTranslation,
            DeleteOriginal,
            InsertOriginal,
            JoinOriginal,
        ];
        let Some(&op) = ops.get(op as usize) else {
            return;
        };
        self.edit("Move translation", |d| tlmode::shift(d, first, count, op));
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
                put_line_tags(d, e, &new);
            }
        });
    }

    /// [`set_tags`](Self::set_tags) with each row's own tags (`tags_texts[i]` for `rows[i]`), in
    /// one edit: a drag of many lines is one change per mouse move, not one per line.
    pub fn set_each_tags(&mut self, rows: &[usize], tags_texts: &[String], amend: &str) {
        let jobs: Vec<(usize, Vec<Tag>)> = rows
            .iter()
            .zip(tags_texts)
            .filter_map(|(&r, t)| Some((self.tl_row(r)?.main(), tags::parse_tags(t))))
            .collect();
        self.edit_amend(amend, "Visual typesetting", move |d| {
            for (e, new) in &jobs {
                put_line_tags(d, *e, new);
            }
        });
    }

    /// Kainote's all-tags tool: rewrite `tag` in the rows from `base` (their
    /// texts when the drag started). `mode`: 0 add, 1 set, 2 multiply,
    /// 3/4 gradient over characters up/down, 5/6 gradient over lines up/down, 7 scale (every
    /// occurrence times `delta`).
    pub fn adjust_tag(
        &mut self,
        rows: &[usize],
        base: &[String],
        tags: &str,
        mode: u8,
        deltas: &[f64],
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
            7 => TagAdjust::Scale,
            _ => TagAdjust::Add,
        };
        let evs = self.main_events(rows);
        let base = base.to_vec();
        let changes: Vec<(String, f64)> = tags
            .split(',')
            .map(str::to_string)
            .zip(deltas.iter().copied())
            .collect();
        self.edit_amend(amend, "Change tags", move |d| {
            let n = evs.len().min(base.len());
            for (k, &e) in evs.iter().take(n).enumerate() {
                let style = d.style(&d.event(e).unwrap().style).cloned();
                let text = changes.iter().fold(base[k].clone(), |text, (tag, delta)| {
                    adjust_tag(&text, style.as_ref(), tag, mode, &[*delta], k, n)
                });
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    #[allow(clippy::too_many_arguments)] // the cxx bridge passes plain numbers, no tuples
    pub fn hydra(
        &mut self,
        rows: &[usize],
        active: usize,
        base: &[String],
        tag: &str,
        mode: u8,
        values: &[f64],
        from: &[f64],
        sel_from: i64,
        sel_to: i64,
        amend: &str,
    ) {
        use fs_core::tag_adjust::{TagAdjust, adjust_tag};
        let evs = self.main_events(rows);
        let active = self.main_events(&[active]).first().copied();
        let base = base.to_vec();
        let tag = tag.to_string();
        let values = values.to_vec();
        let delta: Vec<f64> = values.iter().zip(from).map(|(v, f)| v - f).collect();
        let from = from.to_vec();
        let at = (sel_from >= 0).then(|| (sel_from as usize, sel_to.max(sel_from) as usize));
        self.edit_amend(amend, "Change tags", move |d| {
            let n = evs.len().min(base.len());
            for (k, &e) in evs.iter().take(n).enumerate() {
                let ev = d.event(e).unwrap();
                let style = d.style(&ev.style).cloned();
                let mut text = base[k].clone();
                if tag == "pos" && mode != 7 {
                    // a line without \pos moves from where it is shown (Kainote)
                    let mut placed = ev.clone();
                    placed.text = text.clone();
                    let v = crate::visual::of_event(d, &placed);
                    text = ops::transform_place(&text, &|x, y| (x, y), false, (v.x, v.y));
                }
                let (how, by) = match mode {
                    1 => (TagAdjust::Set, values.clone()),
                    2 => (TagAdjust::Multiply, delta.clone()),
                    3 | 4 => (
                        TagAdjust::GradientText {
                            increasing: mode == 3,
                        },
                        delta.clone(),
                    ),
                    5 | 6 => (
                        TagAdjust::GradientLine {
                            increasing: mode == 5,
                        },
                        delta.clone(),
                    ),
                    7 => (
                        TagAdjust::Insert {
                            at: at.filter(|_| Some(e) == active),
                        },
                        values.clone(),
                    ),
                    8 => (
                        TagAdjust::Set,
                        from.iter()
                            .zip(&delta)
                            .map(|(f, x)| f + x * k as f64)
                            .collect(),
                    ),
                    _ => (TagAdjust::Add, delta.clone()),
                };
                let text = adjust_tag(&text, style.as_ref(), &tag, how, &by, k, n);
                if d.event(e).unwrap().text != text {
                    d.event_mut(e).unwrap().text = text;
                }
            }
        });
    }

    pub fn tag_values(&self, row: usize, tag: &str, count: usize) -> Vec<f64> {
        let Some(r) = self.tl_row(row) else {
            return Vec::new();
        };
        let d = &self.ed.doc;
        let Some(ev) = d.event(r.main()) else {
            return Vec::new();
        };
        if tag == "pos" {
            let v = crate::visual::of_event(d, ev);
            return vec![v.x, v.y];
        }
        fs_core::tag_adjust::line_values(&ev.text, d.style(&ev.style), tag, count)
    }

    #[allow(clippy::too_many_arguments)] // the cxx bridge passes plain numbers, no tuples
    pub fn transform_places(
        &mut self,
        rows: &[usize],
        cx: f64,
        cy: f64,
        sx: f64,
        sy: f64,
        turn: f64,
        amend: &str,
    ) {
        let evs = self.main_events(rows);
        let (s, c) = turn.to_radians().sin_cos();
        self.edit_amend(amend, "Change tags", move |d| {
            let f = move |x: f64, y: f64| {
                let (dx, dy) = ((x - cx) * sx, (y - cy) * sy);
                (cx + dx * c + dy * s, cy - dx * s + dy * c)
            };
            for e in evs {
                let ev = d.event(e).unwrap();
                let v = crate::visual::of_event(d, ev);
                let text = fs_core::ops::transform_place(&ev.text, &f, turn != 0.0, (v.x, v.y));
                if text != ev.text {
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
            let rest = ops::MoveTargets {
                drawing: false,
                ..what
            };
            ops::translate_positions(d, &Rows::Selected(evs.clone()), dx, dy, rest);
            if !what.drawing {
                return;
            }
            // drawing points are in the line's units: the screen step unturned by \frz, unscaled
            let only = ops::MoveTargets {
                pos: false,
                move_start: false,
                move_end: false,
                clip: false,
                drawing: true,
                org: false,
            };
            for e in evs {
                let v = crate::visual::of_event(d, d.event(e).unwrap());
                if v.fscx == 0.0 || v.fscy == 0.0 {
                    continue; // nothing is shown
                }
                let (s, c) = v.frz.to_radians().sin_cos();
                let (x, y) = (dx * c - dy * s, dx * s + dy * c);
                let (x, y) = (x * 100.0 / v.fscx, y * 100.0 / v.fscy);
                ops::translate_positions(d, &Rows::Selected(vec![e]), x, y, only);
            }
        });
    }

    // ---- karaoke ----------------------------------------------------------

    fn kara(&self, row: usize) -> Option<(usize, Karaoke)> {
        let e = self.tl_row(row)?.main();
        Some((e, Karaoke::from_event(self.ed.doc.event(e)?)))
    }

    pub fn syllables(&self, row: usize) -> Vec<Syllable> {
        self.kara(row)
            .map_or_else(Vec::new, |(_, k)| bar_syllables(&k))
    }

    fn kara_edit(&mut self, row: usize, desc: &str, amend: &str, f: impl FnOnce(&mut Karaoke)) {
        let Some((e, mut k)) = self.kara(row) else {
            return;
        };
        f(&mut k);
        self.edit_amend(amend, desc, |d| k.apply(d.event_mut(e).unwrap()));
    }

    /// Split syllable `syl` at `char_pos` UTF-16 units into its visible text (the bar's).
    pub fn kara_split(&mut self, row: usize, syl: usize, char_pos: usize) {
        self.kara_edit(row, "Karaoke split", "", |k| {
            if syl < k.syls.len() {
                k.split_visible(syl, char_pos)
            }
        });
    }

    /// Karaoke mode's line times: the first syllable takes the start's change and the last one
    /// the end's, so the others keep their place against the audio (Aegisub `SetLineTimes`).
    pub fn kara_set_times(&mut self, row: usize, start: i64, end: i64, amend: &str) {
        let Some((e, mut k)) = self.kara(row) else {
            return;
        };
        if k.syls.len() > 1 {
            k.set_line_times(Time(start), Time(end));
        } else {
            (k.line_start, k.line_end) = (Time(start), Time(end.max(start)));
        }
        let single = k.syls.len() <= 1;
        self.edit_amend(amend, "Karaoke timing", |d| {
            let ev = d.event_mut(e).unwrap();
            if single {
                // no syllables to keep in place: the text stays as it is
                (ev.start, ev.end) = (k.line_start, k.line_end);
            } else {
                k.apply(ev);
            }
        });
    }

    pub fn kara_join(&mut self, row: usize, syl: usize) {
        self.kara_edit(row, "Karaoke join", "", |k| k.join(syl));
    }

    /// Move syllable boundary `index` (1..n-1) to `ms`; `shift_rest` moves
    /// the following boundaries by the same amount (right-drag). Steps with the same `amend`
    /// (one drag) make one undo step.
    pub fn kara_set_boundary(
        &mut self,
        row: usize,
        index: usize,
        ms: i64,
        shift_rest: bool,
        amend: &str,
    ) {
        self.kara_edit(row, "Karaoke timing", amend, |k| {
            if shift_rest && index > 0 && index < k.syls.len() {
                let delta = Time(ms) - k.syl_start(index);
                k.shift_markers(index, delta);
            } else {
                k.set_marker(index, Time(ms));
            }
        });
    }

    pub fn set_video_context(
        &mut self,
        w: i32,
        h: i32,
        sar_num: i32,
        sar_den: i32,
        timecodes: &[i64],
        keyframes: &[i64],
    ) {
        self.video_size = (w > 0 && h > 0).then_some((w, h));
        self.video_sar = (sar_num, sar_den);
        self.framerate = Framerate::from_timecodes(timecodes.to_vec()).ok();
        self.keyframes = keyframes.to_vec();
    }
}

/// Keep `\move`/`\t` absolute timing when only the line start moved by -`delta`.
fn shift_tag_times(ev: &mut Event, delta: i64) {
    let mut line = Line::parse(&ev.text);
    line.for_each_tag_mut(|t| {
        // the times counted from the line's start (\fad's fade-out counts from its end)
        let idx: &[usize] = match t.name.as_str() {
            "move" if t.args.len() >= 6 => &[4, 5],
            "t" if t.args.len() >= 3 => &[0, 1],
            "fad" if t.args.len() >= 2 => &[0],
            "fade" if t.args.len() >= 7 => &[3, 4, 5, 6],
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

/// The originals of translation pairs take their translations' times.
/// `evs` (in order) joined as [`Session::join_rows`]'s `mode` says, apart from any file.
fn join_apart(evs: Vec<Event>, mode: u8) -> Event {
    let last = evs.last().map(|e| e.text.clone()).unwrap_or_default();
    let jm = match mode {
        1 | 2 => JoinMode::KeepFirst,
        3 => JoinMode::AsKaraoke,
        _ => JoinMode::Concatenate { newline: false },
    };
    let mut d = Document::empty();
    let all: Vec<usize> = evs.into_iter().map(|e| d.push_event(e)).collect();
    ops::join(&mut d, &all, jm);
    let mut e = d.event(0).unwrap().clone();
    if mode == 2 {
        e.text = last;
    }
    e
}

fn sync_originals(d: &mut Document, pairs: &[(usize, usize)]) {
    for &(t, o) in pairs {
        let (Some(tr), Some(or)) = (d.event(t), d.event(o)) else {
            continue;
        };
        let times = (tr.start, tr.end);
        if (or.start, or.end) != times {
            let ev = d.event_mut(o).unwrap();
            (ev.start, ev.end) = times;
        }
    }
}

/// Kainote's "new times": a line that does not run into the next shifted one (nor ends with
/// it) lasts 110 ms per shown character, at least a second.
fn new_end_times(d: &mut Document, evs: &[usize]) {
    let mut order = evs.to_vec();
    order.sort_by_key(|&e| {
        let ev = d.event(e).unwrap();
        (ev.start, ev.end)
    });
    for w in order.windows(2) {
        let (next_start, next_end) = d.event(w[1]).map(|n| (n.start, n.end)).unwrap();
        let ev = d.event_mut(w[0]).unwrap();
        if ev.end <= next_start && ev.end != next_end {
            let chars = tags::char_count(&ev.text, true, false) as i64;
            ev.end = ev.start + Time((110 * chars).max(1000));
        }
    }
}

fn override_blocks(text: &str) -> String {
    let mut out = String::new();
    let mut rest = text;
    while let Some(a) = rest.find('{') {
        let Some(b) = rest[a..].find('}') else { break };
        let block = &rest[a..=a + b];
        if block.contains('\\') {
            out.push_str(block);
        }
        rest = &rest[a + b + 1..];
    }
    out
}

/// The clipboard's lines as Aegisub pastes them: a `Dialogue:`/`Comment:` line is an event,
/// any other line plain text for the caller to wrap.
fn pasted_lines(text: &str) -> Vec<Result<Event, &str>> {
    let fmt = EventField::default_format(AssVersion::Ass)
        .iter()
        .map(|f| f.name().to_string())
        .collect::<Vec<_>>()
        .join(", ");
    text.lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .map(|l| {
            let d = Document::parse(&format!("[Events]\nFormat: {fmt}\n{l}\n"));
            d.event(0).cloned().ok_or(l)
        })
        .collect()
}

/// Set line tags (\pos, \clip…) in the event's first block, leaving an unchanged text alone.
fn put_line_tags(d: &mut Document, e: usize, new: &[Tag]) {
    let mut line = Line::parse(&d.event(e).unwrap().text);
    for t in new {
        line.set_line_tag(t.clone());
    }
    let text = line.to_text();
    if d.event(e).unwrap().text != text {
        d.event_mut(e).unwrap().text = text;
    }
}

/// End each line no later than the next line of the same style starts.
fn fix_overlaps(d: &mut Document, evs: &[usize]) {
    // the starts by style, sorted once (only ends change here): a lookup per line instead of a scan
    let mut starts: HashMap<String, Vec<Time>> = HashMap::new();
    for o in d
        .events()
        .iter()
        .map(|x| &x.event)
        .filter(|o| !o.is_comment())
    {
        match starts.get_mut(o.style.as_str()) {
            Some(v) => v.push(o.start),
            None => drop(starts.insert(o.style.clone(), vec![o.start])),
        }
    }
    starts.values_mut().for_each(|v| v.sort_unstable());
    for &e in evs {
        let me = d.event(e).unwrap();
        let next = starts
            .get(me.style.as_str())
            .and_then(|v| v.get(v.partition_point(|&s| s <= me.start)).copied());
        if let Some(n) = next
            && me.end > n
        {
            d.event_mut(e).unwrap().end = n;
        }
    }
}

/// What [`collect_fonts`] looks for: the script's faces and the characters drawn with each.
pub struct FontUses(BTreeMap<fs_core::fonts::FontUse, BTreeSet<char>>);

impl FontUses {
    /// Another script's faces too (Kainote collects all open tabs at once).
    pub fn add(&mut self, other: &FontUses) {
        for (face, chars) in &other.0 {
            self.0.entry(face.clone()).or_default().extend(chars);
        }
    }
}

/// The system's fonts, scanned once: a scan takes seconds and they rarely change while editing.
fn system_fonts() -> fontdb::Database {
    static DB: OnceLock<fontdb::Database> = OnceLock::new();
    DB.get_or_init(|| {
        let mut db = fontdb::Database::new();
        db.load_system_fonts();
        db
    })
    .clone()
}

/// The syllables `text` would have on a line from `start` to `end` (a proposed split, before it
/// is written).
const CATALOG_HEAD: &str = "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";

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

pub fn syllables_of(text: &str, start: i64, end: i64) -> Vec<Syllable> {
    let ev = Event::dialogue(Time(start), Time(end), "", text);
    bar_syllables(&Karaoke::from_event(&ev))
}

fn bar_syllables(k: &Karaoke) -> Vec<Syllable> {
    (0..k.syls.len())
        .map(|i| Syllable {
            start: k.syl_start(i).ms(),
            end: k.syl_end(i).ms(),
            text: k.syl_display(i),
        })
        .collect()
}

pub fn collect_fonts(
    uses: &FontUses,
    dirs: Vec<String>,
    media: &str,
    dest: &str,
) -> Result<String, String> {
    use std::sync::atomic::{AtomicUsize, Ordering};
    let mut db = system_fonts();
    for d in &dirs {
        db.load_fonts_dir(d);
    }
    if media.is_empty() {
        return fonts_report(&db, uses, dest, None);
    }
    // a folder per run: two collectors may run at once
    static RUN: AtomicUsize = AtomicUsize::new(0);
    let tmp = std::env::temp_dir().join(format!(
        "fs-editor-fonts-{}-{}",
        std::process::id(),
        RUN.fetch_add(1, Ordering::Relaxed)
    ));
    let unpack = || -> Result<(), String> {
        std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
        for (name, data) in fs_media::attached_fonts(Path::new(media)).unwrap_or_default() {
            // attachment names come from the file: keep only the last path component
            let name = Path::new(&name)
                .file_name()
                .map(|n| n.to_owned())
                .unwrap_or_default();
            if !name.is_empty() {
                std::fs::write(tmp.join(name), data).map_err(|e| e.to_string())?;
            }
        }
        Ok(())
    };
    let report = unpack().and_then(|()| {
        db.load_fonts_dir(&tmp);
        fonts_report(&db, uses, dest, Some(&tmp))
    });
    let _ = std::fs::remove_dir_all(&tmp);
    report
}

/// One line per face; fonts found in `attached` (the unpacked video attachments) are named
/// by file, as that folder is gone afterwards.
fn fonts_report(
    db: &fontdb::Database,
    uses: &FontUses,
    dest: &str,
    attached: Option<&Path>,
) -> Result<String, String> {
    use std::fmt::Write;
    if !dest.is_empty() {
        std::fs::create_dir_all(dest).map_err(|e| format!("{dest}: {e}"))?;
    }
    let mut out = String::new();
    for (f, found) in fs_core::fonts::locate(db, &uses.0) {
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
        match (attached, loc.path.file_name()) {
            (Some(a), Some(name)) if loc.path.starts_with(a) => {
                let _ = write!(out, "{tag}  {face}: {} (attached)", name.to_string_lossy());
            }
            _ => {
                let _ = write!(out, "{tag}  {face}: {}", loc.path.display());
            }
        }
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

/// The parts of `a` and `b` outside their longest common subsequence of characters, as
/// (start, length) pairs in UTF-16 units (Kainote's comparison).
fn diff_spans(a: &str, b: &str) -> (Vec<usize>, Vec<usize>) {
    let (a, b): (Vec<char>, Vec<char>) = (a.chars().collect(), b.chars().collect());
    // edits are usually local: the common ends keep the table small
    let pre = a.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let suf = a[pre..]
        .iter()
        .rev()
        .zip(b[pre..].iter().rev())
        .take_while(|(x, y)| x == y)
        .count();
    let (x, y) = (&a[pre..a.len() - suf], &b[pre..b.len() - suf]);
    let (mut in_x, mut in_y) = (vec![false; x.len()], vec![false; y.len()]);
    // shortcut: longer middles (whole drawings) are marked whole; diff them in pieces if needed
    if x.len() * y.len() <= 1_000_000 {
        // t[i][j]: LCS length of x[i..] and y[j..]
        let w = y.len() + 1;
        let mut t = vec![0u32; (x.len() + 1) * w];
        for i in (0..x.len()).rev() {
            for j in (0..y.len()).rev() {
                t[i * w + j] = if x[i] == y[j] {
                    t[(i + 1) * w + j + 1] + 1
                } else {
                    t[(i + 1) * w + j].max(t[i * w + j + 1])
                };
            }
        }
        let (mut i, mut j) = (0, 0);
        while i < x.len() && j < y.len() {
            if x[i] == y[j] {
                (in_x[i], in_y[j]) = (true, true);
                (i, j) = (i + 1, j + 1);
            } else if t[(i + 1) * w + j] >= t[i * w + j + 1] {
                i += 1;
            } else {
                j += 1;
            }
        }
    }
    let spans = |s: &[char], common: &[bool]| {
        let mut out: Vec<usize> = Vec::new();
        let mut at: usize = s[..pre].iter().map(|c| c.len_utf16()).sum();
        for (c, &same) in s[pre..].iter().zip(common) {
            let n = c.len_utf16();
            if !same {
                match out.len() {
                    l if l >= 2 && out[l - 2] + out[l - 1] == at => out[l - 1] += n,
                    _ => out.extend([at, n]),
                }
            }
            at += n;
        }
        out
    };
    (spans(&a, &in_x), spans(&b, &in_y))
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
    fn sorting_lines_in_order_adds_no_step() {
        let mut s = session(ASS);
        s.sort_rows(0, false, &[]);
        assert!(!s.can_undo());
        s.sort_rows(6, false, &[]); // one, three, two
        assert!(s.can_undo());
        assert_eq!(s.row(1).text, "three");
    }

    #[test]
    fn row_near_a_time() {
        let s = session(ASS);
        assert_eq!(s.row_near(1500), 0);
        assert_eq!(s.row_near(4000), 1); // the end counts
        // between lines, the nearest start: two's at 3 s against three's at 5 s
        assert_eq!(s.row_near(3900), 1);
        assert_eq!(s.row_near(4400), 2);
        assert_eq!(s.row_near(0), 0);
        assert_eq!(s.row_near(99_000), 2);
        assert_eq!(session(&ASS.replace(",one", ",")).row_near(0), 1); // empty lines are skipped
    }

    #[test]
    fn undo_levels_hold_across_opening() {
        let mut s = Session::default();
        s.set_undo_limit(2);
        let dir = std::env::temp_dir().join(format!("fsb-undo-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("u.ass");
        std::fs::write(&p, ASS).unwrap();
        s.open(p.to_str().unwrap()).unwrap();
        for k in 0..5 {
            let mut r = s.row(0);
            r.text = format!("edit {k}");
            s.set_row(0, &r, "");
        }
        let mut undone = 0;
        while s.undo() {
            undone += 1;
        }
        assert_eq!(undone, 2);
        assert_eq!(s.row(0).text, "edit 2");
    }

    #[test]
    fn a_line_is_measured_whole_past_the_screen_edges() {
        let mut s = session(ASS);
        for (i, t) in [
            r"{\an7\pos(-50,10)\bord3\shad4\p1}m 0 0 l 100 0 100 40 0 40",
            r"{\an5\move(0,0,200,0)\bord0\clip(0,0,1,1)\p1}m 0 0 l 100 0 100 40 0 40",
        ]
        .into_iter()
        .enumerate()
        {
            let mut r = s.row(i);
            r.text = t.into();
            s.set_row(i, &r, "");
        }
        let mut r = crate::media::Renderer::new().unwrap();
        // within the pixel libass's antialiasing may add
        let near = |a: Vec<f64>, b: [f64; 10]| {
            a.len() == 10 && a.iter().zip(b).all(|(x, y)| (x - y).abs() <= 1.0)
        };
        // past the left edge, the shadow left out, the border around the text alone
        assert!(near(
            r.line_box(&s, 0, 1500),
            [-3.0, -3.0, 103.0, 43.0, 0.0, 0.0, 100.0, 40.0, -50.0, 10.0]
        ));
        // centred on wherever the move has taken it, the clip left out
        assert!(near(
            r.line_box(&s, 1, 3500),
            [
                -50.0, -20.0, 50.0, 20.0, -50.0, -20.0, 50.0, 20.0, 100.0, 0.0
            ]
        ));
    }

    #[test]
    fn scaling_all_tags_with_places_is_one_step_from_the_base() {
        let mut s = session(ASS);
        for (i, t) in [r"{\pos(100,100)}one", r"{\pos(200,100)\fscx50}two"]
            .into_iter()
            .enumerate()
        {
            let mut r = s.row(i);
            r.text = t.into();
            s.set_row(i, &r, "");
        }
        let base = vec![s.row(0).text, s.row(1).text];
        for f in [2.0, 3.0] {
            s.adjust_tag(&[0, 1], &base, "fscx,fscy", 7, &[f, f], "drag");
            s.transform_places(&[0, 1], 100.0, 100.0, f, f, 0.0, "drag");
        }
        assert_eq!(s.row(0).text, r"{\pos(100,100)\fscx300\fscy300}one");
        assert_eq!(s.row(1).text, r"{\pos(400,100)\fscx150\fscy300}two");
        s.undo();
        assert_eq!(
            [s.row(0).text, s.row(1).text],
            [base[0].clone(), base[1].clone()]
        );
    }

    #[test]
    fn edit_box_changes_reach_the_selection_in_one_step() {
        let mut s = session(ASS);
        let mut r = s.row(0);
        for t in ["a", "ab"] {
            r.text = t.into();
            s.set_row(0, &r, "edit:0:text:0");
            let v = [t.to_string(), t.to_string()];
            s.set_rows_values(&[1, 2], "text", &v, "edit:0:text:0");
        }
        s.set_rows_values(&[1, 2], "times", &["0,500".into(), "7000,8000".into()], "k");
        let row = |s: &Session, i| (s.row(i).text.to_string(), s.row(i).start, s.row(i).end);
        assert_eq!(row(&s, 1), ("ab".into(), 0, 500));
        assert_eq!(row(&s, 2), ("ab".into(), 7000, 8000));
        s.undo();
        s.undo();
        assert_eq!(row(&s, 0), ("one".into(), 1000, 2000));
        assert_eq!(row(&s, 2), ("three".into(), 5000, 6000));
    }

    #[test]
    fn undo_brings_back_the_selection_of_its_step() {
        let mut s = session(ASS);
        s.set_selection(&[1, 2], 2);
        s.delete_rows(&[1, 2]);
        s.set_selection(&[0], 0);
        s.undo();
        assert_eq!((s.selected_rows(), s.selected_active()), (vec![1, 2], 2));
        s.redo();
        assert_eq!((s.selected_rows(), s.selected_active()), (vec![0], 0));
    }

    #[test]
    fn inserted_lines_fit_between_their_neighbours() {
        let mut s = session(ASS);
        // after "one" (1-2 s), cut at "two" (3 s); before "two", from where "one" ends
        let r = s.insert_beside(0, true, 2000, 5000, true);
        assert_eq!((r, s.row(1).start, s.row(1).end), (1, 2000, 3000));
        let r = s.insert_beside(3, false, 0, 5000, true);
        assert_eq!((r, s.row(3).start, s.row(3).end), (3, 4000, 5000));
        s.delete_rows(&(0..s.row_count()).collect::<Vec<_>>());
        assert_eq!((s.row_count(), s.row(0).text.as_str()), (1, ""));
    }

    /// Lines remembered by id stay themselves when lines are added before them or translated.
    #[test]
    fn row_ids_follow_their_lines() {
        let mut s = session(ASS);
        let ids = s.row_ids(&[1, 2]);
        s.insert_row(0, 0, 500);
        assert_eq!(s.rows_of_ids(&ids), [2, 3]);
        s.tl_enable(true);
        s.paste_translation(2, "dwa");
        assert_eq!(s.rows_of_ids(&ids), [2, 3]);
        s.delete_rows(&[2]);
        assert_eq!(s.rows_of_ids(&ids), [2]);
        assert!(s.rows_of_ids(&[]).is_empty());
    }

    #[test]
    fn filter_matches_the_row_fields() {
        let mut s = session(ASS);
        s.set_rows_field(&[0], "comment", "1");
        s.set_rows_field(&[2], "style", "Other");
        s.tl_enable(true);
        s.paste_translation(1, "przekład");
        s.set_doubtful(&[1], true);
        // the old definition, row by row
        let oracle = |s: &Session, spec: &FilterSpec| -> Vec<usize> {
            (0..s.row_count())
                .filter(|&i| {
                    let r = s.row(i);
                    let hidden = spec.hide_styles.contains(&r.style)
                        || spec.hide_rows.contains(&i)
                        || (spec.hide_comments && r.comment)
                        || (spec.only_doubtful && !r.doubtful)
                        || (spec.only_untranslated && r.translated && !r.text.trim().is_empty());
                    hidden == spec.invert
                })
                .collect()
        };
        let specs = [
            FilterSpec {
                hide_comments: true,
                ..Default::default()
            },
            FilterSpec {
                hide_styles: vec!["Other".into()],
                ..Default::default()
            },
            FilterSpec {
                hide_rows: vec![2, 0],
                invert: true,
                ..Default::default()
            },
            FilterSpec {
                only_doubtful: true,
                ..Default::default()
            },
            FilterSpec {
                only_untranslated: true,
                ..Default::default()
            },
        ];
        for spec in &specs {
            assert_eq!(s.filter(spec), oracle(&s, spec), "{spec:?}");
        }
        assert!(specs.iter().any(|sp| s.filter(sp).len() < s.row_count()));
    }

    #[test]
    fn compare_pairs_lines_in_order() {
        let a = session(ASS);
        let mut b = session(ASS);
        let old = a.row(1).text;
        b.set_texts(&[1], &[format!("{old}!")], "edit");
        b.set_rows_field(&[2], "start", "5100");
        let all = [0, 1, 2];
        let pairs = |by_times| {
            a.compare(&b, &all, &all, by_times, false)
                .iter()
                .map(|p| (p.row, p.other, p.spans.clone(), p.other_spans.clone()))
                .collect::<Vec<_>>()
        };
        let one = (1, 1, vec![], vec![old.encode_utf16().count(), 1]);
        assert_eq!(pairs(true), std::slice::from_ref(&one));
        // without times the third lines pair up too; their texts are the same
        assert_eq!(pairs(false), [one]);
        // only the given rows: line 0 against line 1
        let p = a.compare(&b, &[0], &[1], false, false);
        assert_eq!((p.len(), p[0].row, p[0].other), (1, 0, 1));
        // a line without a partner is skipped and the next ones still pair; a partner before
        // the last pair is not taken, the order holds
        let mut c = session(ASS);
        c.set_rows_field(&[0], "start", "7000");
        c.set_texts(&[1, 2], &["x".into(), "y".into()], "edit");
        let rows = |p: Vec<ComparePair>| p.iter().map(|p| (p.row, p.other)).collect::<Vec<_>>();
        assert_eq!(
            rows(a.compare(&c, &all, &all, true, false)),
            [(1, 1), (2, 2)]
        );
        assert_eq!(rows(a.compare(&c, &[2, 1], &all, true, false)), [(2, 2)]);
    }

    #[test]
    fn script_info_edits() {
        let mut s = session(ASS);
        s.set_info_value("PlayResX", "1280", "res");
        s.set_info_value("PlayResY", "720", "res");
        s.undo(); // one step for both
        assert_eq!(s.ed.doc.play_res(), (1920, 1080));
        assert!(!s.ed.is_modified());
        s.set_info_quietly("PlayResX", "640");
        assert_eq!(s.info_value("PlayResX"), "640");
        assert!(!s.ed.is_modified());
    }

    #[test]
    fn shifted_drawings_follow_the_screen_step() {
        let mut s = session(ASS);
        let text = r"{\pos(0,0)\fscx200\frz90\p1}m 0 0 l 10 0 10 10";
        s.set_texts(&[0], &[text.to_string()], "edit");
        s.shift_positions(&[0], 10.0, 0.0, 16 | 1, "");
        // right on screen is down in a drawing turned by 90°; \pos moves as asked
        assert_eq!(
            s.row(0).text,
            r"{\pos(10,0)\fscx200\frz90\p1}m 0 10 l 10 10 10 20"
        );
    }

    #[test]
    fn fonts_attach_once_in_one_step() {
        let dir = std::env::temp_dir().join(format!("fsb-fonts-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let paths: Vec<String> = ["A.ttf", "b.otf"]
            .iter()
            .map(|n| {
                let p = dir.join(n);
                std::fs::write(&p, b"not really a font").unwrap();
                p.to_string_lossy().into_owned()
            })
            .collect();
        let mut s = session(ASS);
        assert_eq!(s.attach_fonts(&paths), Ok(2));
        // the same files again (in another case too) add nothing
        assert_eq!(s.attach_fonts(&paths), Ok(0));
        let names: Vec<String> = s.attachments().into_iter().map(|a| a.name).collect();
        assert_eq!(names, ["A_0.ttf", "b.otf"]);
        assert!(s.undo());
        assert!(s.attachments().is_empty());
        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn each_row_gets_its_own_tags_in_one_step() {
        let mut s = session(ASS);
        let t = [r"\pos(1,2)".to_string(), r"\pos(3,4)".to_string()];
        s.set_each_tags(&[2, 0], &t, "drag-1");
        s.set_each_tags(&[2, 0], &t, "drag-1");
        assert_eq!(s.row(2).text, r"{\pos(1,2)}three");
        assert_eq!(s.row(0).text, r"{\pos(3,4)}one");
        assert!(s.undo());
        assert_eq!(s.row(0).text, "one");
    }

    #[test]
    fn pasted_lines_go_before_the_row_in_order() {
        let mut s = session(ASS);
        let pasted = "Dialogue: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,p1\nplain p2";
        // a line that is not a subtitle line comes in as text (Aegisub)
        assert_eq!(s.paste_rows(1, pasted), [1, 2]);
        assert_eq!(s.paste_rows(9, "x\ny"), [5, 6]);
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["one", "p1", "plain p2", "two", "three", "x", "y"]);
        assert_eq!((s.row(1).start, s.row(2).end), (7000, 5000));
    }

    #[test]
    fn trees_as_kainote() {
        let mut s = session(ASS);
        // rows 0 and 2 are two runs: two trees, each with its description before it
        let heads = s.make_tree(&[2, 0], "intro");
        assert_eq!(heads, [0, 3]);
        assert_eq!(s.row_count(), 5);
        assert_eq!((s.row(0).text.as_str(), s.row(0).comment), ("intro", true));
        assert_eq!(s.row(1).markers, TREE_CLOSED);
        assert_eq!(s.tree_hidden(), [1, 4]);
        assert_eq!(s.tree_members(0), [1]);
        // a line already in a tree makes no new one
        assert!(s.make_tree(&[1], "x").is_empty());
        s.toggle_tree(3);
        assert_eq!(s.tree_hidden(), [1]);
        assert_eq!(s.row(4).markers, TREE_OPENED);
        // the actor stays the actor, other marks stay too
        let mut r = s.row(4);
        r.actor = "Ann".into();
        s.set_row(4, &r, "");
        s.remove_tree(3);
        assert_eq!(s.row_count(), 4);
        assert_eq!(
            (s.row(3).actor.as_str(), s.row(3).markers.as_str()),
            ("Ann", "")
        );
        // a closed line whose description went shows again
        s.delete_rows(&[0]);
        assert!(s.tree_hidden().is_empty());
        // lines join a tree: those above at its start, those below at its end, folded as it is
        let mut s = session(ASS);
        assert_eq!(s.make_tree(&[1], "mid"), [1]);
        assert_eq!(s.tree_add_lines(1, &[2, 0, 3]), 0); // row 2 is in the tree already
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["mid", "one", "two", "three"]);
        assert_eq!(s.tree_hidden(), [1, 2, 3]);
        assert_eq!(s.tree_add_lines(0, &[1]), -1);
        s.toggle_tree(0);
        assert!(s.tree_hidden().is_empty());
        // Kainote writes the tree mark before the bookmark
        assert_eq!(
            with_tree_state("[bookmark]Bob", Some(TREE_CLOSED)),
            "[tree_closed][bookmark]Bob"
        );
    }

    /// Translator mode: pairs join and split half by half, trees fold whole pairs.
    #[test]
    fn translator_pairs_join_split_and_fold() {
        let mut s = session(ASS);
        s.tl_enable(true);
        s.paste_translation(0, "jeden");
        s.paste_translation(2, "trzy");
        let view = |s: &Session| -> Vec<(String, String, i64, i64)> {
            (0..s.row_count())
                .map(|i| {
                    let r = s.row(i);
                    (r.original, r.text, r.start, r.end)
                })
                .collect()
        };
        let own = |o: &str, t: &str, a: i64, b: i64| (o.to_string(), t.to_string(), a, b);
        let words = |w: &[&str]| -> Vec<String> { w.iter().map(|s| s.to_string()).collect() };
        // an untranslated row in the middle: its translation is empty
        assert_eq!(s.join_rows(&[2, 0, 1], 0), 0);
        assert_eq!(view(&s), [own("one two three", "jeden trzy", 1000, 6000)]);
        assert_eq!(s.row_count(), 1);
        s.undo();
        // keep last; an untranslated first row becomes a pair
        assert_eq!(s.join_rows(&[1, 2], 2), 1);
        assert_eq!(view(&s)[1], own("three", "trzy", 3000, 6000));
        assert!(s.row(1).translated && s.row_count() == 2);
        s.undo();
        // split the translation at the cursor: the original stays whole on the first part
        assert_eq!(s.split_row(2, 3, 5500, false), 3);
        assert_eq!(
            view(&s)[2..],
            [own("three", "trz", 5000, 5500), own("", "y", 5500, 6000)]
        );
        s.undo();
        // or the original, keeping the times; an untranslated row splits its original only
        assert_eq!(s.split_row(2, 2, -2, true), 3);
        assert_eq!(
            view(&s)[2..],
            [own("th", "trzy", 5000, 6000), own("ree", "", 5000, 6000)]
        );
        assert_eq!(s.split_row(1, 1, -1, false), -1);
        s.undo();
        // words: the original stays with the first piece; frames: with every copy
        let made = s.explode_rows(
            &[2, 1],
            &[2, 2],
            &words(&["tr", "zy", "t", "wo"]),
            &[5000, 5500, 3000, 3500],
            &[5500, 6000, 3500, 4000],
        );
        assert_eq!(made, [1, 2, 3, 4]);
        assert_eq!(
            view(&s)[1..],
            [
                own("t", "", 3000, 3500),
                own("wo", "", 3500, 4000),
                own("three", "tr", 5000, 5500),
                own("", "zy", 5500, 6000)
            ]
        );
        s.undo();
        s.explode_rows(
            &[2],
            &[2],
            &words(&["trzy", "trzy"]),
            &[5000, 5500],
            &[5500, 6000],
        );
        assert_eq!(
            view(&s)[2..],
            [
                own("three", "trzy", 5000, 5500),
                own("three", "trzy", 5500, 6000)
            ]
        );
        s.undo();
        // a tree before the pair's original; both halves carry the mark
        assert_eq!(s.make_tree(&[2], "end"), [2]);
        assert_eq!(s.tree_hidden(), [3]);
        assert_eq!(
            (s.row(3).original.as_str(), s.row(3).text.as_str()),
            ("three", "trzy")
        );
        s.toggle_tree(2);
        assert!(s.tree_hidden().is_empty());
        s.toggle_tree(2);
        s.tl_enable(false);
        assert_eq!(s.tree_hidden(), [3]); // the translation kept the mark
        s.undo();
        s.remove_tree(2);
        assert_eq!(s.row_count(), 3);
        assert_eq!(s.row(2).markers, "");
    }

    #[test]
    fn continuous_times_as_aegisub() {
        let times = |s: &Session| -> Vec<(i64, i64)> {
            (0..s.row_count())
                .map(|i| (s.row(i).start, s.row(i).end))
                .collect()
        };
        // one row: its neighbour
        let mut s = session(ASS);
        s.make_continuous(&[1], false);
        s.make_continuous(&[1], true);
        assert_eq!(times(&s), [(1000, 2000), (2000, 5000), (5000, 6000)]);
        s.make_continuous(&[2], true); // the last row has no next one
        assert_eq!(times(&s)[2], (5000, 6000));
        // rows 0 and 2: not adjacent, so nothing links
        let mut s = session(ASS);
        s.make_continuous(&[0, 2], false);
        assert_eq!(times(&s), [(1000, 2000), (3000, 4000), (5000, 6000)]);
        // translator mode: rows, not the events between them, are the neighbours
        s.tl_enable(true);
        s.paste_translation(0, "jeden");
        s.paste_translation(1, "dwa");
        s.make_continuous(&[0, 1], true);
        assert_eq!((s.row(0).end, s.row(1).start), (3000, 3000));
    }

    #[test]
    fn timing_keeps_translation_pairs_together() {
        let mut s = session(ASS);
        s.tl_enable(true);
        for (r, t) in ["jeden", "dwa", "trzy"].into_iter().enumerate() {
            s.paste_translation(r, t);
        }
        let paired = |s: &Session| {
            (0..s.row_count()).all(|r| {
                let (t, d) = (s.rows[r], &s.ed.doc);
                let (o, m) = (d.event(t.original).unwrap(), d.event(t.main()).unwrap());
                t.translation.is_some() && (o.start, o.end) == (m.start, m.end)
            })
        };
        let times = |s: &Session| {
            (0..s.row_count())
                .map(|r| (s.row(r).start, s.row(r).end))
                .collect::<Vec<_>>()
        };
        s.make_continuous(&[0, 1], true);
        assert!(paired(&s));
        assert!(s.undo());
        // the gaps close between rows, not between a line's two halves
        s.post_process(
            &PostSpec {
                adjacent: 1500,
                bias: 0.5,
                rows: vec![0, 1, 2],
                ..Default::default()
            },
            &[],
            &[],
        );
        assert_eq!(times(&s), [(1000, 2500), (2500, 4500), (4500, 6000)]);
        assert!(paired(&s));
        assert!(s.undo());
        // ends pushed past the next line come back to it
        let n = s.shift(
            &ShiftSpec {
                amount: 1500,
                which: 2,
                end_fix: 1,
                ..Default::default()
            },
            &[],
        );
        assert_eq!(n, 3);
        assert_eq!(times(&s), [(1000, 3000), (3000, 5000), (5000, 7500)]);
        assert!(paired(&s));
    }

    #[test]
    fn explode_rows_splits_the_lines_in_one_step() {
        let mut s = session(ASS);
        let parts = |v: &[&str]| v.iter().map(|t| t.to_string()).collect::<Vec<_>>();
        // rows given in any order; row 1 stays
        let made = s.explode_rows(
            &[2, 0],
            &[2, 3],
            &parts(&["c1", "c2", "a1", "a2", "a3"]),
            &[5000, 5500, 1000, 1300, 1600],
            &[5500, 6000, 1300, 1600, 2000],
        );
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["a1", "a2", "a3", "two", "c1", "c2"]);
        assert_eq!(made, [0, 1, 2, 4, 5]);
        assert_eq!((s.row(4).start, s.row(5).end), (5000, 6000));
        assert!(s.undo());
        let texts: Vec<String> = (0..s.row_count()).map(|i| s.row(i).text).collect();
        assert_eq!(texts, ["one", "two", "three"]);
    }

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
    fn override_blocks_stay() {
        assert_eq!(
            override_blocks(r"{\an8}Hi {note}there{\i1}!{\p1}m 0 0 l 1 1{\p0}"),
            r"{\an8}{\i1}{\p1}{\p0}"
        );
        assert_eq!(override_blocks("plain {open"), "");
    }

    #[test]
    fn stretch_times_keeps_the_anchor() {
        let mut s = session(ASS);
        let t0: Vec<_> = (0..3).map(|i| (s.row(i).start, s.row(i).end)).collect();
        let (a, b) = (t0[0].0, t0[1].0);
        s.stretch_times(a, b, b + (b - a)); // twice as long
        let t: Vec<_> = (0..3).map(|i| (s.row(i).start, s.row(i).end)).collect();
        for (o, n) in t0.iter().zip(&t) {
            assert_eq!((n.0, n.1), (a + 2 * (o.0 - a), a + 2 * (o.1 - a)));
        }
    }

    #[test]
    fn new_end_times_follow_the_text() {
        let mut s = session(ASS);
        s.set_texts(&[1], &["a long line of text here".to_string()], "edit");
        let d = &mut s.ed.doc;
        new_end_times(d, &[0, 1, 2]);
        fix_overlaps(d, &[0, 1, 2]);
        let ends: Vec<i64> = (0..3).map(|i| d.event(i).unwrap().end.ms()).collect();
        // a second at least; 19 characters run into the next line; the last line stays
        assert_eq!(ends, [2000, 5000, 6000]);
    }

    #[test]
    fn tag_times_follow_a_start_shift() {
        let mut ev = Event {
            text: r"{\move(0,0,10,10,500,900)\fad(300,200)\fade(255,0,255,0,100,400,800)\t(100,600,\fscx120)}x".into(),
            ..Default::default()
        };
        shift_tag_times(&mut ev, -200);
        assert_eq!(
            ev.text,
            r"{\move(0,0,10,10,300,700)\fad(100,200)\fade(255,0,255,0,0,200,600)\t(0,400,\fscx120)}x"
        );
    }

    #[test]
    fn diff_spans_outside_the_common_part() {
        assert_eq!(diff_spans("abc", "abc"), (vec![], vec![]));
        assert_eq!(
            diff_spans("kot ma", "kat mam"),
            (vec![1, 1], vec![1, 1, 6, 1])
        );
        // UTF-16 units: the emoji counts 2
        assert_eq!(diff_spans("😀a", "😀b"), (vec![2, 1], vec![2, 1]));
        assert_eq!(diff_spans("", "xy"), (vec![], vec![0, 2]));
    }

    #[test]
    fn column_info_lists_distinct_values() {
        let mut s = session(ASS);
        s.set_rows_field(&[1], "actor", "Ann");
        let ci = s.column_info();
        assert_eq!(
            (ci.styles, ci.actors, ci.effects),
            (vec!["Default".to_string()], vec!["Ann".to_string()], vec![])
        );
        assert!(!ci.any_layer && !ci.any_margin_v);
        // typing keeps the cached info, a column field drops it
        let mut r = s.row(1);
        r.text += "x";
        s.set_row(1, &r, "typing");
        assert_eq!(s.column_info().actors, ["Ann"]);
        r.actor = "Bob".into();
        s.set_row(1, &r, "typing");
        assert_eq!(s.column_info().actors, ["Bob"]);
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
        // embedded fonts go to libass once, not in every script it parses
        let mut s = session(ASS);
        s.ed.doc.attach(true, "F.ttf", &[7; 300]);
        s.ed.doc.attach(false, "p.png", b"png");
        for at in [-1, 3500] {
            let text = s.script_for_render(false, at);
            assert!(
                !text.contains("[Fonts]")
                    && !text.contains("[Graphics]")
                    && text.contains("[Events]")
            );
        }
    }

    #[test]
    fn open_as_forces_the_encoding() {
        let p = std::env::temp_dir().join(format!("fsb-cp1251-{}.srt", std::process::id()));
        let (bytes, _, _) =
            encoding_rs::WINDOWS_1251.encode("1\n00:00:01,000 --> 00:00:02,000\nПривет\n");
        std::fs::write(&p, &bytes).unwrap();
        let mut s = Session::default();
        s.open_as(p.to_str().unwrap(), "windows-1251", ":", "#", false)
            .unwrap();
        assert_eq!(s.row(0).text, "Привет");
        std::fs::remove_file(p).ok();
    }

    #[test]
    fn microdvd_saves_in_the_rate_it_was_read_in() {
        let p = std::env::temp_dir().join(format!("fsb-mdvd-{}.sub", std::process::id()));
        let path = p.to_str().unwrap();
        std::fs::write(&p, "{1}{1}25\n{25}{50}a\n{77}{90}b\n").unwrap();
        let mut s = Session::default();
        s.open(path).unwrap();
        assert_eq!((s.row(0).start, s.row(1).start), (980, 3060));
        s.save(path).unwrap();
        assert_eq!(
            std::fs::read(&p).unwrap(),
            b"{1}{1}25\r\n{25}{50}a\r\n{77}{90}b\r\n"
        );
        // none named: the video's
        std::fs::write(&p, "{25}{50}a\n").unwrap();
        let mut s = Session::default();
        s.set_video_context(640, 480, 1, 1, &[0, 100, 200, 300], &[]);
        s.open(path).unwrap();
        assert_eq!(s.row(0).start, 2450); // frame 25 at 10 fps
        std::fs::remove_file(p).ok();
    }

    #[test]
    fn a_conversion_to_ass_takes_kainotes_settings() {
        let p = std::env::temp_dir().join(format!("fsb-conv-{}.srt", std::process::id()));
        std::fs::write(
            &p,
            "1\n00:00:01,000 --> 00:00:09,000\nabcd\n\n2\n00:00:03,000 --> 00:00:04,000\nxy\n",
        )
        .unwrap();
        let mut s = Session::default();
        s.open(p.to_str().unwrap()).unwrap();
        let spec = ConvertSpec {
            res_x: 1280,
            res_y: 720,
            style: "Style: Mine,Arial,40,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1".into(),
            tags: r"{e1}".into(),
            new_ends: true,
            ms_per_char: 1000,
        };
        s.convert_with("ass", &spec).unwrap();
        assert_eq!(s.file_info().play_res_x, 1280);
        assert_eq!(s.row(0).text, r"{e1}abcd");
        assert_eq!((s.row(0).end, s.row(1).end), (3000, 5000)); // 4 s cut at the next start; 2 s
        let st = s.styles();
        assert_eq!((st[0].name.as_str(), st[0].size), ("Default", 40.0));
        s.undo(); // one step back to the SRT as read
        assert_eq!((s.row(0).end, s.row(0).text.as_str()), (9000, "abcd"));
        std::fs::remove_file(p).ok();
    }

    #[test]
    fn a_conversion_saves_under_a_new_name() {
        let mut s = session(ASS);
        assert!(!s.converted() && !s.file_info().modified);
        let n = s.row_count();
        s.convert("srt").unwrap();
        // the lines are what SRT holds, the next Save asks for a name
        assert!(s.converted() && s.file_info().modified);
        assert!(s.lossless("a.srt") && s.row_count() <= n);
        s.undo();
        assert_eq!(s.row_count(), n);
        // the format goes back with the lines, and comes again with redo
        assert!(!s.converted());
        s.redo();
        assert!(s.converted());
        s.undo_to(1);
        assert!(!s.converted());
        let p = std::env::temp_dir().join(format!("fsb-conv-{}.srt", std::process::id()));
        s.convert("srt").unwrap();
        s.save(p.to_str().unwrap()).unwrap();
        assert!(!s.converted() && !s.file_info().modified);
        std::fs::remove_file(p).ok();
    }

    #[test]
    fn split_row_estimates_or_keeps_times() {
        let mut s = session(ASS);
        assert_eq!(s.split_row(2, 2, -2, false), 3);
        let (a, b) = (s.row(2), s.row(3));
        assert_eq!((a.text.as_str(), a.start, a.end), ("th", 5000, 6000));
        assert_eq!((b.text.as_str(), b.start, b.end), ("ree", 5000, 6000));
        assert_eq!(s.split_row(0, 1, -1, false), 1);
        assert_eq!((s.row(0).end, s.row(1).start), (1333, 1333));
    }

    #[test]
    fn export_copy_filters_without_touching_the_file() {
        let s = session(ASS);
        let path = s.file_info().path;
        let out = std::env::temp_dir().join(format!("fsb-export-{}.srt", std::process::id()));
        let spec = ExportSpec {
            fix_styles: true,
            fps_from: "23.976".into(),
            fps_to: "25".into(),
            encoding: "UTF-8".into(),
            ..Default::default()
        };
        s.export_copy(out.to_str().unwrap(), &spec, &[]).unwrap();
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
        let path = std::env::temp_dir().join(format!("fsb-state-{}.ass", std::process::id()));
        s.save(path.to_str().unwrap()).unwrap();
        std::fs::remove_file(&path).ok();
        assert_eq!(s.row(1).state, 2);
        // back to the loaded text in a new entry: unchanged by value
        r.text = "two".into();
        s.set_row(1, &r, "");
        assert_eq!(s.row(1).state, 0);
        assert!(s.undo());
        assert_eq!(s.row(1).text, "TWO");
        assert!(s.undo());
        assert_eq!(s.row(1).text, "two");
        assert_eq!(s.move_rows(&[2], -2), vec![0]);
        assert_eq!(s.row(0).text, "three");
        // "three" and "two" with a gap, to the end
        assert_eq!(s.move_rows(&[0, 2], 9), vec![1, 2]);
        assert_eq!(s.row(0).text, "one");
    }

    #[test]
    fn find_all_rows() {
        let s = session(ASS);
        let spec = |pattern: &str, rows: Vec<usize>| FindSpec {
            pattern: pattern.into(),
            rows,
            ..Default::default()
        };
        assert_eq!(s.find_all(&spec("o", vec![])), Ok(vec![0, 1]));
        assert_eq!(s.find_all(&spec("e", vec![2, 0, 2, 9])), Ok(vec![0, 2]));
        assert_eq!(s.find_all(&spec("x", vec![])), Ok(vec![]));
        let at = |pattern: &str, regex, max| {
            let spec = FindSpec {
                regex,
                ..spec(pattern, vec![])
            };
            (s.find_matches(&spec, max).unwrap().iter())
                .map(|m| (m.row, m.start, m.end))
                .collect::<Vec<_>>()
        };
        assert_eq!(at("e", false, 9), [(0, 2, 3), (2, 3, 4), (2, 4, 5)]);
        assert_eq!(at("e", false, 2), [(0, 2, 3), (2, 3, 4)]);
        assert_eq!(at("$", true, 9), [(0, 3, 3), (1, 3, 3), (2, 5, 5)]);
    }

    #[test]
    fn find_next_and_replace_one_match() {
        let mut s = session(ASS);
        let spec = |pattern: &str, replacement: &str, rows: Vec<usize>| FindSpec {
            pattern: pattern.into(),
            replacement: replacement.into(),
            rows,
            match_case: true,
            ..Default::default()
        };
        let hit = |row, start, end| FindHit {
            row,
            original: false,
            start,
            end,
        };
        // "one", "two", "three": match by match, through the rows and around
        let e = spec("e", "E", vec![]);
        assert_eq!(s.find_next(&e, 0, true, 0), Ok(hit(0, 2, 3)));
        assert_eq!(s.find_next(&e, 0, false, 3), Ok(hit(2, 3, 4)));
        assert_eq!(s.find_next(&e, 2, false, 4), Ok(hit(2, 4, 5)));
        assert_eq!(s.find_next(&e, 2, false, 5), Ok(hit(0, 2, 3)));
        assert_eq!(
            s.find_next(&spec("e", "", vec![2]), 0, true, 0),
            Ok(hit(2, 3, 4))
        );
        assert_eq!(
            s.find_next(&spec("", "", vec![]), 0, true, 0),
            Ok(hit(-1, 0, 0))
        );
        // Replace next takes the one shown, while it is still there
        assert_eq!(s.replace_hit(&e, &hit(2, 4, 5)), Ok(5));
        assert_eq!(s.row(2).text, "threE");
        assert_eq!(s.replace_hit(&e, &hit(2, 4, 5)), Ok(-1));
        assert!(s.undo());
        // an empty search replaces nothing (Aegisub)
        assert_eq!(s.replace_all(&spec("", "x", vec![])), Ok(0));
        assert_eq!(s.row(0).text, "one");
        // a translation row: its original first, then the translation; Replace all does both
        s.tl_enable(true);
        s.paste_translation(1, "two too");
        let o = spec("o", "0", vec![1]);
        let orig = FindHit {
            original: true,
            ..hit(1, 2, 3)
        };
        assert_eq!(s.find_next(&o, 1, true, 0), Ok(orig.clone()));
        assert_eq!(s.find_next(&o, 1, true, 3), Ok(hit(1, 2, 3)));
        assert_eq!(s.find_next(&o, 1, false, 3), Ok(hit(1, 5, 6)));
        assert_eq!(s.replace_hit(&o, &orig), Ok(3));
        assert_eq!(s.replace_all(&o), Ok(3));
        assert_eq!(
            (s.row(1).original.as_str(), s.row(1).text.as_str()),
            ("tw0", "tw0 t00")
        );
    }

    #[test]
    fn replace_rules_in_one_undo_step() {
        let mut s = session(ASS);
        let rule = |pattern: &str, replacement: &str| FindSpec {
            pattern: pattern.into(),
            replacement: replacement.into(),
            regex: true,
            skip_tags: true,
            rows: vec![0, 1],
            ..Default::default()
        };
        let bad = [rule("o", "0"), rule("(", "")];
        assert!(s.replace_rules(&bad).is_err());
        assert_eq!(s.row(0).text, "one");
        let rules = [rule(r"\bone\b", "One!"), rule("!", "?"), rule("T", "t")];
        assert_eq!(s.replace_rules(&rules), Ok(3));
        let texts = |s: &Session| (0..3).map(|i| s.row(i).text).collect::<Vec<_>>();
        assert_eq!(texts(&s), ["One?", "two", "three"]);
        assert!(s.undo());
        assert_eq!(texts(&s), ["one", "two", "three"]);
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
    fn hydra_inserts_at_the_cursor_and_steps_from_the_active_line() {
        let mut s = session(ASS);
        let base: Vec<String> = (0..3).map(|i| s.row(i).text).collect();
        // insert: at the active line's cursor, the others' starts
        s.hydra(
            &[0, 1, 2],
            1,
            &base,
            "blur",
            7,
            &[2.0],
            &[0.0],
            1,
            1,
            "drag",
        );
        let texts = |s: &Session| (0..3).map(|i| s.row(i).text).collect::<Vec<_>>();
        assert_eq!(
            texts(&s),
            [r"{\blur2}one", r"t{\blur2}wo", r"{\blur2}three"]
        );
        // step: the active line's value + n·Δ, one undo step with the insert's amend
        s.hydra(
            &[0, 1, 2],
            1,
            &base,
            "bord",
            8,
            &[3.0],
            &[2.0],
            -1,
            -1,
            "drag",
        );
        assert_eq!(
            texts(&s),
            [r"{\bord2}one", r"{\bord3}two", r"{\bord4}three"]
        );
        // a line without \pos moves from where it is shown: the style's bottom centre
        s.hydra(
            &[0],
            0,
            &base[..1],
            "pos",
            0,
            &[970.0, 1060.0],
            &[960.0, 1070.0],
            -1,
            -1,
            "pos",
        );
        assert_eq!(s.row(0).text, r"{\pos(970,1060)}one");
        assert_eq!(s.tag_values(0, "pos", 2), [970.0, 1060.0]);
        assert_eq!(s.tag_values(1, "3c", 1), [0.0, 0.0, 0.0]);
    }

    #[test]
    fn adjust_tag_rewrites_from_base_in_one_step() {
        let mut s = session(ASS);
        let base = vec![s.row(0).text];
        s.adjust_tag(&[0], &base, "bord", 1, &[3.0], "drag");
        s.adjust_tag(&[0], &base, "bord", 1, &[5.0], "drag");
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
    fn snap_to_video_keeps_the_line_whole() {
        let mut s = session(ASS);
        // starts at 2.5 s: the first line (to 2 s) now ends with the frame, the second keeps its end
        s.snap_to_video(&[0, 1], 2500, 2540, true);
        assert_eq!(
            [
                (s.row(0).start, s.row(0).end),
                (s.row(1).start, s.row(1).end)
            ],
            [(2500, 2540), (2500, 4000)]
        );
        // ends at 0.5 s: a start after it comes back
        s.snap_to_video(&[2], 460, 500, false);
        assert_eq!((s.row(2).start, s.row(2).end), (460, 500));
    }

    #[test]
    fn continuity_closes_gaps_where_the_bias_puts_them() {
        let times = |s: &Session| {
            (0..3)
                .map(|i| (s.row(i).start, s.row(i).end))
                .collect::<Vec<_>>()
        };
        // Kainote's start 250 and end 750 thresholds: gaps up to 1000, a quarter from the start
        let mut s = session(ASS);
        let spec = PostSpec {
            adjacent: 1000,
            bias: 0.75,
            rows: vec![0, 1, 2],
            ..Default::default()
        };
        s.post_process(&spec, &[], &[]);
        assert_eq!(times(&s), [(1000, 2750), (2750, 4750), (4750, 6000)]);
        let mut s = session(ASS);
        s.post_process(
            &PostSpec {
                adjacent: 999,
                ..spec
            },
            &[],
            &[],
        );
        assert_eq!(times(&s)[1], (3000, 4000));
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
        s.set_info_value("Title", "x", "");
        assert!(s.file_info().modified && s.can_undo());
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
        s.kara_set_boundary(2, 1, 5300, false, "");
        assert_eq!(s.syllables(2)[0].end, 5300);
        r.text = "{\\k20}a{\\k20}b{\\k60}c".into();
        s.set_row(2, &r, "");
        // shifting the rest keeps b's length (it used to move the wrong boundaries)
        s.kara_set_boundary(2, 1, 5300, true, "");
        assert_eq!(s.row(2).text, "{\\k30}a{\\k20}b{\\k50}c");
        // one drag, one undo step
        s.kara_set_boundary(2, 1, 5400, false, "drag");
        s.kara_set_boundary(2, 1, 5450, false, "drag");
        assert!(s.undo());
        assert_eq!(s.row(2).text, "{\\k30}a{\\k20}b{\\k50}c");
        // the line's times move only the outer syllables: b stays at 5300..5500
        s.kara_set_times(2, 4800, 6200, "");
        assert_eq!(s.row(2).text, "{\\k50}a{\\k20}b{\\k70}c");
        assert_eq!((s.row(2).start, s.row(2).end), (4800, 6200));
        // a line without karaoke keeps its text
        s.kara_set_times(1, 2900, 4000, "");
        assert_eq!((s.row(1).text.as_str(), s.row(1).start), ("two", 2900));
        // a proposed split, not yet written
        let p = syllables_of("{\\k30}ka{\\k70}ra", 1000, 2000);
        assert_eq!(
            (p[1].start, p[1].end, p[1].text.as_str()),
            (1300, 2000, "ra")
        );
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
                t / 3600,
                t / 60 % 60,
                t % 60,
                t / 3600,
                t / 60 % 60,
                t % 60
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
        time("script_for_render", &mut |s| {
            drop(std::hint::black_box(s.script_for_render(false, -1)))
        });
        time("script_for_render at", &mut |s| {
            drop(std::hint::black_box(s.script_for_render(false, 100_000)))
        });
        time("copy all", &mut |s| {
            drop(std::hint::black_box(s.copy_rows(&all)))
        });
        time("set style all", &mut |s| {
            s.set_rows_field(&all, "style", "Other")
        });
        time("sort by end", &mut |s| s.sort_rows(1, false, &[]));
        time("column_info", &mut |s| {
            drop(std::hint::black_box(s.column_info()))
        });
        time("rows_at", &mut |s| {
            drop(std::hint::black_box(s.rows_at(100_000_000)))
        });
        let retimed = session(&text.replace(".50,Default", ".60,Default"));
        time("compare, no pairs by times", &mut |s| {
            drop(std::hint::black_box(
                s.compare(&retimed, &all, &all, true, false),
            ))
        });
        time("compare, every line pairs", &mut |s| {
            drop(std::hint::black_box(
                s.compare(&retimed, &all, &all, false, false),
            ))
        });
        let find = |pattern: &str, regex: bool| FindSpec {
            pattern: pattern.into(),
            replacement: "X".into(),
            regex,
            ..Default::default()
        };
        time("find last", &mut |s| {
            drop(s.find_next(&find("number 99999 ", false), 0, false, 0))
        });
        time("find regex last", &mut |s| {
            drop(s.find_next(&find(r"9999\d ", true), 0, false, 0))
        });
        time("replace all", &mut |s| {
            drop(s.replace_all(&find("with", false)))
        });
        time("filter comments", &mut |s| {
            drop(s.filter(&FilterSpec {
                hide_comments: true,
                ..Default::default()
            }))
        });
        time("shift all", &mut |s| {
            s.shift(
                &ShiftSpec {
                    amount: 1000,
                    ..Default::default()
                },
                &[],
            );
        });
        time("undo replace", &mut |s| assert!(s.undo()));
        let half: Vec<usize> = (0..50_000).map(|i| i * 2).collect();
        time("duplicate 50k", &mut |s| drop(s.duplicate_rows(&half)));
        time("delete 50k", &mut |s| s.delete_rows(&half));
        time("move 50k", &mut |s| drop(s.move_rows(&half, 1)));
        let out = std::env::temp_dir().join(format!("fsb-bench-{}.ass", std::process::id()));
        let t0 = Instant::now();
        s.save(out.to_str().unwrap()).unwrap();
        eprintln!("save: {:?}", t0.elapsed());
        std::fs::remove_file(out).ok();
        // Ctrl+S on an SRT reads what it would write back first
        let srt = std::env::temp_dir().join(format!("fsb-bench-{}.srt", std::process::id()));
        let cues: String = (0..100_000)
            .map(|i| {
                let t = Time(i * 2000);
                let e = Time(i * 2000 + 1500);
                format!(
                    "{}\n{} --> {}\n<i>Line</i> {i}\nwith text\n\n",
                    i + 1,
                    t.to_srt(),
                    e.to_srt()
                )
            })
            .collect();
        std::fs::write(&srt, cues).unwrap();
        let mut s = Session::default();
        let t0 = Instant::now();
        s.open(srt.to_str().unwrap()).unwrap();
        eprintln!("open srt: {:?}", t0.elapsed());
        let t0 = Instant::now();
        assert!(s.lossless(srt.to_str().unwrap()));
        eprintln!("lossless srt: {:?}", t0.elapsed());
        // MPL2 italics are whole lines
        assert!(!s.lossless("a.mpl") && s.lossless("a.ass"));
        let t0 = Instant::now();
        s.save(srt.to_str().unwrap()).unwrap();
        eprintln!("save srt: {:?}", t0.elapsed());
        std::fs::remove_file(srt).ok();
    }
}
