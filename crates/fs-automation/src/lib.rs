//! Aegisub Automation 4 compatible Lua host for FS Editor (LuaJIT via mlua).
//!
//! One Lua state per script, set up like Aegisub's (`package.path`, the
//! `.moon`-aware module loader, `include`, MoonScript, the FFI-backed
//! `aegisub.re`/`unicode`/`lfs`/`clipboard` modules) so existing scripts,
//! karaskel/kara-templater and DependencyControl run unchanged. The spec is
//! `docs/research/automation4.md`.

mod extents;
mod native;
mod regex;
mod subs;

pub use extents::text_extents;
pub use native::{Clipboard, set_clipboard};

use fs_core::Document;
use fs_core::ass::SectionKind;
use fs_core::vfr::{Framerate, TimeType};
use mlua::{
    Function, LightUserData, Lua, LuaOptions, MultiValue, RegistryKey, StdLib, Table, UserData,
    UserDataMethods, Value,
};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use subs::{Handle, Line, State};

const PRELUDE: &str = include_str!("prelude.lua");

/// Directories behind the `?user`, `?data`, … path tokens, plus the
/// automation search paths.
#[derive(Debug, Clone)]
pub struct Paths {
    pub user: PathBuf,
    pub local: PathBuf,
    /// Holds `automation/include` (the bundled modules) and `automation/autoload`.
    pub data: PathBuf,
    pub temp: PathBuf,
    pub dictionary: PathBuf,
    pub autoload: Vec<PathBuf>,
    pub include: Vec<PathBuf>,
}

impl Paths {
    /// Standard layout: `?user/automation/{autoload,include}` before the
    /// bundled `?data/automation/{autoload,include}` (Aegisub defaults).
    pub fn new(data: PathBuf, user: PathBuf) -> Paths {
        Paths {
            autoload: vec![
                user.join("automation").join("autoload"),
                data.join("automation").join("autoload"),
            ],
            include: vec![
                user.join("automation").join("include"),
                data.join("automation").join("include"),
            ],
            local: user.clone(),
            dictionary: data.join("dictionaries"),
            // no trailing separator, like boost's temp_directory_path in Aegisub
            temp: std::env::temp_dir().components().collect(),
            data,
            user,
        }
    }

    fn support_dir(&self) -> PathBuf {
        self.data.join("automation").join("include")
    }

    /// Aegisub's `decode_path`: a leading `?token` (prefix match) is replaced
    /// by its directory; unset tokens and other paths only get native
    /// separators.
    pub fn decode(&self, p: &str, ctx: Option<&Context>) -> String {
        let dir_of =
            |f: &Option<PathBuf>| f.as_ref().and_then(|f| f.parent().map(Path::to_path_buf));
        let tokens: [(&str, Option<PathBuf>); 8] = [
            ("?audio", ctx.and_then(|c| dir_of(&c.audio_file))),
            ("?data", Some(self.data.clone())),
            ("?dictionary", Some(self.dictionary.clone())),
            ("?local", Some(self.local.clone())),
            ("?script", ctx.and_then(|c| dir_of(&c.subtitle_file))),
            ("?temp", Some(self.temp.clone())),
            ("?user", Some(self.user.clone())),
            ("?video", ctx.and_then(|c| dir_of(&c.video_file))),
        ];
        let native = |s: &str| {
            if cfg!(windows) {
                s.replace('/', "\\")
            } else {
                s.to_string()
            }
        };
        if p.len() >= 5 && p.starts_with('?') {
            for (tok, dir) in &tokens {
                if let Some(rest) = p.strip_prefix(tok) {
                    let Some(dir) = dir else { return native(p) };
                    let rest = rest
                        .strip_prefix('/')
                        .or_else(|| rest.strip_prefix(std::path::MAIN_SEPARATOR))
                        .unwrap_or(rest);
                    if rest.is_empty() {
                        return dir.to_string_lossy().into_owned();
                    }
                    return native(&dir.join(rest).to_string_lossy());
                }
            }
        }
        native(p)
    }
}

