//! Automation 4 scripts for the GUI: autoload, the macro list, running a
//! macro on a session (each script undo point becomes an undo step) and
//! export filters. Scripts run on a worker thread (`run_script_job`).

use std::collections::HashMap;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use cxx::UniquePtr;
use fs_automation::{
    Clipboard, Context, Dialog, DialogControl, DialogResult, DialogValue, EditState, Env, Host,
    Interrupt, MacroOutcome, Paths, RunError, Script, VideoFrame, canonical, inline_string_encode,
    serialise_settings,
};
use fs_core::Document;
use fs_core::ops;
use fs_core::vfr::Framerate;

use crate::Session;
use crate::ffi::{
    AutomationHost, DialogControlData, DialogData, ExportSpec, FilterItem, LocalScript,
    MacroContext, MacroItem, MacroResult, MacroState, ScriptItem,
};
use crate::media::{Renderer, Video};

struct CppHost(UniquePtr<AutomationHost>);
// SAFETY: a script calls the host from its worker thread (`run_script_job`); the C++ host takes
// every Qt call over to the GUI thread itself.
unsafe impl Send for CppHost {}
unsafe impl Sync for CppHost {}

#[derive(Default)]
struct GuiHost {
    cpp: Mutex<Option<Arc<CppHost>>>,
    log: Mutex<String>,
    frames: Mutex<Frames>,
    /// The script a job is running, for `stop_script`.
    running: Mutex<Option<Interrupt>>,
}

/// Source for `aegisub.get_frame`: the tab's video, opened on first use
/// (BestSource reuses the index the video box already built).
#[derive(Default)]
struct Frames {
    cache_dir: String,
    path: String,
    /// The subtitles of the run, given to libass on the first frame asked for with them.
    doc: Option<Document>,
    video: Option<Video>,
    renderer: Option<Renderer>,
}

impl Frames {
    fn set(&mut self, path: &str, doc: Document) {
        if path != self.path {
            (self.path, self.video, self.renderer) = (path.to_owned(), None, None);
        }
        self.doc = Some(doc);
    }

    fn frame(&mut self, n: i64, with_subtitles: bool) -> Option<VideoFrame> {
        if self.path.is_empty() {
            return None;
        }
        if self.video.is_none() {
            self.video = Some(Video::open(&self.path, &self.cache_dir, |_, _| true).ok()?);
        }
        let v = self.video.as_mut()?;
        let info = v.info();
        if !(0..info.frames).contains(&n) {
            return None;
        }
        let (w, h) = (info.width as usize, info.height as usize);
        let mut data = vec![0u8; w * h * 4];
        v.frame(n, &mut data).ok()?;
        if with_subtitles {
            if self.renderer.is_none() {
                let mut r = Renderer::new().ok()?;
                r.load_attached_fonts(&self.path);
                self.renderer = Some(r);
            }
            let r = self.renderer.as_mut()?;
            // once per run: the subtitles cannot change while the script runs
            if let Some(doc) = self.doc.take() {
                r.add_script_fonts(&doc);
                r.set_script(&crate::session::render_script(doc)).ok()?;
            }
            r.blend(v.ms_at_frame(n), w, h, w, h, &mut data).ok()?;
        }
        for px in data.as_chunks_mut::<4>().0 {
            px.swap(0, 2); // RGBA -> BGRA, the layout Aegisub's frame object exposes
        }
        Some(VideoFrame {
            width: w as i32,
            height: h as i32,
            pitch: (w * 4) as i32,
            flipped: false,
            data,
        })
    }
}

impl GuiHost {
    fn with<R>(&self, f: impl FnOnce(&AutomationHost) -> R) -> Option<R> {
        // not locked during the call: a dialog waits for the user, and the GUI thread may need
        // the host meanwhile
        let h = self.cpp.lock().unwrap_or_else(|e| e.into_inner()).clone();
        h.as_ref().and_then(|h| h.0.as_ref()).map(f)
    }

    fn set_running(&self, stop: Option<Interrupt>) {
        *self.running.lock().unwrap_or_else(|e| e.into_inner()) = stop;
    }
}

impl Clipboard for GuiHost {
    fn get(&self) -> Option<String> {
        self.with(|h| h.clipboard_get().to_string())
            .filter(|s| !s.is_empty())
    }
    fn set(&self, text: &str) -> bool {
        self.with(|h| h.clipboard_set(text)).unwrap_or(false)
    }
}

fn control_data(c: &DialogControl) -> DialogControlData {
    let mut d = DialogControlData {
        class_name: c.class.clone(),
        name: c.name.clone().unwrap_or_default(),
        x: c.x,
        y: c.y,
        width: c.width,
        height: c.height,
        hint: c.hint.clone().unwrap_or_default(),
        label: c.label.clone(),
        items: c.items.clone(),
        has_min: c.min.is_some(),
        min: c.min.unwrap_or(0.0),
        has_max: c.max.is_some(),
        max: c.max.unwrap_or(0.0),
        step: c.step,
        ..Default::default()
    };
    match &c.value {
        None => {}
        Some(DialogValue::Text(s)) => (d.kind, d.text) = (1, s.clone()),
        Some(DialogValue::Int(i)) => (d.kind, d.num) = (2, *i as f64),
        Some(DialogValue::Float(f)) => (d.kind, d.num) = (3, *f),
        Some(DialogValue::Bool(b)) => (d.kind, d.flag) = (4, *b),
        Some(DialogValue::Color(s)) => (d.kind, d.text) = (5, s.clone()),
    }
    d
}

fn dialog_data(dialog: &Dialog) -> DialogData {
    DialogData {
        title: dialog.title.clone(),
        controls: dialog.controls.iter().map(control_data).collect(),
        buttons: dialog.buttons.clone(),
        button_ids: dialog
            .buttons
            .iter()
            .map(|b| {
                dialog
                    .button_ids
                    .iter()
                    .find(|(_, l)| l == b)
                    .map(|(id, _)| id.clone())
                    .unwrap_or_default()
            })
            .collect(),
    }
}

fn control_value(d: &DialogControlData) -> Option<DialogValue> {
    Some(match d.kind {
        1 => DialogValue::Text(d.text.clone()),
        2 => DialogValue::Int(d.num.round() as i64),
        3 => DialogValue::Float(d.num),
        4 => DialogValue::Bool(d.flag),
        5 => DialogValue::Color(d.text.clone()),
        _ => return None,
    })
}

