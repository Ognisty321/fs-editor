//! The narrow cxx interface the Qt GUI uses: opaque Rust types for a tab's
//! document (`Session`), video and subtitle rendering, audio and automation,
//! plus plain DTO structs. All editing logic stays in the Rust core.

mod automation;
mod media;
mod session;
mod spell;
mod visual;

pub use automation::Automation;
pub use media::{Audio, Renderer, Video};
pub use session::Session;
pub use spell::Speller;
use spell::new_speller;

#[cxx::bridge(namespace = "fsb")]
pub mod ffi {
    /// One grid row (in translation mode: an original/translation pair).
    #[derive(Debug, Clone, Default)]
    struct Row {
        start: i64,
        end: i64,
        layer: i32,
        style: String,
        actor: String,
        effect: String,
        margin_l: i32,
        margin_r: i32,
        margin_v: i32,
        /// Text, or the translation in TL mode.
        text: String,
        /// TL mode: the original (foreign) text.
        original: String,
        comment: bool,
        /// TL mode: "not confirmed" marker.
        doubtful: bool,
        /// TL mode: the row has a translation event.
        translated: bool,
        cps: f64,
        /// Characters per visual line, e.g. `38/12`.
        wraps: String,
        /// 0 = as loaded, 1 = modified, 2 = modified and saved.
        state: u8,
        /// Kainote actor-field prefixes (`[bookmark]` …), hidden from Actor.
        markers: String,
    }

    #[derive(Debug, Clone, Default)]
    struct FileInfo {
        path: String,
        /// `ASS`, `SRT`, …
        format: String,
        encoding: String,
        tl_mode: bool,
        modified: bool,
        play_res_x: i32,
        play_res_y: i32,
        /// `[Script Info]` keys Kainote stores for the session.
        video_file: String,
        audio_file: String,
        keyframes_file: String,
    }

    #[derive(Debug, Clone, Default)]
    struct StyleData {
        name: String,
        font: String,
        size: f64,
        /// `&HAABBGGRR` strings as in the file.
        primary: String,
        secondary: String,
        outline_color: String,
        back: String,
        bold: bool,
        italic: bool,
        underline: bool,
        strikeout: bool,
        scale_x: f64,
        scale_y: f64,
        spacing: f64,
        angle: f64,
        border_style: i32,
        outline: f64,
        shadow: f64,
        alignment: i32,
        margin_l: i32,
        margin_r: i32,
        margin_v: i32,
        encoding: i32,
    }

    /// The Shift Times panel's settings.
    #[derive(Debug, Clone, Default)]
    struct ShiftSpec {
        /// Offset in ms (or frames when `frames`); sign gives the direction.
        amount: i64,
        frames: bool,
        /// 0 both, 1 start, 2 end.
        which: u8,
        /// 0 all, 1 selected, 2 from first selected, 3 start >= active,
        /// 4 start <= active, 5 by styles.
        lines: u8,
        selected: Vec<usize>,
        active: usize,
        styles: Vec<String>,
        /// Also shift `\move`, `\t`, `\fad`-style tag times (relative to line start).
        tag_times: bool,
        /// 0 leave, 1 fix overlaps, 2 new end times from text length.
        end_fix: u8,
    }

    #[derive(Debug, Clone, Default)]
    struct FilterSpec {
        hide_styles: Vec<String>,
        hide_rows: Vec<usize>,
        hide_comments: bool,
        only_doubtful: bool,
        only_untranslated: bool,
        invert: bool,
    }

    #[derive(Debug, Clone, Default)]
    struct FindSpec {
        pattern: String,
        replacement: String,
        regex: bool,
        match_case: bool,
        skip_tags: bool,
        /// 0 text, 1 style, 2 actor, 3 effect.
        field: u8,
        include_comments: bool,
        /// Restrict to these rows (empty = all).
        rows: Vec<usize>,
    }

