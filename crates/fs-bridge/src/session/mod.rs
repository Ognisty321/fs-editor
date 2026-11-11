//! One editor tab: a document with undo history, its file format and the
//! translation-mode row mapping. Grid rows are logical rows: in TL mode an
//! original/translation event pair is one row, otherwise one event.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use encoding_rs::Encoding;
use fs_core::ass::SectionKind;
use fs_core::history::Editor;
use fs_core::tlmode::{self, TlRow};
use fs_core::vfr::Framerate;
use fs_core::{Document, EventEntry, EventId};
use fs_formats::{Format, LoadOptions, SaveOptions, TextImport};

use crate::ffi::{ColumnInfo, FileInfo};

mod attachments;
mod convert;
#[cfg(test)]
mod fixtures;
mod joinsplit;
mod karaoke;
mod overrides;
mod render;
mod rows;
mod search;
mod styles;
mod timing;
mod translation;
mod trees;

pub use attachments::{FontUses, collect_fonts};
pub(crate) use convert::export_rate;
pub use karaoke::syllables_of;
pub use render::RenderSubs;
pub(crate) use render::render_script;
pub use styles::{parse_style_lines, style_lines, style_with_changes};

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
    /// The amend key of a position shift being dragged, and the text of each event it moves
    /// as its first edit found it (`shift_positions`).
    shift_from: Option<(String, Vec<(usize, String)>)>,
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
            shift_from: None,
        };
        // Document::new already holds the one empty line a new tab starts with
        s.ed.mark_saved();
        s.snapshot_loaded();
        s.refresh();
        s
    }
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

/// A subtitle stream read out of a video file: the part of opening it that can take long (a file
/// with no index of the stream is read whole), so it is done away from the session and its thread.
pub struct EmbeddedSubs {
    text: String,
    ext: &'static str,
}

impl EmbeddedSubs {
    /// `progress(done, total)` returns false to cancel, which is an error.
    pub fn read(
        video: &str,
        index: i32,
        progress: impl FnMut(i64, i64) -> bool,
    ) -> Result<Self, String> {
        let how = fs_media::SubtitleRead::Auto;
        fs_media::extract_subtitles_with(Path::new(video), index, how, progress)
            .map(|(text, ext)| Self { text, ext })
            .map_err(|e| format!("{video}: {e}"))
    }
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
        let subs = EmbeddedSubs::read(video, index, |_, _| true)?;
        self.open_embedded_subs(&subs, video)
    }

    /// The stream [`EmbeddedSubs::read`] took out of `video`, as an unsaved ASS script.
    pub fn open_embedded_subs(&mut self, subs: &EmbeddedSubs, video: &str) -> Result<(), String> {
        let mut l = fs_formats::load_bytes(
            subs.text.as_bytes(),
            Some(subs.ext),
            &LoadOptions::default(),
        )
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

    /// Forget the file the subtitles came from (a recovered autosave): Save asks for a name, and
    /// they count as modified until then.
    pub fn detach(&mut self) {
        self.path = None;
        self.ed.mark_unsaved();
        self.generation += 1;
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

    pub fn row_count(&self) -> usize {
        self.rows.len()
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

    pub fn undo_description(&self) -> String {
        self.ed.undo_description().unwrap_or_default().to_string()
    }

    pub fn redo_description(&self) -> String {
        self.ed.redo_description().unwrap_or_default().to_string()
    }

    pub fn undo_to(&mut self, steps: usize) {
        for _ in 0..steps {
            if !self.undo_step() {
                break;
            }
        }
        self.refresh();
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ffi::{FilterSpec, FindSpec, ShiftSpec};
    use crate::session::fixtures::{ASS, session};
    use fs_core::Time;

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