impl Host for GuiHost {
    fn log(&self, text: &str) {
        // the GUI shows it as it comes (in the progress window during a run); kept for
        // take_log without one
        if self.with(|h| h.log(text)).is_none() {
            self.log
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .push_str(text);
        }
    }
    fn progress(&self, percent: f64) {
        self.with(|h| h.progress(percent));
    }
    fn task(&self, text: &str) {
        self.with(|h| h.task(text));
    }
    fn title(&self, text: &str) {
        self.with(|h| h.title(text));
    }
    fn is_cancelled(&self) -> bool {
        self.with(|h| h.is_cancelled()).unwrap_or(false)
    }
    fn status_text(&self, text: &str) {
        self.with(|h| h.status_text(text));
    }
    fn dialog(&self, dialog: &Dialog) -> DialogResult {
        let data = dialog_data(dialog);
        let Some(reply) = self.with(|h| h.dialog(&data)) else {
            return dialog.accept_defaults();
        };
        DialogResult {
            button: reply.accepted.then_some(reply.button),
            values: reply
                .controls
                .iter()
                .filter(|c| !c.name.is_empty())
                .filter_map(|c| Some((c.name.clone(), control_value(c)?)))
                .collect(),
        }
    }
    fn open_files(
        &self,
        title: &str,
        dir: &str,
        file: &str,
        wildcard: &str,
        multiple: bool,
        must_exist: bool,
    ) -> Option<Vec<String>> {
        self.with(|h| h.open_files(title, dir, file, wildcard, multiple, must_exist))
            .filter(|v| !v.is_empty())
    }
    fn save_file(
        &self,
        title: &str,
        dir: &str,
        file: &str,
        wildcard: &str,
        no_prompt: bool,
    ) -> Option<String> {
        self.with(|h| h.save_file(title, dir, file, wildcard, no_prompt))
            .filter(|s| !s.is_empty())
    }
    fn frame(&self, n: i64, with_subtitles: bool) -> Option<VideoFrame> {
        let mut f = self.frames.lock().unwrap_or_else(|e| e.into_inner());
        f.frame(n, with_subtitles)
    }
}

/// A macro (`start_macro` .. `finish_macro`), export filters (`start_export` .. `finish_export`)
/// or their settings panels (`start_filter_configs` .. `finish_filter_configs`);
/// `run_script_job` runs the scripts on a worker thread.
pub struct ScriptJob {
    host: Arc<GuiHost>,
    doc: Document,
    ctx: Context,
    work: Work,
}

enum Work {
    Macro {
        script: Arc<Script>,
        index: usize,
        sel: Vec<usize>,
        active: Option<usize>,
        out: Option<Result<MacroOutcome, RunError>>,
    },
    /// Filters in the order they run.
    Export {
        steps: Vec<Step>,
        out: Option<Result<Document, RunError>>,
    },
    /// The export dialog's settings panels, parallel to the filter list: a script filter with a
    /// config function and the settings the file last exported with.
    Configs {
        filters: Vec<Option<(Arc<Script>, usize, String)>>,
        out: Vec<Option<Dialog>>,
    },
}

/// One filter of an export, as Aegisub's AssExportFilterChain holds it.
enum Step {
    Script(Arc<Script>, usize, HashMap<String, DialogValue>),
    Framerate(Framerate, Framerate),
    FixStyles,
}

/// An entry of the export filter list.
#[derive(Clone, Copy)]
enum Filter {
    Framerate,
    FixStyles,
    /// (script, filter of the script)
    Script(usize, usize),
}

// the job crosses to the worker thread
const _: fn() = || {
    fn send<T: Send>() {}
    send::<ScriptJob>();
};

pub fn run_script_job(job: &mut ScriptJob) {
    let (host, doc, ctx) = (&job.host, &job.doc, &job.ctx);
    match &mut job.work {
        Work::Macro {
            script,
            index,
            sel,
            active,
            out,
        } => {
            host.set_running(script.interrupter());
            *out = Some(script.run_macro(*index, doc, sel, *active, ctx));
        }
        Work::Export { steps, out } => *out = Some(run_filters(host, steps, doc, ctx)),
        Work::Configs { filters, out } => *out = filter_configs(host, filters, doc, ctx),
    }
    host.set_running(None);
}

/// Aegisub's AssExporter::Export: each chosen filter in turn on the copy, a script's with what
/// its panel in the export dialog held.
fn run_filters(
    host: &GuiHost,
    steps: &[Step],
    doc: &Document,
    ctx: &Context,
) -> Result<Document, RunError> {
    let mut doc = doc.clone();
    for step in steps {
        match step {
            Step::Script(script, f, settings) => {
                host.set_running(script.interrupter());
                doc = script.run_filter(*f, &doc, settings, ctx)?;
            }
            Step::Framerate(from, to) => ops::change_fps(&mut doc, from, to),
            Step::FixStyles => {
                ops::fix_styles(&mut doc);
            }
        }
    }
    Ok(doc)
}

/// Aegisub's DrawSettings: every script filter's config function as the export dialog opens,
/// with what the file last exported with put back into its controls. A failing one has no panel.
fn filter_configs(
    host: &GuiHost,
    filters: &[Option<(Arc<Script>, usize, String)>],
    doc: &Document,
    ctx: &Context,
) -> Vec<Option<Dialog>> {
    filters
        .iter()
        .map(|f| {
            let (script, i, stored) = f.as_ref()?;
            if host.is_cancelled() {
                return None;
            }
            host.set_running(script.interrupter());
            match script.filter_config(*i, doc, ctx) {
                Ok(d) => d.map(|mut d| {
                    d.unserialise(stored);
                    d
                }),
                Err(RunError::Lua(e)) => {
                    host.log(&format!(
                        "Runtime error in Lua config dialog function:\n{e}\n"
                    ));
                    None
                }
                Err(_) => None,
            }
        })
        .collect()
}