    #[derive(Debug, Clone, Default)]
    struct PostSpec {
        lead_in: i64,
        lead_out: i64,
        /// Gaps up to `adjacent` close at `bias` (0 previous end .. 1 next start).
        adjacent: i64,
        bias: f64,
        /// Then gaps up to this close by moving the next start back.
        adjacent_start: i64,
        kf_before_start: i64,
        kf_after_start: i64,
        kf_before_end: i64,
        kf_after_end: i64,
        snap_keyframes: bool,
        rows: Vec<usize>,
    }

    /// What the visual tools edit on a line, in script coordinates.
    #[derive(Debug, Clone, Default)]
    struct Visual {
        /// `\an` (or the style's alignment).
        align: i32,
        /// Position: `\pos`, the `\move` start, or derived from alignment/margins.
        x: f64,
        y: f64,
        has_pos: bool,
        has_move: bool,
        x2: f64,
        y2: f64,
        t1: i64,
        t2: i64,
        has_org: bool,
        org_x: f64,
        org_y: f64,
        /// 0 none, 1 rectangle, 2 vector.
        clip: u8,
        inverse: bool,
        clip_x1: f64,
        clip_y1: f64,
        clip_x2: f64,
        clip_y2: f64,
        /// Vector clip drawing (with its scale), `m 0 0 l …`.
        clip_drawing: String,
        clip_scale: i32,
        /// `\p` drawing in the text, if the line is a drawing.
        drawing: String,
        drawing_scale: i32,
        fscx: f64,
        fscy: f64,
        frz: f64,
        frx: f64,
        fry: f64,
    }

    #[derive(Debug, Clone, Default)]
    struct Syllable {
        start: i64,
        end: i64,
        text: String,
    }

    /// A font or graphic embedded in the script.
    struct AttachmentRow {
        font: bool,
        name: String,
        size: usize,
    }

    /// A text subtitle stream inside a media file.
    struct SubTrack {
        index: i32,
        codec: String,
        language: String,
        title: String,
    }

    #[derive(Debug, Clone, Default)]
    struct VideoInfo {
        width: i32,
        height: i32,
        frames: i64,
        fps: f64,
        vfr: bool,
        sar_num: i32,
        sar_den: i32,
        duration_ms: i64,
    }

    #[derive(Debug, Clone, Default)]
    struct MacroItem {
        id: usize,
        script: String,
        name: String,
        description: String,
    }

    /// The editor state handed to the C++ side after a macro ran.
    #[derive(Debug, Clone, Default)]
    struct MacroResult {
        changed: bool,
        selection: Vec<usize>,
        active: i64,
    }

    /// One `aegisub.dialog.display` control; `kind` of the value:
    /// 0 none, 1 text, 2 int, 3 float, 4 bool, 5 colour (`#RRGGBB[AA]` in `text`).
    #[derive(Debug, Clone, Default)]
    struct DialogControlData {
        class_name: String,
        name: String,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        hint: String,
        label: String,
        kind: u8,
        text: String,
        num: f64,
        flag: bool,
        items: Vec<String>,
        has_min: bool,
        min: f64,
        has_max: bool,
        max: f64,
        step: f64,
    }

    #[derive(Debug, Clone, Default)]
    struct DialogData {
        title: String,
        controls: Vec<DialogControlData>,
        /// Button labels; empty = OK and Cancel.
        buttons: Vec<String>,
        /// Ids parallel to `buttons` (`ok`, `cancel`, … or empty).
        button_ids: Vec<String>,
    }

    #[derive(Debug, Clone, Default)]
    struct DialogReply {
        /// False when closed or a `cancel` button was pressed.
        accepted: bool,
        /// Pressed button label (empty for the default OK).
        button: String,
        /// `controls` with the edited values, same order.
        controls: Vec<DialogControlData>,
    }