/// What the scripts can see of the open project while a feature runs.
#[derive(Debug, Clone, Default)]
pub struct Context {
    pub subtitle_file: Option<PathBuf>,
    pub video_file: Option<PathBuf>,
    pub audio_file: Option<PathBuf>,
    /// `(width, height, ar_value, ar_type)` when video is open.
    pub video_size: Option<(i32, i32, f64, i32)>,
    /// Video timecodes (or a loaded timecodes file).
    pub framerate: Option<Framerate>,
    pub keyframes: Vec<i64>,
    /// Active line range in the audio timing controller, ms.
    pub audio_selection: Option<(i64, i64)>,
    /// Edit box state for arch1t3cht's `aegisub.gui` (1-based byte positions).
    pub edit: EditState,
    pub modified: bool,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct EditState {
    pub cursor: i64,
    pub sel_start: i64,
    pub sel_end: i64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum DialogValue {
    Text(String),
    Int(i64),
    Float(f64),
    Bool(bool),
    /// `#RRGGBB` or `#RRGGBBAA`.
    Color(String),
}

#[derive(Debug, Clone, PartialEq)]
pub struct DialogControl {
    /// Lowercase class: label, edit, textbox, intedit, floatedit, dropdown,
    /// checkbox, color, coloralpha, alpha.
    pub class: String,
    pub name: Option<String>,
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
    pub hint: Option<String>,
    pub label: String,
    pub value: Option<DialogValue>,
    pub items: Vec<String>,
    pub min: Option<f64>,
    pub max: Option<f64>,
    pub step: f64,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Dialog {
    pub title: String,
    pub controls: Vec<DialogControl>,
    /// Button labels; empty means the platform OK and Cancel.
    pub buttons: Vec<String>,
    /// `(id, label)`: ok, yes, save, apply, close, no, cancel, help, context_help.
    pub button_ids: Vec<(String, String)>,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct DialogResult {
    /// Label of the pressed button (`""` for the default OK); `None` when
    /// the dialog was closed or the button has the `cancel` id.
    pub button: Option<String>,
    pub values: HashMap<String, DialogValue>,
}

impl Dialog {
    /// The answer a headless host gives: the default button with the
    /// initial values.
    pub fn accept_defaults(&self) -> DialogResult {
        let button = if self.buttons.is_empty() {
            String::new()
        } else {
            self.button_ids
                .iter()
                .find(|(id, _)| matches!(id.as_str(), "ok" | "yes" | "save"))
                .map(|(_, l)| l.clone())
                .unwrap_or_else(|| self.buttons[0].clone())
        };
        let cancel = self
            .button_ids
            .iter()
            .any(|(id, l)| id == "cancel" && *l == button);
        DialogResult {
            button: (!cancel).then_some(button),
            values: self
                .controls
                .iter()
                .filter_map(|c| Some((c.name.clone()?, c.value.clone()?)))
                .collect(),
        }
    }
}

/// A decoded video frame for `aegisub.get_frame` (BGRA, `pitch` bytes per row).
pub struct VideoFrame {
    pub width: i32,
    pub height: i32,
    pub pitch: i32,
    pub flipped: bool,
    pub data: Vec<u8>,
}

/// The application side: logging, progress, dialogs. Calls come from the
/// thread running the script; a GUI marshals them to its main thread.
pub trait Host: Send + Sync {
    /// `aegisub.debug.out` / `aegisub.log` text and error reports.
    fn log(&self, text: &str) {
        eprint!("{text}");
    }
    fn progress(&self, _percent: f64) {}
    fn task(&self, _text: &str) {}
    fn title(&self, _text: &str) {}
    fn is_cancelled(&self) -> bool {
        false
    }
    fn dialog(&self, dialog: &Dialog) -> DialogResult {
        dialog.accept_defaults()
    }
    /// `aegisub.dialog.open`: `None` when cancelled.
    fn open_files(
        &self,
        _title: &str,
        _dir: &str,
        _file: &str,
        _wildcard: &str,
        _multiple: bool,
        _must_exist: bool,
    ) -> Option<Vec<String>> {
        None
    }
    fn save_file(
        &self,
        _title: &str,
        _dir: &str,
        _file: &str,
        _wildcard: &str,
        _dont_prompt_overwrite: bool,
    ) -> Option<String> {
        None
    }
    fn status_text(&self, _text: &str) {}
    fn frame(&self, _n: i64, _with_subtitles: bool) -> Option<VideoFrame> {
        None
    }
}

/// Host for tools and tests: logs to stderr, accepts dialogs with defaults.
pub struct StderrHost;
impl Host for StderrHost {}

#[derive(Clone)]
pub struct Env {
    pub paths: Paths,
    pub host: Arc<dyn Host>,
    /// `Automation/Trace Level`: `debug.out` messages above it are dropped.
    pub trace_level: i32,
}

impl Env {
    pub fn new(paths: Paths, host: Arc<dyn Host>) -> Env {
        Env {
            paths,
            host,
            trace_level: 3,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct MacroInfo {
    pub name: String,
    pub description: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct FilterInfo {
    pub name: String,
    pub description: String,
    pub priority: i64,
    pub has_config: bool,
}

#[derive(Debug, thiserror::Error)]
pub enum RunError {
    /// `aegisub.cancel()`: nothing is committed and nothing reported.
    #[error("cancelled")]
    Cancelled,
    #[error("Lua reported a runtime error:\n{0}")]
    Lua(String),
    #[error("script is not loaded")]
    NotLoaded,
}

impl From<mlua::Error> for RunError {
    fn from(e: mlua::Error) -> Self {
        RunError::Lua(e.to_string())
    }
}

pub struct MacroOutcome {
    /// Undo steps in order: one per `set_undo_point` with changes, then the
    /// remaining changes under the macro's name.
    pub commits: Vec<(String, Document)>,
    /// Selected event rows of the last document.
    pub selection: Vec<usize>,
    pub active: Option<usize>,
    /// Edit box state staged through `aegisub.gui`, if changed.
    pub edit: Option<EditState>,
}

struct Shared {
    env: Env,
    ctx: Option<Context>,
    doc: Option<Document>,
    edit: EditState,
    edit_changed: bool,
}

struct Runtime {
    lua: Lua,
    /// The native helper table handed to the prelude.
    fs: Table,
    api: RegistryKey,
    macros: RegistryKey,
    filters: RegistryKey,
}

/// mlua can't serve Rust callbacks from inside `lua_close` (its handle is
/// already gone; a DepCtrl `__gc` reaching one overflowed the stack). Collect
/// what is garbage while they still work, then unhook them so the finalizers
/// `lua_close` runs get a Lua error instead. Globals stay intact, as in Aegisub.
fn detach_rust(lua: &Lua, fs: &Table) {
    let _ = lua.gc_collect();
    let _ = fs.clear();
    if let Ok(preload) = lua
        .globals()
        .get::<Table>("package")
        .and_then(|p| p.get::<Table>("preload"))
    {
        let _ = preload.clear();
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        detach_rust(&self.lua, &self.fs);
    }
}

/// `detach_rust` for a state that failed to load; disarmed on success.
struct LoadGuard(Option<(Lua, Table)>);

impl Drop for LoadGuard {
    fn drop(&mut self) {
        if let Some((lua, fs)) = &self.0 {
            detach_rust(lua, fs);
        }
    }
}

pub struct Script {
    pub path: PathBuf,
    pub name: String,
    pub description: String,
    pub author: String,
    pub version: String,
    pub macros: Vec<MacroInfo>,
    pub filters: Vec<FilterInfo>,
    /// Load failure (syntax or runtime error at top level).
    pub error: Option<String>,
    rt: Option<Runtime>,
}

fn canonical(p: &Path) -> PathBuf {
    let c = std::fs::canonicalize(p).unwrap_or_else(|_| p.to_path_buf());
    let s = c.to_string_lossy();
    if let Some(r) = s.strip_prefix(r"\\?\UNC\") {
        return PathBuf::from(format!(r"\\{r}"));
    }
    match s.strip_prefix(r"\\?\") {
        Some(r) => PathBuf::from(r),
        None => c,
    }
}

fn lua_str(v: &Value) -> Option<String> {
    match v {
        Value::String(s) => Some(s.to_string_lossy()),
        Value::Integer(i) => Some(i.to_string()),
        Value::Number(n) => Some(fs_core::model::fmt_f64(*n)),
        _ => None,
    }
}

fn ret(lua: &Lua, v: impl mlua::IntoLuaMulti) -> mlua::Result<MultiValue> {
    let mut mv = v.into_lua_multi(lua)?;
    mv.push_front(Value::Boolean(true));
    Ok(mv)
}

fn fail(lua: &Lua, msg: impl AsRef<str>) -> mlua::Result<MultiValue> {
    (false, msg.as_ref()).into_lua_multi(lua)
}

/// Errors instead of panicking: finalizers can call back while the state is
/// closing, after mlua has dropped its app data.
fn shared(lua: &Lua) -> mlua::Result<mlua::AppDataRefMut<'_, Shared>> {
    lua.try_app_data_mut::<Shared>()
        .ok()
        .flatten()
        .ok_or_else(|| mlua::Error::runtime("automation state is unavailable"))
}

struct Frame(VideoFrame);

impl UserData for Frame {
    fn add_methods<M: UserDataMethods<Self>>(m: &mut M) {
        m.add_method("width", |_, f, ()| Ok(f.0.width));
        m.add_method("height", |_, f, ()| Ok(f.0.height));
        fn pixel(f: &VideoFrame, x: i64, y: i64) -> Option<(u8, u8, u8)> {
            if x < 0 || y < 0 || x >= f.width as i64 || y >= f.height as i64 {
                return None;
            }
            let y = if f.flipped { f.height as i64 - y } else { y };
            let o = (y * f.pitch as i64 + x * 4) as usize;
            let p = f.data.get(o..o + 3)?;
            Some((p[2], p[1], p[0]))
        }
        m.add_method("getPixel", |lua, f, (x, y): (i64, i64)| {
            match pixel(&f.0, x, y) {
                Some(p) => p.into_lua_multi(lua),
                None => Value::Nil.into_lua_multi(lua),
            }
        });
        m.add_method("getPixelFormatted", |_, f, (x, y): (i64, i64)| {
            Ok(pixel(&f.0, x, y).map(|(r, g, b)| format!("&H{b:02X}{g:02X}{r:02X}&")))
        });
        m.add_method("data", |_, f, ()| {
            Ok((
                LightUserData(f.0.data.as_ptr() as *mut _),
                f.0.pitch,
                f.0.flipped,
            ))
        });
    }
}

fn dialog_from_lua(controls: &Value, buttons: &Value, ids: &Value) -> Result<Dialog, String> {
    let Value::Table(ct) = controls else {
        return Err("Cannot create config dialog from something non-table".into());
    };
    let mut d = Dialog::default();
    for pair in ct.pairs::<Value, Value>() {
        let (_, v) = pair.map_err(|e| e.to_string())?;
        let Value::Table(t) = v else {
            return Err("bad control table entry".into());
        };
        let get = |k: &str| t.raw_get::<Value>(k).unwrap_or(Value::Nil);
        let string = |k: &str| match get(k) {
            Value::String(s) => Some(s.to_string_lossy()),
            _ => None,
        };
        let number = |k: &str| match get(k) {
            Value::Integer(i) => Some(i as f64),
            Value::Number(n) => Some(n),
            _ => None,
        };
        let int = |k: &str, def: i32| number(k).map_or(def, |n| n.trunc() as i32);
        let Some(class) = string("class").map(|c| c.to_lowercase()) else {
            return Err("bad control table entry".into());
        };
        let range = |min: Option<f64>, max: Option<f64>| match (min, max) {
            (Some(a), Some(b)) if a >= b => (None, None),
            r => r,
        };
        let mut c = DialogControl {
            class: class.clone(),
            name: string("name"),
            x: int("x", 0),
            y: int("y", 0),
            width: int("width", 1),
            height: int("height", 1),
            hint: string("hint"),
            label: string("label").unwrap_or_default(),
            value: None,
            items: Vec::new(),
            min: None,
            max: None,
            step: 0.0,
        };
        match class.as_str() {
            "label" => {}
            "edit" | "textbox" | "alpha" => {
                c.value = Some(DialogValue::Text(
                    string("text").or(string("value")).unwrap_or_default(),
                ))
            }
            "intedit" => {
                (c.min, c.max) = range(number("min"), number("max"));
                c.value = Some(DialogValue::Int(
                    number("value").unwrap_or(0.0).trunc() as i64
                ));
            }
            "floatedit" => {
                (c.min, c.max) = range(number("min"), number("max"));
                c.step = number("step").unwrap_or(0.0);
                if c.step != 0.0 && c.min.is_none() {
                    (c.min, c.max) = (Some(0.0), Some(100.0));
                }
                c.value = Some(DialogValue::Float(number("value").unwrap_or(0.0)));
            }
            "dropdown" => {
                if let Value::Table(items) = get("items") {
                    c.items = items
                        .sequence_values::<Value>()
                        .filter_map(|v| match v {
                            Ok(Value::String(s)) => Some(s.to_string_lossy()),
                            _ => None,
                        })
                        .collect();
                }
                c.value = Some(DialogValue::Text(string("value").unwrap_or_default()));
            }
            "checkbox" => {
                c.value = Some(DialogValue::Bool(matches!(
                    get("value"),
                    Value::Boolean(true)
                )))
            }
            "color" | "coloralpha" => {
                let col = subs::parse_color(&string("value").unwrap_or_default());
                c.value = Some(DialogValue::Color(if class == "color" {
                    format!("#{:02X}{:02X}{:02X}", col.r, col.g, col.b)
                } else {
                    format!("#{:02X}{:02X}{:02X}{:02X}", col.r, col.g, col.b, col.a)
                }));
            }
            _ => return Err("bad control table entry".into()),
        }
        d.controls.push(c);
    }
    if let Value::Table(b) = buttons {
        for v in b.sequence_values::<Value>() {
            match v.map_err(|e| e.to_string())? {
                Value::String(s) => d.buttons.push(s.to_string_lossy()),
                _ => return Err("Invalid button label".into()),
            }
        }
    }
    if let Value::Table(ids) = ids {
        for pair in ids.pairs::<String, String>() {
            let (id, label) = pair.map_err(|e| e.to_string())?;
            if !d.buttons.contains(&label) {
                return Err(format!("Invalid button for id {id}"));
            }
            d.button_ids.push((id.to_lowercase(), label));
        }
    }
    Ok(d)
}

fn dialog_values(lua: &Lua, values: &HashMap<String, DialogValue>) -> mlua::Result<Table> {
    let t = lua.create_table()?;
    for (k, v) in values {
        match v {
            DialogValue::Text(s) | DialogValue::Color(s) => t.set(k.as_str(), s.as_str())?,
            DialogValue::Int(i) => t.set(k.as_str(), *i)?,
            DialogValue::Float(f) => t.set(k.as_str(), *f)?,
            DialogValue::Bool(b) => t.set(k.as_str(), *b)?,
        }
    }
    Ok(t)
}

fn project_properties(lua: &Lua, doc: &Document, ctx: &Context) -> mlua::Result<Table> {
    let get = |k: &str| {
        doc.kv_get(SectionKind::ProjectGarbage, k)
            .or_else(|| doc.info(k))
            .or_else(|| doc.kv_get(SectionKind::ProjectGarbage, &format!("Aegisub {k}")))
            .unwrap_or("")
            .to_string()
    };
    let num = |k: &str| get(k).trim().parse::<f64>().unwrap_or(0.0);
    let script_dir = ctx.subtitle_file.as_ref().and_then(|f| f.parent());
    let file = |k: &str| {
        let v = get(k);
        match script_dir {
            Some(d) if !v.is_empty() && Path::new(&v).is_relative() => {
                d.join(&v).to_string_lossy().into_owned()
            }
            _ => v,
        }
    };
    let t = lua.create_table()?;
    t.set("automation_scripts", get("Automation Scripts"))?;
    t.set("export_filters", get("Export Filters"))?;
    t.set("export_encoding", get("Export Encoding"))?;
    t.set("style_storage", get("Last Style Storage"))?;
    t.set("video_zoom", num("Video Zoom Percent"))?;
    t.set("ar_value", num("Video AR Value"))?;
    t.set("scroll_position", num("Scroll Position") as i64)?;
    t.set("active_row", num("Active Line") as i64)?;
    t.set("ar_mode", num("Video AR Mode") as i64)?;
    t.set("video_position", num("Video Position") as i64)?;
    t.set("audio_file", file("Audio File"))?;
    t.set("video_file", file("Video File"))?;
    t.set("timecodes_file", file("Timecodes File"))?;
    t.set("keyframes_file", file("Keyframes File"))?;
    Ok(t)
}

/// Native helpers the prelude builds the `aegisub` table from.
fn fs_table(lua: &Lua) -> mlua::Result<Table> {
    let fs = lua.create_table()?;
    fs.set(
        "read_file",
        lua.create_function(|lua, path: String| {
            let p = canonical(Path::new(&path));
            match std::fs::read(&p) {
                Ok(b) => {
                    (lua.create_string(&b)?, p.to_string_lossy().into_owned()).into_lua_multi(lua)
                }
                Err(e) => (Value::Nil, format!("{path}: {e}")).into_lua_multi(lua),
            }
        })?,
    )?;
    fs.set(
        "file_exists",
        lua.create_function(|_, p: String| Ok(Path::new(&p).is_file()))?,
    )?;
    fs.set(
        "decode_path",
        lua.create_function(|lua, p: String| {
            let s = shared(lua)?;
            Ok(s.env.paths.decode(&p, s.ctx.as_ref()))
        })?,
    )?;
    fs.set(
        "text_extents",
        lua.create_function(|lua, (style, text): (Value, Value)| {
            let Value::Table(t) = &style else {
                return fail(lua, "Not a style entry");
            };
            match t.raw_get::<Value>("class")? {
                Value::String(c) if c.to_string_lossy().eq_ignore_ascii_case("style") => {}
                _ => return fail(lua, "Not a style entry"),
            }
            let Some(text) = lua_str(&text) else {
                return fail(lua, "bad argument #2 to 'text_extents' (string expected)");
            };
            match subs::table_style(t, None) {
                Ok(s) => ret(lua, text_extents(&s, &text)),
                Err(e) => fail(lua, e),
            }
        })?,
    )?;
    fs.set(
        "frame_from_ms",
        lua.create_function(|lua, ms: Option<f64>| {
            let s = shared(lua)?;
            Ok(s.ctx
                .as_ref()
                .and_then(|c| c.framerate.as_ref())
                .filter(|f| f.is_loaded())
                .map(|f| f.frame_at_time(ms.unwrap_or(0.0) as i64, TimeType::Start)))
        })?,
    )?;
    fs.set(
        "ms_from_frame",
        lua.create_function(|lua, n: Option<f64>| {
            let s = shared(lua)?;
            Ok(s.ctx
                .as_ref()
                .and_then(|c| c.framerate.as_ref())
                .filter(|f| f.is_loaded())
                .map(|f| f.time_at_frame(n.unwrap_or(0.0) as i64, TimeType::Start)))
        })?,
    )?;
    fs.set(
        "video_size",
        lua.create_function(|lua, ()| {
            let s = shared(lua)?;
            match s.ctx.as_ref().and_then(|c| c.video_size) {
                Some((w, h, ar, t)) => (w, h, ar, t).into_lua_multi(lua),
                None => Value::Nil.into_lua_multi(lua),
            }
        })?,
    )?;
    fs.set(
        "keyframes",
        lua.create_function(|lua, ()| {
            let s = shared(lua)?;
            match &s.ctx {
                Some(c) => Ok(Value::Table(
                    lua.create_sequence_from(c.keyframes.iter().copied())?,
                )),
                None => Ok(Value::Nil),
            }
        })?,
    )?;
    fs.set(
        "file_name",
        lua.create_function(|lua, ()| {
            let s = shared(lua)?;
            Ok(s.ctx
                .as_ref()
                .and_then(|c| c.subtitle_file.as_ref())
                .and_then(|f| f.file_name())
                .map(|n| n.to_string_lossy().into_owned()))
        })?,
    )?;
    fs.set(
        "project_properties",
        lua.create_function(|lua, ()| {
            let s = shared(lua)?;
            match (&s.ctx, &s.doc) {
                (Some(c), Some(d)) => Ok(Some(project_properties(lua, d, c)?)),
                _ => Ok(None),
            }
        })?,
    )?;
    fs.set(
        "get_audio_selection",
        lua.create_function(|lua, ()| {
            let s = shared(lua)?;
            match s.ctx.as_ref().and_then(|c| c.audio_selection) {
                Some((a, b)) => (a, b).into_lua_multi(lua),
                None => Value::Nil.into_lua_multi(lua),
            }
        })?,
    )?;
    fs.set(
        "set_status_text",
        lua.create_function(|lua, t: String| {
            let s = shared(lua)?;
            if s.ctx.is_some() {
                s.env.host.status_text(&t);
            }
            Ok(())
        })?,
    )?;
    fs.set(
        "get_frame",
        lua.create_function(|lua, (n, with_subs): (i64, bool)| {
            let host = {
                let s = shared(lua)?;
                if !s
                    .ctx
                    .as_ref()
                    .is_some_and(|c| c.framerate.as_ref().is_some_and(|f| f.is_loaded()))
                {
                    return Ok(Value::Nil);
                }
                s.env.host.clone()
            };
            match host.frame(n, with_subs) {
                Some(f) => Ok(Value::UserData(lua.create_userdata(Frame(f))?)),
                None => Ok(Value::Nil),
            }
        })?,
    )?;
    fs.set(
        "gui_get_cursor",
        lua.create_function(|lua, ()| {
            let s = shared(lua)?;
            Ok(s.ctx.as_ref().map(|_| s.edit.cursor))
        })?,
    )?;
    fs.set(
        "gui_set_cursor",
        lua.create_function(|lua, p: i64| {
            let mut s = shared(lua)?;
            if s.ctx.is_some() {
                s.edit = EditState {
                    cursor: p,
                    sel_start: p,
                    sel_end: p,
                };
                s.edit_changed = true;
            }
            Ok(())
        })?,
    )?;
    fs.set(
        "gui_get_selection",
        lua.create_function(|lua, ()| {
            let s = shared(lua)?;
            if s.ctx.is_none() {
                return Value::Nil.into_lua_multi(lua);
            }
            let (a, b) = (s.edit.sel_start, s.edit.sel_end);
            (a.min(b), a.max(b)).into_lua_multi(lua)
        })?,
    )?;
    fs.set(
        "gui_set_selection",
        lua.create_function(|lua, (a, b): (i64, i64)| {
            let mut s = shared(lua)?;
            if s.ctx.is_some() {
                s.edit = EditState {
                    cursor: b,
                    sel_start: a,
                    sel_end: b,
                };
                s.edit_changed = true;
            }
            Ok(())
        })?,
    )?;
    fs.set(
        "gui_is_modified",
        lua.create_function(|lua, ()| Ok(shared(lua)?.ctx.as_ref().map(|c| c.modified)))?,
    )?;
    fs.set(
        "trace_level",
        lua.create_function(|lua, ()| Ok(shared(lua)?.env.trace_level))?,
    )?;
    fs.set(
        "log",
        lua.create_function(|lua, t: mlua::LuaString| {
            let host = shared(lua)?.env.host.clone();
            host.log(&t.to_string_lossy());
            Ok(())
        })?,
    )?;
    fs.set(
        "progress_set",
        lua.create_function(|lua, p: f64| {
            let host = shared(lua)?.env.host.clone();
            host.progress(p);
            Ok(())
        })?,
    )?;
    fs.set(
        "progress_task",
        lua.create_function(|lua, t: String| {
            let host = shared(lua)?.env.host.clone();
            host.task(&t);
            Ok(())
        })?,
    )?;
    fs.set(
        "progress_title",
        lua.create_function(|lua, t: String| {
            let host = shared(lua)?.env.host.clone();
            host.title(&t);
            Ok(())
        })?,
    )?;
    fs.set(
        "is_cancelled",
        lua.create_function(|lua, ()| {
            let host = shared(lua)?.env.host.clone();
            Ok(host.is_cancelled())
        })?,
    )?;
    fs.set(
        "dialog_display",
        lua.create_function(|lua, (c, b, ids): (Value, Value, Value)| {
            let mut d = match dialog_from_lua(&c, &b, &ids) {
                Ok(d) => d,
                Err(e) => return fail(lua, e),
            };
            let host = shared(lua)?.env.host.clone();
            d.title = lua
                .globals()
                .get::<Option<String>>("__fs_feature_name")?
                .unwrap_or_default();
            let r = host.dialog(&d);
            let button = match r.button {
                Some(l) => Value::String(lua.create_string(&l)?),
                None => Value::Boolean(false),
            };
            ret(lua, (button, dialog_values(lua, &r.values)?))
        })?,
    )?;
    fs.set(
        "dialog_open",
        lua.create_function(
            |lua,
             (title, dir, file, wc, multiple, must_exist): (
                String,
                String,
                String,
                String,
                bool,
                bool,
            )| {
                let host = shared(lua)?.env.host.clone();
                match host.open_files(&title, &dir, &file, &wc, multiple, must_exist) {
                    None => ret(lua, Value::Nil),
                    Some(v) if multiple => ret(lua, lua.create_sequence_from(v)?),
                    Some(v) => ret(lua, v.into_iter().next()),
                }
            },
        )?,
    )?;
    fs.set(
        "dialog_save",
        lua.create_function(
            |lua, (title, dir, file, wc, dp): (String, String, String, String, bool)| {
                let host = shared(lua)?.env.host.clone();
                ret(lua, host.save_file(&title, &dir, &file, &wc, dp))
            },
        )?,
    )?;
    Ok(fs)
}

use mlua::IntoLuaMulti;

/// Outcome of `api.run`: `Ok(values)` or the error (nil = cancelled).
fn call(
    lua: &Lua,
    api: &Table,
    f: &Function,
    args: impl IntoLuaMulti,
) -> Result<Vec<Value>, RunError> {
    let run: Function = api.get("run")?;
    let mut args = args.into_lua_multi(lua)?;
    args.push_front(Value::Function(f.clone()));
    let mut out: Vec<Value> = run.call::<MultiValue>(args)?.into_iter().collect();
    if out.is_empty() {
        return Ok(out);
    }
    if matches!(out[0], Value::Boolean(true)) {
        out.remove(0);
        return Ok(out);
    }
    match out.get(1) {
        None | Some(Value::Nil) => Err(RunError::Cancelled),
        Some(v) => Err(RunError::Lua(
            lua_str(v).unwrap_or_else(|| format!("{v:?}")),
        )),
    }
}

impl Script {
    /// Load and run a script's top level. A failed load is reported in
    /// `error` (like Aegisub listing the script as broken).
    pub fn load(path: &Path, env: &Env) -> Script {
        let path = canonical(path);
        let stem = path
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default();
        let mut script = Script {
            name: path
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default(),
            path: path.clone(),
            description: String::new(),
            author: String::new(),
            version: String::new(),
            macros: Vec::new(),
            filters: Vec::new(),
            error: None,
            rt: None,
        };
        match Self::init(&path, &stem, env, &mut script) {
            Ok(rt) => script.rt = Some(rt),
            Err(e) => script.error = Some(e),
        }
        script
    }

    fn init(path: &Path, stem: &str, env: &Env, script: &mut Script) -> Result<Runtime, String> {
        // SAFETY: Aegisub scripts need the full LuaJIT library (ffi, debug, io, os).
        let lua = unsafe { Lua::unsafe_new_with(StdLib::ALL, LuaOptions::default()) };
        let e = |e: mlua::Error| e.to_string();
        lua.set_app_data(Shared {
            env: env.clone(),
            ctx: None,
            doc: None,
            edit: EditState::default(),
            edit_changed: false,
        });
        let init_clipboard = native::install(&lua).map_err(e)?;
        let script_dir = path.parent().unwrap_or(Path::new(".")).to_path_buf();
        let mut includes = vec![script_dir.to_string_lossy().into_owned()];
        for d in &env.paths.include {
            if d.is_dir() {
                includes.push(canonical(d).to_string_lossy().into_owned());
            }
        }
        let fs = fs_table(&lua).map_err(e)?;
        let mut guard = LoadGuard(Some((lua.clone(), fs.clone())));
        let api: Table = lua
            .load(PRELUDE)
            .set_name("=fs-editor prelude")
            .call((
                fs.clone(),
                path.to_string_lossy().into_owned(),
                script_dir.to_string_lossy().into_owned(),
                includes,
                env.paths.support_dir().to_string_lossy().into_owned(),
                cfg!(windows),
                init_clipboard,
                stem.to_string(),
            ))
            .map_err(e)?;

        let load: Function = api.get("load_file").map_err(e)?;
        let (f, err): (Option<Function>, Option<String>) =
            load.call(path.to_string_lossy().into_owned()).map_err(e)?;
        let Some(f) = f else {
            return Err(err.unwrap_or_default());
        };
        match call(&lua, &api, &f, ()) {
            Ok(_) => {}
            Err(RunError::Cancelled) => return Err("script cancelled while loading".into()),
            Err(err) => {
                return Err(match err {
                    RunError::Lua(m) => m,
                    o => o.to_string(),
                });
            }
        }
        let g = lua.globals();
        if g.get::<Value>("version")
            .ok()
            .and_then(|v| lua_str(&v))
            .as_deref()
            == Some("3")
        {
            return Err("Attempted to load an Automation 3 script as an Automation 4 Lua script. Automation 3 is no longer supported.".into());
        }
        let global = |k: &str| {
            g.get::<Value>(k)
                .ok()
                .and_then(|v| lua_str(&v))
                .unwrap_or_default()
        };
        if !global("script_name").is_empty() {
            script.name = global("script_name");
        }
        script.description = global("script_description");
        script.author = global("script_author");
        script.version = global("script_version");

        let take: Function = api.get("take_warnings").map_err(e)?;
        let warnings: Vec<String> = take.call(()).map_err(e)?;
        for w in warnings {
            env.host
                .log(&format!("Warning in Automation script '{stem}':\n{w}\n"));
        }

        let features: Function = api.get("features").map_err(e)?;
        let (macros, filters): (Table, Table) = features.call(()).map_err(e)?;
        for m in macros.sequence_values::<Table>() {
            let m = m.map_err(e)?;
            script.macros.push(MacroInfo {
                name: m.get("name").map_err(e)?,
                description: m.get("description").map_err(e)?,
            });
        }
        for f in filters.sequence_values::<Table>() {
            let f = f.map_err(e)?;
            script.filters.push(FilterInfo {
                name: f.get("name").map_err(e)?,
                description: f.get("description").map_err(e)?,
                priority: f.get("priority").map_err(e)?,
                has_config: f.get::<Option<Function>>("config").map_err(e)?.is_some(),
            });
        }
        let rt = Runtime {
            api: lua.create_registry_value(api).map_err(e)?,
            macros: lua.create_registry_value(macros).map_err(e)?,
            filters: lua.create_registry_value(filters).map_err(e)?,
            fs,
            lua,
        };
        guard.0 = None;
        Ok(rt)
    }

    pub fn is_loaded(&self) -> bool {
        self.rt.is_some()
    }

    /// Command id Aegisub uses for hotkeys: `automation/lua/<stem>/<name>`.
    pub fn command_id(&self, macro_index: usize) -> String {
        format!(
            "automation/lua/{}/{}",
            self.path.file_stem().unwrap_or_default().to_string_lossy(),
            self.macros[macro_index].name
        )
    }

    fn feature(
        &self,
        list: fn(&Runtime) -> &RegistryKey,
        i: usize,
        field: &str,
    ) -> Result<(&Runtime, Table, Option<Function>), RunError> {
        let rt = self.rt.as_ref().ok_or(RunError::NotLoaded)?;
        let api: Table = rt.lua.registry_value(&rt.api)?;
        let t: Table = rt.lua.registry_value(list(rt))?;
        let feat: Table = t.get(i + 1)?;
        Ok((rt, api, feat.get(field)?))
    }

    #[allow(clippy::too_many_arguments)]
    fn begin(
        &self,
        rt: &Runtime,
        api: &Table,
        ctx: &Context,
        doc: &Document,
        name: &str,
        progress: bool,
        dialog: bool,
    ) -> mlua::Result<()> {
        {
            let mut s = shared(&rt.lua)?;
            s.ctx = Some(ctx.clone());
            s.doc = Some(doc.clone());
            s.edit = ctx.edit;
            s.edit_changed = false;
        }
        rt.lua.globals().set("__fs_feature_name", name)?;
        api.get::<Function>("begin_run")?
            .call::<()>((progress, dialog))
    }

    fn end(&self, rt: &Runtime, api: &Table) {
        let _ = api
            .get::<Function>("end_run")
            .and_then(|f| f.call::<()>(()));
        if let Ok(mut s) = shared(&rt.lua) {
            s.doc = None;
        }
    }

    fn subs(rt: &Runtime, api: &Table, state: State) -> mlua::Result<(Handle, Value)> {
        let h = Handle(Arc::new(Mutex::new(Some(state))));
        let make: Function = api.get("make_subs")?;
        let obj: Value = make.call(rt.lua.create_userdata(h.clone())?)?;
        Ok((h, obj))
    }

    fn selection_args(
        lua: &Lua,
        st: &State,
        selection: &[usize],
        active: Option<usize>,
    ) -> mlua::Result<(Table, Option<i64>)> {
        let base = st.first_event_index() as i64 + 1;
        let mut sel: Vec<i64> = selection.iter().map(|&r| base + r as i64).collect();
        sel.sort_unstable();
        sel.dedup();
        Ok((
            lua.create_sequence_from(sel)?,
            active.map(|a| base + a as i64),
        ))
    }

    /// Run `validate`: `(enabled, replacement help text)`. Errors are logged
    /// and disable the macro.
    pub fn validate_macro(
        &self,
        i: usize,
        doc: &Document,
        selection: &[usize],
        active: Option<usize>,
        ctx: &Context,
    ) -> (bool, Option<String>) {
        let r = (|| -> Result<(bool, Option<String>), RunError> {
            let (rt, api, f) = self.feature(|r| &r.macros, i, "validate")?;
            let Some(f) = f else { return Ok((true, None)) };
            self.begin(rt, &api, ctx, doc, &self.macros[i].name, false, false)?;
            let st = State::new(doc, false, false);
            let (sel, act) = Self::selection_args(&rt.lua, &st, selection, active)?;
            let (h, subs) = Self::subs(rt, &api, st)?;
            let r = call(&rt.lua, &api, &f, (subs, sel, act));
            h.0.lock().unwrap_or_else(|e| e.into_inner()).take();
            self.end(rt, &api);
            let v = r?;
            let ok = v
                .first()
                .is_some_and(|v| !matches!(v, Value::Nil | Value::Boolean(false)));
            let help = v.get(1).and_then(lua_str).filter(|s| !s.is_empty());
            Ok((ok, help))
        })();
        r.unwrap_or_else(|e| {
            if !matches!(e, RunError::Cancelled) {
                self.log(&format!("{e}\n"));
            }
            (false, None)
        })
    }

    /// Run `is_active` (checked state of a toggle macro).
    pub fn macro_is_active(
        &self,
        i: usize,
        doc: &Document,
        selection: &[usize],
        active: Option<usize>,
        ctx: &Context,
    ) -> bool {
        let r = (|| -> Result<bool, RunError> {
            let (rt, api, f) = self.feature(|r| &r.macros, i, "is_active")?;
            let Some(f) = f else { return Ok(false) };
            self.begin(rt, &api, ctx, doc, &self.macros[i].name, false, false)?;
            let st = State::new(doc, false, false);
            let (sel, act) = Self::selection_args(&rt.lua, &st, selection, active)?;
            let (h, subs) = Self::subs(rt, &api, st)?;
            let r = call(&rt.lua, &api, &f, (subs, sel, act));
            h.0.lock().unwrap_or_else(|e| e.into_inner()).take();
            self.end(rt, &api);
            Ok(r?
                .first()
                .is_some_and(|v| !matches!(v, Value::Nil | Value::Boolean(false))))
        })();
        r.unwrap_or_else(|e| {
            if !matches!(e, RunError::Cancelled) {
                self.log(&format!("{e}\n"));
            }
            false
        })
    }

    fn log(&self, msg: &str) {
        if let Some(rt) = &self.rt
            && let Ok(s) = shared(&rt.lua)
        {
            let host = s.env.host.clone();
            drop(s);
            host.log(msg);
        }
    }

    /// Run a macro on `doc` with the selected event rows. Errors leave the
    /// document untouched (the caller just drops the outcome).
    pub fn run_macro(
        &self,
        i: usize,
        doc: &Document,
        selection: &[usize],
        active: Option<usize>,
        ctx: &Context,
    ) -> Result<MacroOutcome, RunError> {
        let (rt, api, f) = self.feature(|r| &r.macros, i, "process")?;
        let f = f.ok_or(RunError::NotLoaded)?;
        let name = self.macros[i].name.clone();
        self.begin(rt, &api, ctx, doc, &name, true, true)?;
        let st = State::new(doc, true, true);
        let (sel, act) = Self::selection_args(&rt.lua, &st, selection, active)?;
        let orig_sel: Vec<i64> = sel
            .clone()
            .sequence_values::<i64>()
            .collect::<mlua::Result<_>>()?;
        let orig_active_id = active.and_then(|a| doc.event_id(a));
        let (h, subs) = Self::subs(rt, &api, st)?;
        let r = call(&rt.lua, &api, &f, (subs, sel, act.unwrap_or(0)));
        let st = h.0.lock().unwrap_or_else(|e| e.into_inner()).take();
        let (edit, edit_changed) = {
            let s = shared(&rt.lua)?;
            (s.edit, s.edit_changed)
        };
        self.end(rt, &api);
        let out = match r {
            Ok(v) => v,
            Err(e) => {
                if let RunError::Lua(m) = &e {
                    self.log(&format!("\n\nLua reported a runtime error:\n{m}\n"));
                }
                return Err(e);
            }
        };
        let st = st.expect("subtitles state");

        let mut commits = Vec::new();
        for (desc, lines, info) in &st.undo_points {
            commits.push((desc.clone(), st.build(lines, *info)));
        }
        let last_modified = match st.undo_points.last() {
            Some((_, lines, _)) => !same_lines(lines, &st.lines),
            None => st.any_modified,
        };
        if last_modified {
            commits.push((name, st.build(&st.lines, st.info_touched)));
        }

        // Map absolute indices of the final vector to event rows.
        let lines = &st.lines;
        let row_of = |idx: i64| -> Option<usize> {
            let l = lines.get((idx - 1) as usize)?;
            l.is_event().then(|| {
                lines[..(idx - 1) as usize]
                    .iter()
                    .filter(|l| l.is_event())
                    .count()
            })
        };
        let final_doc = commits.last().map(|c| &c.1).unwrap_or(doc);
        let cur_active = orig_active_id.and_then(|id| final_doc.index_of_id(id));
        let mut selection = Vec::new();
        let mut new_active = cur_active;
        if let Some(Value::Table(t)) = out.first() {
            let active_idx = match out.get(1) {
                Some(Value::Integer(i)) => Some(*i),
                Some(Value::Number(n)) => Some(n.trunc() as i64),
                _ => None,
            }
            .filter(|&a| {
                let ok = row_of(a).is_some();
                if !ok {
                    self.log(&format!(
                        "Active row {a} is out of bounds (must be 1-{})\n",
                        lines.len()
                    ));
                }
                ok
            });
            let mut first = None;
            for pair in t.pairs::<Value, Value>() {
                let (_, v) = pair?;
                let idx = match v {
                    Value::Integer(i) => i,
                    Value::Number(n) => n.trunc() as i64,
                    _ => continue,
                };
                if idx < 1 || idx as usize > lines.len() {
                    self.log(&format!(
                        "Selected row {idx} is out of bounds (must be 1-{})\n",
                        lines.len()
                    ));
                    break;
                }
                let Some(row) = row_of(idx) else {
                    self.log(&format!("Selected row {idx} is not a dialogue line\n"));
                    break;
                };
                first.get_or_insert(row);
                selection.push(row);
            }
            if let Some(a) = active_idx
                .and_then(row_of)
                .filter(|r| selection.contains(r))
            {
                new_active = Some(a);
            } else if !cur_active.is_some_and(|c| selection.contains(&c)) && first.is_some() {
                new_active = first;
            }
            if selection.is_empty() {
                selection.extend(new_active);
            }
        } else {
            for idx in orig_sel {
                if idx as usize > lines.len() {
                    break;
                }
                if let Some(r) = row_of(idx) {
                    selection.push(r);
                }
            }
            if selection.is_empty() && lines.iter().any(Line::is_event) {
                selection.push(0);
            }
            if !new_active.is_some_and(|a| selection.contains(&a)) {
                new_active = selection.first().copied();
            }
        }
        selection.sort_unstable();
        selection.dedup();
        Ok(MacroOutcome {
            commits,
            selection,
            active: new_active,
            edit: edit_changed.then_some(edit),
        })
    }

    /// Controls of a filter's configuration panel (no buttons).
    pub fn filter_config(
        &self,
        i: usize,
        doc: &Document,
        ctx: &Context,
    ) -> Result<Option<Dialog>, RunError> {
        let (rt, api, f) = self.feature(|r| &r.filters, i, "config")?;
        let Some(f) = f else { return Ok(None) };
        self.begin(rt, &api, ctx, doc, &self.filters[i].name, false, false)?;
        let (h, subs) = Self::subs(rt, &api, State::new(doc, false, false))?;
        let r = call(&rt.lua, &api, &f, (subs, rt.lua.create_table()?));
        h.0.lock().unwrap_or_else(|e| e.into_inner()).take();
        self.end(rt, &api);
        let v = r?;
        let controls = v.into_iter().next().unwrap_or(Value::Nil);
        dialog_from_lua(&controls, &Value::Nil, &Value::Nil)
            .map(|mut d| {
                d.title = self.filters[i].name.clone();
                Some(d)
            })
            .map_err(RunError::Lua)
    }

    /// Run an export filter on (a copy of) the document.
    pub fn run_filter(
        &self,
        i: usize,
        doc: &Document,
        settings: &HashMap<String, DialogValue>,
        ctx: &Context,
    ) -> Result<Document, RunError> {
        let (rt, api, f) = self.feature(|r| &r.filters, i, "process")?;
        let f = f.ok_or(RunError::NotLoaded)?;
        self.begin(rt, &api, ctx, doc, &self.filters[i].name, true, false)?;
        let (h, subs) = Self::subs(rt, &api, State::new(doc, true, false))?;
        let r = call(&rt.lua, &api, &f, (subs, dialog_values(&rt.lua, settings)?));
        let st = h.0.lock().unwrap_or_else(|e| e.into_inner()).take();
        self.end(rt, &api);
        if let Err(RunError::Lua(m)) = &r {
            self.log(&format!("\n\nLua reported a runtime error:\n{m}\n"));
        }
        r?;
        let st = st.expect("subtitles state");
        Ok(if st.any_modified {
            st.build(&st.lines, st.info_touched)
        } else {
            doc.clone()
        })
    }

    /// Execute a Lua chunk in the script's state (tests and the CLI).
    pub fn eval(&self, code: &str) -> Result<Vec<String>, RunError> {
        let rt = self.rt.as_ref().ok_or(RunError::NotLoaded)?;
        let api: Table = rt.lua.registry_value(&rt.api)?;
        let f = rt.lua.load(code).set_name("=eval").into_function()?;
        Ok(call(&rt.lua, &api, &f, ())?
            .iter()
            .map(|v| lua_str(v).unwrap_or_else(|| format!("{v:?}")))
            .collect())
    }
}

fn same_lines(a: &[Line], b: &[Line]) -> bool {
    a.len() == b.len()
        && a.iter().zip(b).all(|(x, y)| match (x, y) {
            (
                Line::Info {
                    key: k1, value: v1, ..
                },
                Line::Info {
                    key: k2, value: v2, ..
                },
            ) => k1 == k2 && v1 == v2,
            (
                Line::Style {
                    base: b1,
                    style: s1,
                },
                Line::Style {
                    base: b2,
                    style: s2,
                },
            ) => s1 == s2 && b1.as_ref().map(Arc::as_ptr) == b2.as_ref().map(Arc::as_ptr),
            (
                Line::Event {
                    base: b1,
                    event: e1,
                },
                Line::Event {
                    base: b2,
                    event: e2,
                },
            ) => e1 == e2 && b1.as_ref().map(Arc::as_ptr) == b2.as_ref().map(Arc::as_ptr),
            _ => false,
        })
}

/// Scripts in the autoload directories (`.lua` and `.moon`, not recursive),
/// sorted by file name within each directory, duplicates removed.
pub fn autoload_files(paths: &Paths) -> Vec<PathBuf> {
    let mut out: Vec<PathBuf> = Vec::new();
    for dir in &paths.autoload {
        let Ok(rd) = std::fs::read_dir(dir) else {
            continue;
        };
        let mut files: Vec<PathBuf> = rd
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.is_file()
                    && p.extension().is_some_and(|e| {
                        e.eq_ignore_ascii_case("lua") || e.eq_ignore_ascii_case("moon")
                    })
            })
            .collect();
        files.sort();
        for f in files {
            let c = canonical(&f);
            if !out.contains(&c) {
                out.push(c);
            }
        }
    }
    out
}