/// Scripts loaded side by side (Aegisub's autoload loads each file with std::async), in the
/// order given. A script that panicked while loading is None.
fn load_scripts(paths: &[PathBuf], env: &Env) -> Vec<Option<Script>> {
    let next = AtomicUsize::new(0);
    let slots: Vec<Mutex<Option<Script>>> = paths.iter().map(|_| Mutex::new(None)).collect();
    let workers = std::thread::available_parallelism().map_or(4, |n| n.get());
    std::thread::scope(|s| {
        for _ in 0..workers.min(paths.len()) {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    let Some(p) = paths.get(i) else { break };
                    let script = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        Script::load(p, env)
                    }));
                    *slots[i].lock().unwrap_or_else(|e| e.into_inner()) = script.ok();
                }
            });
        }
    });
    slots
        .into_iter()
        .map(|m| m.into_inner().unwrap_or_else(|e| e.into_inner()))
        .collect()
}

/// `path` relative to `base`, both absolute on the same drive (agi::Path::MakeRelative).
fn relative(path: &Path, base: &Path) -> Option<PathBuf> {
    let (p, b): (Vec<_>, Vec<_>) = (path.components().collect(), base.components().collect());
    if !path.is_absolute() || !base.is_absolute() || p.first() != b.first() {
        return None;
    }
    let common = p.iter().zip(&b).take_while(|(x, y)| x == y).count();
    let mut out = PathBuf::new();
    for c in &b[common..] {
        if !matches!(c, Component::CurDir) {
            out.push("..");
        }
    }
    for c in &p[common..] {
        out.push(c.as_os_str());
    }
    Some(out)
}

pub struct Automation {
    env: Env,
    host: Arc<GuiHost>,
    scripts: Vec<Arc<Script>>,
    /// Flat macro list: (script index, macro index).
    macros: Vec<(usize, usize)>,
}

impl Automation {
    pub fn new(data_dir: &str, user_dir: &str) -> Self {
        let host = Arc::new(GuiHost::default());
        let paths = Paths::new(PathBuf::from(data_dir), PathBuf::from(user_dir));
        Automation {
            env: Env::new(paths, host.clone()),
            host,
            scripts: Vec::new(),
            macros: Vec::new(),
        }
    }

    /// BestSource index cache shared with the video box.
    pub fn set_cache_dir(&mut self, dir: &str) {
        self.host
            .frames
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .cache_dir = dir.to_owned();
    }

    /// The GUI's host; it also serves `aegisub.clipboard`.
    pub fn set_host(&mut self, host: UniquePtr<AutomationHost>) {
        *self.host.cpp.lock().unwrap_or_else(|e| e.into_inner()) = Some(Arc::new(CppHost(host)));
        fs_automation::set_clipboard(Some(self.host.clone()));
    }

    /// Stops the script a job is running, also one that never checks for Cancel (any thread).
    pub fn stop_script(&self) {
        let running = self.host.running.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(stop) = running.as_ref() {
            stop.trigger();
        }
    }

    fn index(&mut self) {
        self.macros = self
            .scripts
            .iter()
            .enumerate()
            .flat_map(|(s, sc)| (0..sc.macros.len()).map(move |m| (s, m)))
            .collect();
    }

    /// Scripts loaded side by side, then put in the list in the order given (a path loaded
    /// already is replaced); returns the load errors.
    fn add_all(&mut self, paths: &[PathBuf]) -> Vec<String> {
        let mut errors = Vec::new();
        for (p, s) in paths.iter().zip(load_scripts(paths, &self.env)) {
            let Some(s) = s else {
                errors.push(format!("{}: the script crashed while loading", p.display()));
                continue;
            };
            if let Some(e) = &s.error {
                errors.push(format!("{}: {e}", p.display()));
            }
            self.scripts.retain(|o| o.path != s.path);
            self.scripts.push(Arc::new(s));
        }
        self.index();
        errors
    }

    /// Aegisub's AssExportFilterChain: the built-in filters (registered first), then each
    /// script's in load order, sorted by priority, highest first; a tie keeps that order. A name
    /// taken already gets " (1)", " (2)", ….
    fn chain(&self) -> Vec<(Filter, String)> {
        let mut all = vec![
            (Filter::Framerate, 1000, "Transform Framerate".to_owned()),
            (Filter::FixStyles, -5000, "Fix Styles".to_owned()),
        ];
        for (s, script) in self.scripts.iter().enumerate() {
            for (f, info) in script.filters.iter().enumerate() {
                let mut name = info.name.clone();
                for n in 1.. {
                    if !all.iter().any(|(_, _, taken)| *taken == name) {
                        break;
                    }
                    name = format!("{} ({n})", info.name);
                }
                all.push((Filter::Script(s, f), info.priority, name));
            }
        }
        all.sort_by_key(|&(_, p, _)| std::cmp::Reverse(p));
        all.into_iter().map(|(f, _, n)| (f, n)).collect()
    }

    fn add(&mut self, path: &Path) -> Result<(), String> {
        let s = Script::load(path, &self.env);
        let err = s.error.clone();
        self.scripts.retain(|o| o.path != s.path);
        self.scripts.push(Arc::new(s));
        self.index();
        err.map_or(Ok(()), |e| Err(format!("{}: {e}", path.display())))
    }

    /// Load every script in the autoload dirs, side by side; returns the load errors.
    pub fn load_autoload(&mut self) -> Vec<String> {
        let files = fs_automation::autoload_files(&self.env.paths);
        self.add_all(&files)
    }

    pub fn load_script(&mut self, path: &str) -> Result<(), String> {
        self.add(Path::new(path))
    }

    pub fn scripts(&self) -> Vec<ScriptItem> {
        let autoload = fs_automation::autoload_files(&self.env.paths);
        self.scripts
            .iter()
            .map(|s| ScriptItem {
                path: s.path.to_string_lossy().into_owned(),
                name: s.name.clone(),
                description: s.description.clone(),
                author: s.author.clone(),
                version: s.version.clone(),
                error: s.error.clone().unwrap_or_default(),
                autoload: autoload.contains(&s.path),
                macros: s.macros.len(),
                filters: s.filters.len(),
            })
            .collect()
    }

    /// Reload every loaded script from disk and load new files in the
    /// autoload dirs (Aegisub's "Reload all scripts" with its rescan).
    pub fn reload(&mut self) -> Vec<String> {
        let mut paths: Vec<PathBuf> = self.scripts.drain(..).map(|s| s.path.clone()).collect();
        for p in fs_automation::autoload_files(&self.env.paths) {
            if !paths.contains(&p) {
                paths.push(p);
            }
        }
        self.add_all(&paths)
    }