    unsafe extern "C++" {
        include!("fs-bridge/include/host.h");
        type AutomationHost;
        fn dialog(self: &AutomationHost, d: &DialogData) -> DialogReply;
        fn open_files(
            self: &AutomationHost,
            title: &str,
            dir: &str,
            file: &str,
            wildcard: &str,
            multiple: bool,
            must_exist: bool,
        ) -> Vec<String>;
        fn save_file(
            self: &AutomationHost,
            title: &str,
            dir: &str,
            file: &str,
            wildcard: &str,
            dont_prompt_overwrite: bool,
        ) -> String;
        fn progress(self: &AutomationHost, percent: f64);
        fn task(self: &AutomationHost, text: &str);
        fn title(self: &AutomationHost, text: &str);
        fn is_cancelled(self: &AutomationHost) -> bool;
    }

    extern "Rust" {
        type Session;
        fn new_session() -> Box<Session>;
        fn open(self: &mut Session, path: &str) -> Result<()>;
        fn open_embedded(self: &mut Session, video: &str, index: i32) -> Result<()>;
        fn save(self: &mut Session, path: &str) -> Result<()>;
        fn export_copy(
            self: &Session,
            path: &str,
            fix_styles: bool,
            fps_from: &str,
            fps_to: &str,
            video: &[i64],
            encoding: &str,
        ) -> Result<()>;
        fn save_translation(self: &Session, path: &str) -> Result<()>;
        fn autosave(self: &Session, path: &str) -> Result<()>;
        fn convert(self: &mut Session, format: &str) -> Result<()>;
        fn file_info(self: &Session) -> FileInfo;
        fn set_info_value(self: &mut Session, key: &str, value: &str);
        fn set_project_value(self: &mut Session, key: &str, value: &str);
        fn project_value(self: &Session, key: &str) -> String;
        fn info_value(self: &Session, key: &str) -> String;
        fn generation(self: &Session) -> u64;

        fn row_count(self: &Session) -> usize;
        fn row(self: &Session, i: usize) -> Row;
        fn set_row(self: &mut Session, i: usize, row: &Row, amend: &str);
        fn set_rows_field(self: &mut Session, rows: &[usize], field: &str, value: &str);
        fn insert_row(self: &mut Session, at: usize, start: i64, end: i64) -> usize;
        fn duplicate_rows(self: &mut Session, rows: &[usize]) -> Vec<usize>;
        fn delete_rows(self: &mut Session, rows: &[usize]);
        fn clear_text(self: &mut Session, rows: &[usize]);
        fn join_rows(self: &mut Session, rows: &[usize], mode: u8) -> i64;
        fn recombine_rows(self: &mut Session, rows: &[usize]) -> Vec<usize>;
        fn change_fps(self: &mut Session, from: &str, to: &str, video: &[i64]) -> Result<()>;
        fn split_by_karaoke(self: &mut Session, rows: &[usize]) -> Vec<usize>;
        fn split_row(self: &mut Session, row: usize, byte_pos: usize, at_ms: i64) -> i64;
        fn swap_rows(self: &mut Session, a: usize, b: usize);
        /// Replace a row by one copy per part (split into words, wraps, frames…).
        fn explode_row(
            self: &mut Session,
            row: usize,
            texts: &[String],
            starts: &[i64],
            ends: &[i64],
        ) -> Vec<usize>;
        fn move_rows(self: &mut Session, rows: &[usize], delta: i64) -> Vec<usize>;
        fn make_continuous(self: &mut Session, rows: &[usize], next: bool);
        fn sort_rows(self: &mut Session, key: u8, selected_only: bool, rows: &[usize]);
        fn shift(self: &mut Session, spec: &ShiftSpec, fps_timecodes: &[i64]) -> usize;
        fn post_process(self: &mut Session, spec: &PostSpec, keyframes: &[i64], timecodes: &[i64]);
        fn spec_rows(
            self: &Session,
            lines: u8,
            selected: &[usize],
            active: usize,
            styles: &[String],
        ) -> Vec<usize>;
        fn filter(self: &Session, spec: &FilterSpec) -> Vec<usize>;
        fn find(self: &Session, spec: &FindSpec, from: usize) -> Result<i64>;
        fn replace_all(self: &mut Session, spec: &FindSpec) -> Result<usize>;
        fn overlapping(self: &Session, row: usize) -> Vec<usize>;
        fn rows_at(self: &Session, ms: i64) -> Vec<usize>;
        fn resample(self: &mut Session, x: i32, y: i32);
        fn collect_fonts(
            self: &Session,
            dirs: Vec<String>,
            media: &str,
            dest: &str,
        ) -> Result<String>;
        fn copy_rows(self: &Session, rows: &[usize]) -> String;
        fn paste_rows(self: &mut Session, at: usize, text: &str) -> Vec<usize>;
        fn paste_over(self: &mut Session, rows: &[usize], text: &str, fields: u16) -> Vec<usize>;

        fn undo(self: &mut Session) -> bool;
        fn redo(self: &mut Session) -> bool;
        fn can_undo(self: &Session) -> bool;
        fn can_redo(self: &Session) -> bool;
        fn history(self: &Session) -> Vec<String>;
        fn undo_to(self: &mut Session, steps: usize);

        fn styles(self: &Session) -> Vec<StyleData>;
        fn attachments(self: &Session) -> Vec<AttachmentRow>;
        /// Embed a file into [Fonts] or [Graphics]; returns the stored name.
        fn attach_file(self: &mut Session, font: bool, path: &str) -> Result<String>;
        fn extract_attachment(self: &Session, font: bool, name: &str, path: &str) -> Result<()>;
        fn remove_attachment(self: &mut Session, font: bool, name: &str);
        /// The file name an attachment extracts to (without SSA's `_0`).
        fn attachment_file_name(name: &str) -> String;
        fn set_style(self: &mut Session, index: i64, style: &StyleData);
        fn delete_style(self: &mut Session, index: usize);
        fn clean_styles(self: &mut Session) -> Vec<String>;
        fn script_for_render(self: &Session, hide_original: bool) -> String;

        fn tl_enable(self: &mut Session, on: bool);
        fn set_doubtful(self: &mut Session, rows: &[usize], on: bool);
        fn next_doubtful(self: &Session, from: usize) -> i64;
        fn next_untranslated(self: &Session, from: usize) -> i64;
        fn paste_translation(self: &mut Session, first: usize, text: &str) -> usize;

        fn visual(self: &Session, row: usize) -> Visual;
        fn set_tags(self: &mut Session, rows: &[usize], tags: &str, amend: &str);
        fn adjust_tag(
            self: &mut Session,
            rows: &[usize],
            base: &[String],
            tag: &str,
            mode: u8,
            delta: f64,
            amend: &str,
        );
        fn remove_tags(self: &mut Session, rows: &[usize], names: &str, amend: &str);
        fn set_drawing(self: &mut Session, row: usize, drawing: &str, amend: &str);
        fn shift_positions(
            self: &mut Session,
            rows: &[usize],
            dx: f64,
            dy: f64,
            what: u8,
            amend: &str,
        );

        fn syllables(self: &Session, row: usize) -> Vec<Syllable>;
        fn kara_split(self: &mut Session, row: usize, syl: usize, char_pos: usize);
        fn kara_join(self: &mut Session, row: usize, syl: usize);
        fn kara_set_boundary(
            self: &mut Session,
            row: usize,
            index: usize,
            ms: i64,
            shift_rest: bool,
        );

        fn set_video_context(
            self: &mut Session,
            w: i32,
            h: i32,
            timecodes: &[i64],
            keyframes: &[i64],
        );

        fn fmt_time(ms: i64) -> String;
        fn parse_time(s: &str) -> i64;
        /// Tags the all-tags visual tool edits.
        fn adjustable_tags() -> Vec<String>;
        fn strip_tags(s: &str) -> String;
        /// Keyframe frame numbers from an XviD/Aegisub/… keyframes file.
        fn load_keyframes(path: &str) -> Result<Vec<i64>>;
        /// Write keyframes in Aegisub's v1 format.
        fn save_keyframes(path: &str, keyframes: &[i64]) -> Result<()>;
        /// Frame timestamps from a v1/v2 timecodes file, extended to `frames + 1` entries.
        fn load_timecodes(path: &str, frames: i64) -> Result<Vec<i64>>;
        /// Write a v2 timecodes file.
        fn save_timecodes(path: &str, timecodes: &[i64]) -> Result<()>;

        type Video;
        fn open_video(path: &str, cache_dir: &str) -> Result<Box<Video>>;
        /// Empty when the file cannot be read or has no text subtitles.
        fn subtitle_tracks(path: &str) -> Vec<SubTrack>;
        fn info(self: &Video) -> VideoInfo;
        fn frame(self: &mut Video, n: i64, out: &mut [u8]) -> Result<()>;
        fn timecodes(self: &Video) -> Vec<i64>;
        fn keyframes(self: &Video) -> Vec<i64>;
        fn frame_at_ms(self: &Video, ms: i64) -> i64;
        fn ms_at_frame(self: &Video, frame: i64) -> i64;

        type Renderer;
        fn new_renderer() -> Result<Box<Renderer>>;
        fn load_attached_fonts(self: &mut Renderer, media_path: &str);
        fn set_script(self: &mut Renderer, ass: &str) -> Result<()>;
        fn blend(
            self: &mut Renderer,
            ms: i64,
            width: usize,
            height: usize,
            video_w: usize,
            video_h: usize,
            rgba: &mut [u8],
        ) -> Result<bool>;

        type Speller;
        fn new_speller(aff_path: &str, dic_path: &str) -> Result<Box<Speller>>;
        fn misspelled(self: &Speller, text: &str) -> Vec<usize>;
        fn check(self: &Speller, word: &str) -> bool;
        fn suggest(self: &Speller, word: &str) -> Vec<String>;
        fn add_word(self: &mut Speller, word: &str);

        type Audio;
        fn open_audio(path: &str, cache_dir: &str) -> Result<Box<Audio>>;
        fn blank_audio(ms: i64) -> Box<Audio>;
        fn duration_ms(self: &Audio) -> i64;
        fn save_clip(self: &Audio, path: &str, start_ms: i64, end_ms: i64) -> Result<()>;
        fn rate(self: &Audio) -> u32;
        /// `(min, max, avg)` per column for samples from `start` at `spp` samples per column.
        fn peaks(self: &Audio, start: f64, spp: f64, out: &mut [f32]);
        fn spectrum(
            self: &Audio,
            start: f64,
            spp: f64,
            width: usize,
            height: usize,
            log: bool,
            out: &mut [f32],
        );
        fn play(self: &mut Audio, start_ms: i64, end_ms: i64) -> Result<()>;
        fn stop(self: &Audio);
        fn playing(self: &Audio) -> bool;
        fn position_ms(self: &Audio) -> i64;
        fn set_volume(self: &Audio, v: f32);

        type Automation;
        fn new_automation(data_dir: &str, user_dir: &str) -> Box<Automation>;
        /// Without a host, dialogs are answered with their defaults.
        fn set_host(self: &mut Automation, host: UniquePtr<AutomationHost>);
        fn set_cache_dir(self: &mut Automation, dir: &str);
        fn scripts(self: &Automation) -> Vec<String>;
        fn reload(self: &mut Automation) -> Vec<String>;
        fn load_autoload(self: &mut Automation) -> Vec<String>;
        fn load_script(self: &mut Automation, path: &str) -> Result<()>;
        fn macros(self: &Automation) -> Vec<MacroItem>;
        fn filters(self: &Automation) -> Vec<MacroItem>;
        /// False when the user cancelled a filter's settings.
        fn export_filtered(
            self: &Automation,
            ids: &[usize],
            session: &Session,
            path: &str,
            fix_styles: bool,
            fps_from: &str,
            fps_to: &str,
            video: &[i64],
            encoding: &str,
        ) -> Result<bool>;
        fn macro_enabled(
            self: &Automation,
            id: usize,
            session: &Session,
            rows: &[usize],
            active: i64,
        ) -> bool;
        fn run_macro(
            self: &mut Automation,
            id: usize,
            session: &mut Session,
            rows: &[usize],
            active: i64,
        ) -> Result<MacroResult>;
        fn take_log(self: &Automation) -> String;
    }
}

