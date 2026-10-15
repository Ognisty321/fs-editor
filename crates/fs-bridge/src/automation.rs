//! Automation 4 scripts for the GUI: autoload, the macro list, running a
//! macro on a session (each script undo point becomes an undo step) and
//! export filters. Scripts run on a worker thread (`run_script_job`).

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use cxx::UniquePtr;
use fs_automation::{
    Clipboard, Context, Dialog, DialogControl, DialogResult, DialogValue, EditState, Env, Host,
    Interrupt, MacroOutcome, Paths, RunError, Script, VideoFrame,
};
use fs_core::Document;

use crate::Session;
use crate::ffi::{
    AutomationHost, DialogControlData, DialogData, ExportSpec, MacroContext, MacroItem,
    MacroResult, ScriptItem,
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
        self.log
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .push_str(text);
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
        let data = DialogData {
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
        };
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

/// A macro (`start_macro` .. `finish_macro`) or export filters (`start_export` ..
/// `finish_export`); `run_script_job` runs the scripts on a worker thread.
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
        filters: Vec<(Arc<Script>, usize)>,
        out: Option<Result<Document, RunError>>,
    },
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
        Work::Export { filters, out } => *out = Some(run_filters(host, filters, doc, ctx)),
    }
    host.set_running(None);
}

/// Aegisub's Export: each chosen filter asks for its settings, then runs on the copy.
// shortcut: a filter's settings are asked in its own dialog at export time, not inline in the export dialog; inline them if users want to set them up front.
fn run_filters(
    host: &GuiHost,
    filters: &[(Arc<Script>, usize)],
    doc: &Document,
    ctx: &Context,
) -> Result<Document, RunError> {
    let mut doc = doc.clone();
    for (script, f) in filters {
        host.set_running(script.interrupter());
        let mut settings = Default::default();
        let config = script.filter_config(*f, &doc, ctx).inspect_err(|e| {
            if let RunError::Lua(_) = e {
                host.log(&format!("{e}\n"));
            }
        })?;
        if let Some(d) = config {
            let r = host.dialog(&d);
            if r.button.is_none() {
                return Err(RunError::Cancelled);
            }
            settings = r.values;
        }
        doc = script.run_filter(*f, &doc, &settings, ctx)?;
    }
    Ok(doc)
}

pub struct Automation {
    env: Env,
    host: Arc<GuiHost>,
    scripts: Vec<Arc<Script>>,
    /// Flat macro list: (script index, macro index).
    macros: Vec<(usize, usize)>,
    /// Flat export filter list, highest priority first like Aegisub's.
    filters: Vec<(usize, usize)>,
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
            filters: Vec::new(),
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
        self.filters = self
            .scripts
            .iter()
            .enumerate()
            .flat_map(|(s, sc)| (0..sc.filters.len()).map(move |f| (s, f)))
            .collect();
        let prio = |&(s, f): &(usize, usize)| -self.scripts[s].filters[f].priority;
        self.filters.sort_by_key(prio);
    }

    fn add(&mut self, path: &Path) -> Result<(), String> {
        let s = Script::load(path, &self.env);
        let err = s.error.clone();
        self.scripts.retain(|o| o.path != s.path);
        self.scripts.push(Arc::new(s));
        self.index();
        err.map_or(Ok(()), |e| Err(format!("{}: {e}", path.display())))
    }

    /// Load every script in the autoload dirs; returns the load errors.
    pub fn load_autoload(&mut self) -> Vec<String> {
        fs_automation::autoload_files(&self.env.paths)
            .iter()
            .filter_map(|p| self.add(p).err())
            .collect()
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
        paths.iter().filter_map(|p| self.add(p).err()).collect()
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

    pub fn filters(&self) -> Vec<MacroItem> {
        self.filters
            .iter()
            .enumerate()
            .map(|(id, &(s, f))| MacroItem {
                id,
                script: self.scripts[s].name.clone(),
                name: self.scripts[s].filters[f].name.clone(),
                description: self.scripts[s].filters[f].description.clone(),
            })
            .collect()
    }

    /// Export filters `ids` (of `filters()`) to run on a copy, in priority order (GUI thread).
    pub fn start_export(&self, ids: &[usize], session: &Session) -> Box<ScriptJob> {
        let filters = (self.filters.iter().enumerate())
            .filter(|(id, _)| ids.contains(id))
            .map(|(_, &(s, f))| (self.scripts[s].clone(), f))
            .collect();
        Box::new(ScriptJob {
            host: self.host.clone(),
            doc: session.ed.doc.clone(),
            ctx: Self::context(session),
            work: Work::Export { filters, out: None },
        })
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
        match out.unwrap_or(Err(RunError::Cancelled)) {
            Ok(doc) => session.export_doc(doc, path, spec, video).map(|()| true),
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
            video_size: session
                .video_size
                .map(|(w, h)| (w, h, w as f64 / h as f64, 0)),
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

    pub fn macro_enabled(&self, id: usize, session: &Session, rows: &[usize], active: i64) -> bool {
        let Some(&(s, m)) = self.macros.get(id) else {
            return false;
        };
        let (sel, act) = Self::selection(session, rows, active);
        self.scripts[s]
            .validate_macro(m, &session.ed.doc, &sel, act, &Self::context(session))
            .0
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

    fn run(a: &Automation, id: usize, s: &mut Session, rows: &[usize]) -> MacroResult {
        let mut job = a
            .start_macro(id, s, rows, 0, &MacroContext::default())
            .unwrap();
        run_script_job(&mut job);
        a.finish_macro(job, s).unwrap()
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
        assert!(a.macro_enabled(0, &s, &[0], 0));
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
        let macro_lua = |name: &str| {
            format!(r#"script_name="{name}" aegisub.register_macro("{name}", "", function() end)"#)
        };
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
aegisub.register_filter("Early", "", 9, function(subs) local l = subs[#subs]; l.text = l.text .. "?"; subs[#subs] = l end)"#,
        )
        .unwrap();
        let data = concat!(env!("CARGO_MANIFEST_DIR"), "/../fs-automation");
        let mut a = Automation::new(data, dir.join("user").to_str().unwrap());
        a.load_script(script.to_str().unwrap()).unwrap();
        let names: Vec<String> = a.filters().into_iter().map(|f| f.name).collect();
        assert_eq!(names, ["Early", "Late"]);
        let mut s = Session::default();
        let mut r = s.row(0);
        r.text = "x".into();
        s.set_row(0, &r, "");
        let out = dir.join("out.ass");
        let export = |a: &Automation, ids: &[usize]| {
            let mut job = a.start_export(ids, &s);
            run_script_job(&mut job);
            let spec = ExportSpec::default();
            a.finish_export(job, &s, out.to_str().unwrap(), &spec, &[])
                .unwrap()
        };
        assert!(export(&a, &[0, 1]));
        assert!(std::fs::read_to_string(&out).unwrap().contains(",x?!"));
        assert_eq!(s.row(0).text, "x");
        // a failing filter exports nothing and leaves its error in the log
        std::fs::write(
            &script,
            r#"script_name="F"
aegisub.register_filter("Bad", "", 1, function() error("broken") end)"#,
        )
        .unwrap();
        a.reload_script(0).unwrap();
        std::fs::remove_file(&out).unwrap();
        assert!(!export(&a, &[0]));
        assert!(!out.exists() && a.take_log().contains("broken"));
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
        s.set_video_context(i.width, i.height, &v.timecodes(), &[]);
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