    /// Reload one script, keeping its place in the macro menu.
    pub fn reload_script(&mut self, index: usize) -> Result<(), String> {
        let Some(old) = self.scripts.get(index) else {
            return Ok(());
        };
        let s = Script::load(&old.path, &self.env);
        let err = s
            .error
            .as_ref()
            .map(|e| format!("{}: {e}", s.path.display()));
        self.scripts[index] = Arc::new(s);
        self.index();
        err.map_or(Ok(()), Err)
    }

    pub fn remove_script(&mut self, index: usize) {
        if index < self.scripts.len() {
            self.scripts.remove(index);
            self.index();
        }
    }

    /// Aegisub's `Automation/Trace Level`: `aegisub.debug.out` messages above it are dropped,
    /// in the scripts loaded already too.
    pub fn set_trace_level(&self, level: i32) {
        (self.env.trace_level).store(level.clamp(0, 5), Ordering::Relaxed);
    }

    /// Aegisub's `Path/Automation/Base`, what a `$` script path is relative to.
    fn automation_base(&self) -> PathBuf {
        self.env.paths.data.join("automation")
    }

    /// A token of the file's "Automation Scripts" as (base folder, path): `~` next to the
    /// file, `$` in the automation base, `/` absolute; None for another specifier.
    fn local_path(&self, session: &Session, tok: &str) -> Option<(PathBuf, PathBuf)> {
        let mut chars = tok.chars();
        let base = match chars.next()? {
            '~' => (session.path.as_deref().and_then(Path::parent))
                .map(Path::to_path_buf)
                .unwrap_or_default(),
            '$' => self.automation_base(),
            '/' => PathBuf::new(),
            _ => return None,
        };
        let path = base.join(chars.as_str());
        Some((base, path))
    }

    fn local_tokens(session: &Session) -> Vec<String> {
        (session.project_value("Automation Scripts").split('|'))
            .map(str::trim)
            .filter(|t| !t.is_empty())
            .map(str::to_owned)
            .collect()
    }

    /// Aegisub's LocalScriptManager::Reload: what the file's "Automation Scripts" lists and is
    /// not loaded yet, for the trust prompt.
    // shortcut: loaded scripts are shared by the tabs (Aegisub has one file per window): a file's scripts stay loaded after it is closed; give each tab its own set if users mix files with different local scripts.
    pub fn local_scripts(&self, session: &Session) -> Vec<LocalScript> {
        Self::local_tokens(session)
            .into_iter()
            .filter_map(|token| {
                let Some((base, path)) = self.local_path(session, &token) else {
                    return Some(LocalScript {
                        token,
                        ..Default::default()
                    });
                };
                let c = canonical(&path);
                if self.scripts.iter().any(|s| s.path == c) {
                    return None;
                }
                Some(LocalScript {
                    token,
                    path: path.to_string_lossy().into_owned(),
                    base: base.to_string_lossy().into_owned(),
                    exists: path.is_file(),
                })
            })
            .collect()
    }

    /// Aegisub's SaveLoadedList for a script loaded by hand: the file lists it by the shortest of
    /// `$` (relative to the automation base), `~` (relative to the file) and `/` (absolute).
    pub fn add_local_script(&self, session: &mut Session, path: &str) {
        let script = canonical(Path::new(path));
        let mut list = Self::local_tokens(session);
        let listed = |t: &String| {
            (self.local_path(session, t)).is_some_and(|(_, p)| canonical(&p) == script)
        };
        if list.iter().any(listed) {
            return;
        }
        let generic = |p: &Path| p.to_string_lossy().replace('\\', "/");
        let abs = generic(&script);
        // the folders canonical too, as the script is: a short 8.3 name in one (a TEMP folder)
        // would otherwise leave nothing in common
        let base = relative(&script, &canonical(&self.automation_base())).map(|r| generic(&r));
        let near = (session.path.as_deref().and_then(Path::parent))
            .and_then(|d| relative(&script, &canonical(d)))
            .map(|r| generic(&r));
        list.push(match (base, near) {
            (Some(b), n)
                if b.len() <= abs.len() && n.as_ref().is_none_or(|n| b.len() <= n.len()) =>
            {
                format!("${b}")
            }
            (_, Some(n)) if n.len() <= abs.len() => format!("~{n}"),
            _ => format!("/{abs}"),
        });
        session.set_project_value("Automation Scripts", &list.join("|"));
    }

    /// The file no longer lists a script removed in the Automation Manager.
    pub fn remove_local_script(&self, session: &mut Session, path: &str) {
        let script = canonical(Path::new(path));
        let list = Self::local_tokens(session);
        let kept: Vec<&str> = (list.iter())
            .filter(|t| {
                !(self.local_path(session, t)).is_some_and(|(_, p)| canonical(&p) == script)
            })
            .map(String::as_str)
            .collect();
        if kept.len() != list.len() {
            session.set_project_value("Automation Scripts", &kept.join("|"));
        }
    }

    pub fn macros(&self) -> Vec<MacroItem> {
        self.macros
            .iter()
            .enumerate()
            .map(|(id, &(s, m))| MacroItem {
                id,
                script: self.scripts[s].name.clone(),
                name: self.scripts[s].macros[m].name.clone(),
                description: self.scripts[s].macros[m].description.clone(),
            })
            .collect()
    }

    /// The export dialog's filter list (Aegisub's): the built-in Transform Framerate and Fix
    /// Styles among the scripts' filters by priority.
    pub fn export_filters(&self) -> Vec<FilterItem> {
        (self.chain().into_iter())
            .map(|(f, name)| match f {
                Filter::Script(s, i) => FilterItem {
                    name,
                    description: self.scripts[s].filters[i].description.clone(),
                    kind: 0,
                    config: self.scripts[s].filters[i].has_config,
                },
                Filter::Framerate => FilterItem {
                    name,
                    kind: 1,
                    ..Default::default()
                },
                Filter::FixStyles => FilterItem {
                    name,
                    kind: 2,
                    ..Default::default()
                },
            })
            .collect()
    }