fn new_session() -> Box<Session> {
    Box::new(Session::default())
}

fn fmt_time(ms: i64) -> String {
    fs_core::Time(ms).to_ass()
}

/// Accepts `h:mm:ss.cc`, `h:mm:ss,mmm` and offsets like `1.5s`; -1 if invalid.
fn attachment_file_name(name: &str) -> String {
    fs_core::attachments::file_name(name)
}

fn adjustable_tags() -> Vec<String> {
    fs_core::tag_adjust::ADJUSTABLE_TAGS
        .iter()
        .map(|t| t.to_string())
        .collect()
}

fn parse_time(s: &str) -> i64 {
    let s = s.trim();
    fs_core::Time::parse_ass(s)
        .or_else(|| fs_core::Time::parse_srt(s))
        .or_else(|| fs_core::Time::parse_offset(s))
        .map_or(-1, |t| t.ms())
}

fn strip_tags(s: &str) -> String {
    fs_core::tags::strip_tags(s)
}

fn load_keyframes(path: &str) -> Result<Vec<i64>, String> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
    fs_core::keyframes::parse(&text).map_err(|e| format!("{path}: {e}"))
}

fn save_keyframes(path: &str, keyframes: &[i64]) -> Result<(), String> {
    std::fs::write(path, fs_core::keyframes::write(keyframes)).map_err(|e| format!("{path}: {e}"))
}

