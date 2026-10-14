//! Automation 4 scripts for the GUI: autoload, the macro list and running a
//! macro on a session (each script undo point becomes an undo step).

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use cxx::UniquePtr;
use fs_automation::{
    Context, Dialog, DialogControl, DialogResult, DialogValue, Env, Host, Paths, RunError, Script,
};

use crate::Session;
use crate::ffi::{AutomationHost, DialogControlData, DialogData, MacroItem, MacroResult};

struct CppHost(UniquePtr<AutomationHost>);
// SAFETY: scripts run synchronously on the GUI thread (see `run_macro`), the
// only thread that ever calls into the host.
unsafe impl Send for CppHost {}
unsafe impl Sync for CppHost {}

#[derive(Default)]
struct GuiHost {
    cpp: Mutex<Option<CppHost>>,
    log: Mutex<String>,
}

impl GuiHost {
    fn with<R>(&self, f: impl FnOnce(&AutomationHost) -> R) -> Option<R> {
        let g = self.cpp.lock().unwrap_or_else(|e| e.into_inner());
        g.as_ref().and_then(|h| h.0.as_ref()).map(f)
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
    // shortcut: aegisub.get_frame returns nil in the GUI; wire it to the open Video when a script needs it.
}

pub struct Automation {
    env: Env,
    host: Arc<GuiHost>,
    scripts: Vec<Script>,
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

    pub fn set_host(&mut self, host: UniquePtr<AutomationHost>) {
        *self.host.cpp.lock().unwrap_or_else(|e| e.into_inner()) = Some(CppHost(host));
    }

    fn index(&mut self) {
        self.macros = self
            .scripts
            .iter()
            .enumerate()
            .flat_map(|(s, sc)| (0..sc.macros.len()).map(move |m| (s, m)))
            .collect();
    }

    fn add(&mut self, path: &Path) -> Result<(), String> {
        let s = Script::load(path, &self.env);
        let err = s.error.clone();
        self.scripts.retain(|o| o.path != s.path);
        self.scripts.push(s);
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

    pub fn scripts(&self) -> Vec<String> {
        self.scripts
            .iter()
            .map(|s| s.path.to_string_lossy().into_owned())
            .collect()
    }

    /// Reload every loaded script from disk.
    pub fn reload(&mut self) -> Vec<String> {
        let paths: Vec<PathBuf> = self.scripts.drain(..).map(|s| s.path).collect();
        paths.iter().filter_map(|p| self.add(p).err()).collect()
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

    fn context(session: &Session) -> Context {
        let info = |k: &str| {
            let v = session.info_value(k);
            (!v.is_empty()).then(|| PathBuf::from(v))
        };
        Context {
            subtitle_file: session.path.clone(),
            video_file: info("Video File"),
            audio_file: info("Audio File"),
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

    /// Runs on the calling (GUI) thread so dialogs can be modal.
    // shortcut: the UI is blocked while a script runs (no live progress repaint); move to a worker thread with a marshalling host if long scripts need it.
    pub fn run_macro(
        &mut self,
        id: usize,
        session: &mut Session,
        rows: &[usize],
        active: i64,
    ) -> Result<MacroResult, String> {
        let &(s, m) = self.macros.get(id).ok_or("no such macro")?;
        let (sel, act) = Self::selection(session, rows, active);
        let ctx = Self::context(session);
        let out = match self.scripts[s].run_macro(m, &session.ed.doc, &sel, act, &ctx) {
            Ok(o) => o,
            Err(RunError::Cancelled) => return Ok(MacroResult::default()),
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
        Ok(MacroResult {
            changed,
            selection: rows,
            active,
        })
    }

    pub fn take_log(&self) -> String {
        std::mem::take(&mut *self.host.log.lock().unwrap_or_else(|e| e.into_inner()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
        let res = a.run_macro(0, &mut s, &[0], 0).unwrap();
        assert!(res.changed);
        assert_eq!(res.selection, vec![0]);
        assert_eq!(s.row(0).text, "ABC");
        assert!(s.undo());
        assert_eq!(s.row(0).text, "abc");
    }
}