    /// The export dialog's settings panels as a job (GUI thread): every script filter's config
    /// function, as Aegisub calls each as its dialog opens.
    pub fn start_filter_configs(&self, session: &Session) -> Box<ScriptJob> {
        let filters = (self.chain().into_iter())
            .map(|(f, name)| match f {
                Filter::Script(s, i) if self.scripts[s].filters[i].has_config => Some((
                    self.scripts[s].clone(),
                    i,
                    (session.filter_settings.get(&inline_string_encode(&name)))
                        .cloned()
                        .unwrap_or_default(),
                )),
                _ => None,
            })
            .collect();
        Box::new(ScriptJob {
            host: self.host.clone(),
            doc: session.ed.doc.clone(),
            ctx: Self::context(session),
            work: Work::Configs {
                filters,
                out: Vec::new(),
            },
        })
    }

    /// The panels, parallel to `export_filters()`; an error when the user cancelled.
    pub fn finish_filter_configs(&self, job: Box<ScriptJob>) -> Result<Vec<DialogData>, String> {
        let Work::Configs { filters, out } = job.work else {
            return Err("not the export settings".into());
        };
        if job.host.is_cancelled() {
            return Err("cancelled".into());
        }
        Ok((0..filters.len())
            .map(|k| out.get(k).and_then(Option::as_ref).map(dialog_data))
            .map(Option::unwrap_or_default)
            .collect())
    }

    /// Export filters `ids` (of `export_filters()`, in the order they run) as a job on a copy
    /// (GUI thread). `panels`, parallel to `export_filters()`, are as the export dialog left
    /// them: what a script filter's panel holds is its setting for this file from now on
    /// (Aegisub's LoadSettings), and what the run gets.
    pub fn start_export(
        &self,
        ids: &[usize],
        panels: &[DialogData],
        spec: &ExportSpec,
        video: &[i64],
        session: &mut Session,
    ) -> Result<Box<ScriptJob>, String> {
        let chain = self.chain();
        let mut steps = Vec::with_capacity(ids.len());
        for &id in ids {
            let Some((filter, name)) = chain.get(id) else {
                continue;
            };
            steps.push(match *filter {
                Filter::Framerate => Step::Framerate(
                    crate::session::export_rate(&spec.fps_from, video)?,
                    crate::session::export_rate(&spec.fps_to, video)?,
                ),
                Filter::FixStyles => Step::FixStyles,
                Filter::Script(s, f) => {
                    let mut settings = HashMap::new();
                    if let Some(p) = panels.get(id).filter(|p| !p.controls.is_empty()) {
                        let values: Vec<(&str, DialogValue)> = (p.controls.iter())
                            .filter_map(|c| Some((c.name.as_str(), control_value(c)?)))
                            .collect();
                        session.filter_settings.insert(
                            inline_string_encode(name),
                            serialise_settings(values.iter().map(|(n, v)| (*n, v))),
                        );
                        settings.extend(
                            (values.into_iter())
                                .filter(|(n, _)| !n.is_empty())
                                .map(|(n, v)| (n.to_owned(), v)),
                        );
                    }
                    Step::Script(self.scripts[s].clone(), f, settings)
                }
            });
        }
        Ok(Box::new(ScriptJob {
            host: self.host.clone(),
            doc: session.ed.doc.clone(),
            ctx: Self::context(session),
            work: Work::Export { steps, out: None },
        }))
    }

    /// Saves what the filters made; false when they were cancelled or failed (the error is in
    /// the log) (GUI thread).
    pub fn finish_export(
        &self,
        job: Box<ScriptJob>,
        session: &Session,
        path: &str,
        spec: &ExportSpec,
        video: &[i64],
    ) -> Result<bool, String> {
        let Work::Export { out, .. } = job.work else {
            return Err("not an export".into());
        };
        // Fix Styles and Transform Framerate ran in the chain, in their places
        let spec = ExportSpec {
            fix_styles: false,
            fps_to: String::new(),
            ..spec.clone()
        };
        match out.unwrap_or(Err(RunError::Cancelled)) {
            Ok(doc) => session.export_doc(doc, path, &spec, video).map(|()| true),
            Err(RunError::Cancelled | RunError::Lua(_)) => Ok(false),
            Err(e) => Err(e.to_string()),
        }
    }

    fn context(session: &Session) -> Context {
        // the project's files as the tab keeps them (Aegisub's project_properties); a relative
        // one is next to the script
        let file = |k: &str| {
            let v = session.project_value(k);
            let p = PathBuf::from(&v);
            match session.path.as_deref().and_then(Path::parent) {
                _ if v.is_empty() => None,
                Some(dir) if p.is_relative() && !v.starts_with('?') => Some(dir.join(p)),
                _ => Some(p),
            }
        };
        Context {
            subtitle_file: session.path.clone(),
            video_file: file("Video File"),
            audio_file: file("Audio File"),
            video_size: session.video_size.map(|(w, h)| {
                // Aegisub's aspect ratio value and type; a video with non-square pixels opens
                // as a custom ratio of its display aspect
                let (sn, sd) = session.video_sar;
                let (wf, hf) = (f64::from(w), f64::from(h));
                let mode = session.project_value("Video AR Mode").trim().parse::<i32>();
                let (ar, mode) = match mode.unwrap_or(0) {
                    1 => (4.0 / 3.0, 1),
                    2 => (16.0 / 9.0, 2),
                    3 => (2.35, 3),
                    4 => {
                        let v = session.project_value("Video AR Value");
                        (v.trim().parse().unwrap_or(wf / hf), 4)
                    }
                    _ if sn > 0 && sd > 0 && sn != sd => {
                        (wf * f64::from(sn) / (hf * f64::from(sd)), 4)
                    }
                    _ => (wf / hf, 0),
                };
                (w, h, ar, mode)
            }),
            framerate: session.framerate.clone(),
            keyframes: session.keyframes.clone(),
            modified: session.ed.is_modified(),
            ..Default::default()
        }
    }

    fn selection(session: &Session, rows: &[usize], active: i64) -> (Vec<usize>, Option<usize>) {
        let mut sel = session.main_events(rows);
        sel.sort_unstable();
        let act = usize::try_from(active)
            .ok()
            .and_then(|a| session.main_events(&[a]).first().copied());
        (sel, act)
    }

    /// What the Automation menu shows for every macro, by id (Aegisub's validate and
    /// is_active): the selection and the project are read once for all of them.
    pub fn macro_states(&self, session: &Session, rows: &[usize], active: i64) -> Vec<MacroState> {
        let (sel, act) = Self::selection(session, rows, active);
        let ctx = Self::context(session);
        let doc = &session.ed.doc;
        self.macros
            .iter()
            .map(|&(s, m)| {
                let script = &self.scripts[s];
                let (enabled, help) = script.validate_macro(m, doc, &sel, act, &ctx);
                let checked = script.macro_is_active(m, doc, &sel, act, &ctx);
                MacroState {
                    enabled,
                    toggle: checked.is_some(),
                    checked: checked.unwrap_or(false),
                    help: help.unwrap_or_default(),
                }
            })
            .collect()
    }