fn load_timecodes(path: &str, frames: i64) -> Result<Vec<i64>, String> {
    use fs_core::vfr::TimeType;
    let text = std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?;
    let f = fs_core::vfr::Framerate::parse_timecodes(&text).map_err(|e| format!("{path}: {e}"))?;
    Ok((0..=frames.max(0))
        .map(|n| f.time_at_frame(n, TimeType::Exact))
        .collect())
}

fn save_timecodes(path: &str, timecodes: &[i64]) -> Result<(), String> {
    let f =
        fs_core::vfr::Framerate::from_timecodes(timecodes.to_vec()).map_err(|e| e.to_string())?;
    std::fs::write(path, f.to_v2_string(timecodes.len())).map_err(|e| format!("{path}: {e}"))
}

fn subtitle_tracks(path: &str) -> Vec<ffi::SubTrack> {
    fs_media::subtitle_tracks(std::path::Path::new(path))
        .unwrap_or_default()
        .into_iter()
        .map(|t| ffi::SubTrack {
            index: t.index,
            codec: t.codec,
            language: t.language,
            title: t.title,
        })
        .collect()
}

fn open_video(path: &str, cache_dir: &str) -> Result<Box<Video>, String> {
    Video::open(path, cache_dir).map(Box::new)
}

fn new_renderer() -> Result<Box<Renderer>, String> {
    Renderer::new().map(Box::new)
}

fn open_audio(path: &str, cache_dir: &str) -> Result<Box<Audio>, String> {
    Audio::open(path, cache_dir).map(Box::new)
}

fn blank_audio(ms: i64) -> Box<Audio> {
    Box::new(Audio::blank(ms))
}

fn new_automation(data_dir: &str, user_dir: &str) -> Box<Automation> {
    Box::new(Automation::new(data_dir, user_dir))
}