    /// Prepares a macro run on a copy of the document (GUI thread).
    pub fn start_macro(
        &self,
        id: usize,
        session: &Session,
        rows: &[usize],
        active: i64,
        editor: &MacroContext,
    ) -> Result<Box<ScriptJob>, String> {
        let &(s, m) = self.macros.get(id).ok_or("no such macro")?;
        let (sel, act) = Self::selection(session, rows, active);
        let ctx = Context {
            audio_selection: (editor.has_audio).then_some((editor.audio_start, editor.audio_end)),
            edit: EditState {
                cursor: editor.cursor,
                sel_start: editor.sel_start,
                sel_end: editor.sel_end,
            },
            ..Self::context(session)
        };
        let video = ctx
            .video_file
            .as_deref()
            .and_then(Path::to_str)
            .unwrap_or_default();
        self.host
            .frames
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .set(video, session.ed.doc.clone());
        Ok(Box::new(ScriptJob {
            host: self.host.clone(),
            doc: session.ed.doc.clone(),
            ctx,
            work: Work::Macro {
                script: self.scripts[s].clone(),
                index: m,
                sel,
                active: act,
                out: None,
            },
        }))
    }

    /// Applies a finished run: each script undo point becomes an undo step (GUI thread). A Lua
    /// error is already in the log.
    pub fn finish_macro(
        &self,
        job: Box<ScriptJob>,
        session: &mut Session,
    ) -> Result<MacroResult, String> {
        let Work::Macro { out, .. } = job.work else {
            return Err("not a macro".into());
        };
        let out = match out.unwrap_or(Err(RunError::Cancelled)) {
            Ok(o) => o,
            Err(RunError::Cancelled | RunError::Lua(_)) => return Ok(MacroResult::default()),
            Err(e) => return Err(e.to_string()),
        };
        let changed = !out.commits.is_empty();
        for (desc, doc) in out.commits {
            session.ed.edit(&desc, |d| *d = doc);
        }
        session.refresh();
        let rows = session.rows_of_events(&out.selection);
        let active = out
            .active
            .and_then(|a| session.rows_of_events(&[a]).first().copied())
            .map_or(-1, |a| a as i64);
        let edit = out.edit.unwrap_or_default();
        Ok(MacroResult {
            changed,
            selection: rows,
            active,
            edit_changed: out.edit.is_some(),
            cursor: edit.cursor,
            sel_start: edit.sel_start,
            sel_end: edit.sel_end,
        })
    }

    pub fn take_log(&self) -> String {
        std::mem::take(&mut *self.host.log.lock().unwrap_or_else(|e| e.into_inner()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn macro_lua(name: &str) -> String {
        format!(r#"script_name="{name}" aegisub.register_macro("{name}", "", function() end)"#)
    }

    fn run(a: &Automation, id: usize, s: &mut Session, rows: &[usize]) -> MacroResult {
        let mut job = a
            .start_macro(id, s, rows, 0, &MacroContext::default())
            .unwrap();
        run_script_job(&mut job);
        a.finish_macro(job, s).unwrap()
    }

    /// The menu's view of the macros: validate's help text, a toggle's check mark, and
    /// video_size as Aegisub reports an anamorphic video (a custom ratio of its display aspect).
    #[test]
    fn menu_states_and_video_size() {
        let dir = std::env::temp_dir().join(format!("fsb-menu-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("menu.lua");
        std::fs::write(
            &script,
            r#"script_name="Menu"
aegisub.register_macro("Plain", "", function() end)
aegisub.register_macro("Toggle", "desc", function() end,
  function(subs, sel) return #sel > 0, "help for " .. #sel end, function() return true end)
aegisub.register_macro("Size", "", function() end, function()
  local w, h, ar, t = aegisub.video_size()
  return t == 4 and math.abs(ar - 16 / 9) < 1e-9
end)"#,
        )
        .unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        a.load_script(script.to_str().unwrap()).unwrap();
        let mut s = Session::default();
        let st = a.macro_states(&s, &[0], 0);
        assert!(st[0].enabled && !st[0].toggle && st[0].help.is_empty());
        assert!(st[1].enabled && st[1].toggle && st[1].checked);
        assert_eq!(st[1].help, "help for 1");
        assert!(!st[2].enabled, "no video");
        s.set_video_context(720, 480, 32, 27, &[], &[]);
        assert!(a.macro_states(&s, &[0], 0)[2].enabled);
        // a ratio set by hand is Aegisub's type and value
        s.set_project_value("Video AR Mode", "1");
        assert!(!a.macro_states(&s, &[0], 0)[2].enabled);
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn runs_a_macro_as_undo_step() {
        let dir = std::env::temp_dir().join(format!("fsb-auto-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("upper.lua");
        std::fs::write(
            &script,
            r#"script_name="Upper"
aegisub.register_macro("Upper", "uppercase", function(subs, sel)
  for _, i in ipairs(sel) do local l = subs[i]; l.text = l.text:upper(); subs[i] = l end
  return sel
end)"#,
        )
        .unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        a.load_script(script.to_str().unwrap()).unwrap();
        assert_eq!(a.macros()[0].name, "Upper");
        let mut s = Session::default();
        let mut r = s.row(0);
        r.text = "abc".into();
        s.set_row(0, &r, "");
        assert!(a.macro_states(&s, &[0], 0)[0].enabled);
        let res = run(&a, 0, &mut s, &[0]);
        assert!(res.changed);
        assert_eq!(res.selection, vec![0]);
        assert_eq!(s.row(0).text, "ABC");
        assert!(s.undo());
        assert_eq!(s.row(0).text, "abc");
    }

    #[test]
    fn manage_scripts() {
        let dir = std::env::temp_dir().join(format!("fsb-manage-{}", std::process::id()));
        let autoload = dir.join("user/automation/autoload");
        std::fs::create_dir_all(&autoload).unwrap();
        let a_lua = dir.join("a.lua");
        std::fs::write(&a_lua, macro_lua("A")).unwrap();
        std::fs::write(dir.join("b.lua"), macro_lua("B")).unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        a.load_script(a_lua.to_str().unwrap()).unwrap();
        a.load_script(dir.join("b.lua").to_str().unwrap()).unwrap();
        let names = |a: &Automation| a.macros().into_iter().map(|m| m.name).collect::<Vec<_>>();
        assert_eq!(names(&a), ["A", "B"]);
        // reloading one keeps its place; a broken one keeps its entry with the error
        std::fs::write(&a_lua, macro_lua("A2")).unwrap();
        a.reload_script(0).unwrap();
        assert_eq!(names(&a), ["A2", "B"]);
        std::fs::write(&a_lua, "syntax error(").unwrap();
        assert!(a.reload_script(0).is_err());
        assert!(!a.scripts()[0].error.is_empty());
        a.remove_script(0);
        assert_eq!(names(&a), ["B"]);
        // reload all picks up a new autoload script (and the bundled ones)
        std::fs::write(autoload.join("c.lua"), macro_lua("C")).unwrap();
        let errors = a.reload();
        assert!(errors.iter().all(|e| !e.contains("c.lua")), "{errors:?}");
        let n = names(&a);
        assert!(n[0] == "B" && n.contains(&"C".to_owned()), "{n:?}");
        let s = a.scripts();
        assert!(
            s.iter()
                .any(|s| s.name == "C" && s.autoload && s.macros == 1)
        );
        assert!(s.iter().any(|s| s.name == "B" && !s.autoload));
    }

    #[test]
    fn export_filters_run_on_the_copy() {
        let dir = std::env::temp_dir().join(format!("fsb-filter-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("f.lua");
        std::fs::write(
            &script,
            r#"script_name="F"
aegisub.register_filter("Late", "", 1, function(subs) local l = subs[#subs]; l.text = l.text .. "!"; subs[#subs] = l end)
aegisub.register_filter("Early", "", 9, function(subs) local l = subs[#subs]; l.text = l.text .. "?"; subs[#subs] = l end)
aegisub.register_filter("Fix Styles", "", -9000, function(subs) local l = subs[#subs]; l.text = l.text .. l.style; subs[#subs] = l end)
aegisub.register_filter("Early", "", 9, function() end)"#,
        )
        .unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        a.load_script(script.to_str().unwrap()).unwrap();
        // Aegisub's chain: the built-ins among the scripts' filters by priority, names unique
        let list = a.export_filters();
        let names: Vec<&str> = list.iter().map(|f| f.name.as_str()).collect();
        assert_eq!(
            names,
            [
                "Transform Framerate",
                "Early",
                "Early (1)",
                "Late",
                "Fix Styles",
                "Fix Styles (1)"
            ]
        );
        assert_eq!((list[0].kind, list[1].kind, list[4].kind), (1, 0, 2));
        let mut s = Session::default();
        let mut r = s.row(0);
        r.text = "x".into();
        r.style = "Gone".into();
        s.set_row(0, &r, "");
        let out = dir.join("out.ass");
        let export = |a: &Automation, s: &mut Session, ids: &[usize]| {
            let spec = ExportSpec::default();
            let mut job = a.start_export(ids, &[], &spec, &[], s).unwrap();
            run_script_job(&mut job);
            a.finish_export(job, s, out.to_str().unwrap(), &spec, &[])
                .unwrap()
        };
        assert!(export(&a, &mut s, &[1, 3]));
        assert!(std::fs::read_to_string(&out).unwrap().contains(",x?!"));
        assert_eq!(s.row(0).text, "x");
        // in the order given: Fix Styles' Default before the script that reads the style, and
        // after it once moved down
        assert!(export(&a, &mut s, &[4, 5]));
        assert!(std::fs::read_to_string(&out).unwrap().contains(",xDefault"));
        assert!(export(&a, &mut s, &[5, 4]));
        assert!(std::fs::read_to_string(&out).unwrap().contains(",xGone"));
        // a failing filter exports nothing and leaves its error in the log
        std::fs::write(
            &script,
            r#"script_name="F"
aegisub.register_filter("Bad", "", 1, function() error("broken") end)"#,
        )
        .unwrap();
        a.reload_script(0).unwrap();
        std::fs::remove_file(&out).unwrap();
        assert!(!export(&a, &mut s, &[1]));
        assert!(!out.exists() && a.take_log().contains("broken"));
    }

    /// Aegisub's settings panels: a filter's config function runs as the export dialog opens,
    /// what the panel held at export reaches the filter and comes back in the next dialog, for
    /// this file only.
    #[test]
    fn export_settings_are_kept_for_the_file() {
        let dir = std::env::temp_dir().join(format!("fsb-cfg-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("c.lua");
        std::fs::write(
            &script,
            r#"script_name="C"
aegisub.register_filter("Suffix", "", 0, function(subs, cfg)
  local l = subs[#subs]; l.text = l.text .. cfg.suffix .. (cfg.twice and cfg.suffix or ""); subs[#subs] = l
end, function()
  return {{class = "label", label = "Suffix:"}, {class = "edit", name = "suffix", value = "?"},
          {class = "checkbox", name = "twice", value = false}}
end)"#,
        )
        .unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        a.load_script(script.to_str().unwrap()).unwrap();
        let mut s = Session::default();
        let configs = |a: &Automation, s: &Session| {
            let mut job = a.start_filter_configs(s);
            run_script_job(&mut job);
            a.finish_filter_configs(job).unwrap()
        };
        let mut panels = configs(&a, &s);
        let id = 1;
        assert_eq!(a.export_filters()[id].name, "Suffix");
        assert!(panels[0].controls.is_empty() && panels[2].controls.is_empty());
        let p = &mut panels[id].controls;
        assert_eq!((p.len(), p[1].text.as_str(), p[2].flag), (3, "?", false));
        (p[1].text, p[2].flag) = ("|:#".into(), true);
        let out = dir.join("out.ass");
        let spec = ExportSpec::default();
        let mut job = a.start_export(&[id], &panels, &spec, &[], &mut s).unwrap();
        run_script_job(&mut job);
        assert!(
            a.finish_export(job, &s, out.to_str().unwrap(), &spec, &[])
                .unwrap()
        );
        assert!(std::fs::read_to_string(&out).unwrap().contains(",|:#|:#"));
        let again = configs(&a, &s);
        assert_eq!(
            (
                again[id].controls[1].text.as_str(),
                again[id].controls[2].flag
            ),
            ("|:#", true)
        );
        // nothing of it in the file, and another file starts from the defaults
        assert!(s.project_value("Automation Settings Suffix").is_empty());
        let other = Session::default();
        assert_eq!(configs(&a, &other)[id].controls[1].text, "?");
        std::fs::remove_dir_all(dir).unwrap();
    }

    /// Aegisub's local scripts: "Automation Scripts" names them relative to the file (`~`), to
    /// the automation base (`$`) or absolutely (`/`); one loaded already is not offered again.
    #[test]
    fn local_scripts_of_the_file() {
        let dir = std::env::temp_dir().join(format!("fsb-local-{}", std::process::id()));
        let near = dir.join("subs").join("tools");
        std::fs::create_dir_all(&near).unwrap();
        let data = dir.join("data");
        std::fs::create_dir_all(data.join("automation")).unwrap();
        std::fs::write(near.join("n.lua"), macro_lua("N")).unwrap();
        std::fs::write(data.join("automation").join("b.lua"), macro_lua("B")).unwrap();
        let elsewhere = dir.join("x.lua");
        std::fs::write(&elsewhere, macro_lua("X")).unwrap();
        let mut a = Automation::new(data.to_str().unwrap(), dir.join("user").to_str().unwrap());
        let file = dir.join("subs").join("ep.ass");
        std::fs::write(&file, "[Script Info]\nScriptType: v4.00+\n\n[Events]\n").unwrap();
        let mut s = Session::default();
        s.open(file.to_str().unwrap()).unwrap();
        // added by hand: the shortest way to name each, once
        for p in [
            near.join("n.lua"),
            data.join("automation").join("b.lua"),
            elsewhere.clone(),
        ] {
            a.add_local_script(&mut s, p.to_str().unwrap());
        }
        a.add_local_script(&mut s, near.join("n.lua").to_str().unwrap());
        assert_eq!(
            s.project_value("Automation Scripts"),
            "~tools/n.lua|$b.lua|~../x.lua"
        );
        let abs = canonical(&elsewhere).to_string_lossy().replace('\\', "/");
        s.set_project_value(
            "Automation Scripts",
            &format!("~tools/n.lua|$b.lua| /{abs} | ?odd.lua|~missing.lua"),
        );
        let l = a.local_scripts(&s);
        let tokens: Vec<&str> = l.iter().map(|l| l.token.as_str()).collect();
        assert_eq!(
            tokens,
            [
                "~tools/n.lua",
                "$b.lua",
                &format!("/{abs}"),
                "?odd.lua",
                "~missing.lua"
            ]
        );
        assert!(l[0].exists && l[1].exists && l[2].exists && !l[4].exists);
        assert!(l[3].path.is_empty());
        // (its include dir is not in this data dir: loaded with an error, listed all the same)
        let _ = a.load_script(l[0].path.as_str());
        assert_eq!(a.local_scripts(&s).len(), 4, "loaded already");
        a.remove_local_script(&mut s, near.join("n.lua").to_str().unwrap());
        assert!(!s.project_value("Automation Scripts").contains("n.lua"));
        assert!(!s.file_info().modified);
        std::fs::remove_dir_all(dir).unwrap();
    }

    /// Autoload loads its files side by side and lists them in file name order.
    #[test]
    fn autoload_in_parallel_keeps_the_order() {
        let dir = std::env::temp_dir().join(format!("fsb-par-{}", std::process::id()));
        let autoload = dir.join("user").join("automation").join("autoload");
        std::fs::create_dir_all(&autoload).unwrap();
        for k in 0..24 {
            std::fs::write(
                autoload.join(format!("s{k:02}.lua")),
                macro_lua(&format!("S{k:02}")),
            )
            .unwrap();
        }
        std::fs::write(autoload.join("s99.lua"), "syntax error(").unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        let errors = a.load_autoload();
        assert!(
            errors.len() == 1 && errors[0].contains("s99.lua"),
            "{errors:?}"
        );
        let names: Vec<String> = (a.macros().into_iter())
            .map(|m| m.name)
            .filter(|n| n.len() == 3 && n.starts_with('S'))
            .collect();
        let want: Vec<String> = (0..24).map(|k| format!("S{k:02}")).collect();
        assert_eq!(names, want);
        std::fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn get_frame_reads_the_tab_video() {
        let dir = std::env::temp_dir().join(format!("fsb-frame-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("frame.lua");
        std::fs::write(
            &script,
            r#"script_name="Frame"
aegisub.register_macro("Frame", "", function(subs, sel)
  local f, g = aegisub.get_frame(0, false), aegisub.get_frame(0, true)
  local l = subs[sel[1]]
  l.text = f:width() .. "x" .. f:height() .. " " .. tostring(aegisub.get_frame(1e9) == nil)
    .. " " .. f:getPixelFormatted(0, 0) .. " " .. g:getPixelFormatted(0, 0)
  subs[sel[1]] = l
end)"#,
        )
        .unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        a.set_cache_dir(dir.join("cache").to_str().unwrap());
        a.load_script(script.to_str().unwrap()).unwrap();
        let mut s = Session::default();
        let video = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/media/cfr.mkv");
        s.set_project_value("Video File", video);
        let v = Video::open(video, dir.join("cache").to_str().unwrap(), |_, _| true).unwrap();
        let i = v.info();
        s.set_video_context(i.width, i.height, i.sar_num, i.sar_den, &v.timecodes(), &[]);
        // a full-screen white box from 0 s, so the subtitle frame's corner differs
        let mut r = s.row(0);
        r.text =
            r"{\an7\pos(0,0)\p1\1c&HFFFFFF&\bord0\shad0}m 0 0 l 4000 0 4000 4000 0 4000".into();
        r.end = 10_000;
        s.set_row(0, &r, "");
        run(&a, 0, &mut s, &[0]);
        let t = s.row(0).text;
        let parts: Vec<&str> = t.split(' ').collect();
        assert!(parts[0].contains('x') && parts[1] == "true", "{t}");
        // libass may apply a colour-matrix tweak, so "white" is only checked as bright
        let red = |p: &str| u8::from_str_radix(&p[6..8], 16).unwrap();
        assert!(red(parts[2]) < 0x40 && red(parts[3]) > 0xC0, "{t}");
    }
}
