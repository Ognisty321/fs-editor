//! Runs Aegisub's own automation tests and bundled scripts through the host:
//! the busted specs for aegisub.re/unicode/lfs/argcheck, the Automation 4
//! test macros, and kara-templater on Aegisub's karaoke test files.

use fs_automation::{Context, Dialog, DialogResult, Env, Host, Paths, RunError, Script};
use fs_core::Document;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

fn crate_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

/// Logs, answers dialogs with their defaults (keeping the last one asked) and
/// says Cancel was pressed while `cancelled` is set.
#[derive(Default)]
struct LogHost(Mutex<String>, AtomicBool, Mutex<Option<Dialog>>);

impl Host for LogHost {
    fn log(&self, text: &str) {
        self.0.lock().unwrap().push_str(text);
    }
    fn is_cancelled(&self) -> bool {
        self.1.load(Ordering::Relaxed)
    }
    fn dialog(&self, dialog: &Dialog) -> DialogResult {
        *self.2.lock().unwrap() = Some(dialog.clone());
        dialog.accept_defaults()
    }
}

struct Fixture {
    _user: tempfile::TempDir,
    host: Arc<LogHost>,
    env: Env,
}

fn fixture() -> Fixture {
    let user = tempfile::tempdir().unwrap();
    let host = Arc::new(LogHost::default());
    let env = Env::new(
        Paths::new(crate_dir(), user.path().to_path_buf()),
        host.clone(),
    );
    Fixture {
        _user: user,
        host,
        env,
    }
}

impl Fixture {
    fn log(&self) -> String {
        self.host.0.lock().unwrap().clone()
    }

    fn load(&self, path: &Path) -> Script {
        let s = Script::load(path, &self.env);
        assert!(
            s.error.is_none(),
            "{}: {}",
            path.display(),
            s.error.unwrap()
        );
        s
    }

    /// A script written into a temp dir next to nothing else.
    fn script(&self, dir: &Path, name: &str, code: &str) -> Script {
        let p = dir.join(name);
        std::fs::write(&p, code).unwrap();
        self.load(&p)
    }
}

fn aegisub_test(name: &str) -> PathBuf {
    crate_dir().join("tests/aegisub/automation").join(name)
}

fn doc(name: &str) -> Document {
    Document::parse(&std::fs::read_to_string(aegisub_test(name)).unwrap())
}

fn run_spec(f: &Fixture, host_dir: &Path, spec: &str) {
    let s = f.script(host_dir, "spec-host.lua", "script_name = 'spec host'\n");
    let runner = crate_dir().join("tests/support/mini_busted.lua");
    // argcheck's spec expects error locations inside `tests/modules/<spec>`,
    // the layout of Aegisub's source tree.
    let dst = host_dir.join("tests/modules");
    std::fs::create_dir_all(&dst).unwrap();
    std::fs::copy(
        crate_dir().join("tests/aegisub/modules").join(spec),
        dst.join(spec),
    )
    .unwrap();
    let spec = PathBuf::from(dst.join(spec).to_string_lossy().replace('\\', "/"));
    let code = format!(
        "return assert(loadstring(io.open({:?}):read('*a'), 'mini_busted'))({:?})",
        runner.to_string_lossy(),
        spec.to_string_lossy()
    );
    let out = s.eval(&code).unwrap_or_else(|e| panic!("{spec:?}: {e}"));
    assert_eq!(
        out[1],
        "0",
        "{} failures in {}:\n{}",
        out[1],
        spec.display(),
        out[2]
    );
    assert!(out[0].parse::<i32>().unwrap() > 0);
}

#[test]
fn aegisub_module_specs() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    for spec in ["re.moon", "unicode.moon", "argcheck.moon"] {
        run_spec(&f, dir.path(), spec);
    }
}

// The lfs spec compares against `pwd` and chdir's to `/`, which only makes
// sense on POSIX shells (Aegisub runs it on Linux CI).
#[cfg(unix)]
#[test]
fn aegisub_lfs_spec() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    run_spec(&f, dir.path(), "lfs.moon");
}

#[test]
fn lfs_semantics() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(dir.path(), "lfs-host.lua", "lfs = require 'aegisub.lfs'\n");
    let base = dir.path().join("a b ż");
    let code = format!(
        r#"
        local base = {:?}
        assert(lfs.mkdir(base .. '/x/y') == true)
        assert(lfs.mkdir(base .. '/x/y') == true)        -- existing dir is not an error
        assert(lfs.touch(base .. '/f.txt') == true)
        assert(lfs.attributes(base, 'mode') == 'directory')
        local a = lfs.attributes(base .. '/f.txt')
        assert(a.mode == 'file' and a.size == 0 and a.modification > 0)
        local n, e = lfs.attributes(base)                 -- Aegisub quirk: dirs have no size
        assert(n == nil and e:find('not a file'))
        assert(select('#', lfs.attributes(base .. '/missing')) >= 1 and lfs.attributes(base .. '/missing') == nil)
        local names = {{}}
        for name in lfs.dir(base) do names[#names + 1] = name end
        table.sort(names)
        assert(#names == 2 and names[1] == 'f.txt' and names[2] == 'x', table.concat(names, ','))
        local ok, err = pcall(lfs.dir, base .. '/missing')
        assert(not ok and err:find('cannot open', 1, true))
        local r, msg = lfs.rmdir(base .. '/x')
        assert(r == nil and msg)
        assert(lfs.rmdir(base .. '/x/y') and lfs.rmdir(base .. '/x') and lfs.rmdir(base .. '/f.txt'))
        local cwd = lfs.currentdir()
        assert(lfs.chdir(base) and lfs.currentdir() ~= cwd)
        assert(lfs.chdir(cwd))
        -- unicode-monkeypatch makes io.open take UTF-8 names on Windows
        local fh = assert(io.open(base .. '/żółw.txt', 'w')); fh:write('x'); fh:close()
        assert(lfs.attributes(base .. '/żółw.txt', 'size') == 1)
        return 'ok'
    "#,
        base.to_string_lossy()
    );
    assert_eq!(s.eval(&code).unwrap(), vec!["ok"]);
}

#[test]
fn runtime_environment_matches_aegisub() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    std::fs::write(
        dir.path().join("helper.moon"),
        "x = 41\n{value: x + 1, :x}\n",
    )
    .unwrap();
    std::fs::write(dir.path().join("inc.lua"), "return 'included', ...\n").unwrap();
    let s = f.script(
        dir.path(),
        "env.lua",
        r#"
        script_name = 'env test'
        script_version = 7
        source = debug.getinfo(1, 'S').source
        function test()
          local ok, err = pcall(error, 'boom')
          assert(dofile == nil and loadfile == nil)
          assert(require('helper').value == 42)                       -- .moon module via package.loaders
          assert(include('inc.lua') == 'included')
          assert(require('moonscript.version').version == '0.7.0')
          assert(require('lpeg').match(require('lpeg').P'a', 'a') == 2)
          local proxy = setmetatable({}, {__pairs = function() return function(_, k) if not k then return 1, 'x' end end, nil, nil end})
          for k, v in pairs(proxy) do assert(k == 1 and v == 'x') end   -- LUA52COMPAT
          assert(aegisub.__depCtrl == nil)
          assert(aegisub.lua_automation_version == 4)
          assert(aegisub.log == nil and aegisub.progress == nil)      -- only during runs
          assert(aegisub.video_size() == nil and aegisub.frame_from_ms(100) == nil)
          assert(aegisub.decode_path('?user/x') ~= '?user/x')
          local u = require 'aegisub.unicode'
          assert(u.to_upper_case('straße żółw') == 'STRASSE ŻÓŁW' and u.to_fold_case('ΣΑΣ') == 'σασ' and u.len('żółw') == 4)
          local re = require 'aegisub.re'
          assert(re.sub('Ala ma kota', [[(\w+) (\w+)]], [[$2 \U$1]], 1) == 'ma ALA kota')
          local c = require 'aegisub.clipboard'
          assert(c.get() == nil and c.set('x') == false)
          return 'ok'
        end
        "#,
    );
    assert_eq!(s.name, "env test");
    assert_eq!(s.version, "7");
    assert_eq!(s.eval("return test()").unwrap(), vec!["ok"]);
    // chunk names are plain paths without '@'
    let src = s.eval("return source").unwrap();
    assert!(
        src[0].ends_with("env.lua") && !src[0].starts_with('@'),
        "{src:?}"
    );
}

#[test]
fn errors_carry_aegisub_stack_traces() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "broken.lua",
        "function inner() error('kaboom') end\nfunction m() inner() end\naegisub.register_macro('Broken', '', m)\n",
    );
    let d = doc("kara-templater-retime.ass");
    let Err(RunError::Lua(msg)) = s.run_macro(0, &d, &[0], Some(0), &Context::default()) else {
        panic!("expected a Lua error")
    };
    // Aegisub prints the frames outermost first and the message last
    assert!(msg.ends_with("\nkaboom"), "{msg}");
    assert!(msg.contains("broken.lua\", line 1\ninner"), "{msg}");
    assert!(f.log().contains("Lua reported a runtime error"));

    let bad = Script::load(&aegisub_test("missing-file.lua"), &f.env);
    assert!(bad.error.is_some());
    let syntax = dir.path().join("syntax.lua");
    std::fs::write(&syntax, "function (").unwrap();
    assert!(Script::load(&syntax, &f.env).error.is_some());
}

#[test]
fn aegisub_test_scripts_load() {
    let f = fixture();
    let mut n = 0;
    for e in std::fs::read_dir(crate_dir().join("tests/aegisub/automation")).unwrap() {
        let p = e.unwrap().path();
        if p.extension().is_some_and(|x| x == "lua") {
            let s = f.load(&p);
            assert!(
                !s.macros.is_empty() || !s.filters.is_empty(),
                "{}",
                p.display()
            );
            n += 1;
        }
    }
    assert!(n >= 15);
    for e in std::fs::read_dir(crate_dir().join("automation/autoload")).unwrap() {
        let s = f.load(&e.unwrap().path());
        assert!(!s.macros.is_empty() || !s.filters.is_empty());
    }
}

#[test]
fn hello_world_and_debug_levels() {
    let f = fixture();
    let s = f.load(&aegisub_test("hello-world.lua"));
    assert_eq!(s.macros[0].name, "Hello");
    assert_eq!(s.command_id(0), "automation/lua/hello-world/Hello");
    let d = doc("kara-templater-retime.ass");
    let out = s
        .run_macro(0, &d, &[0], Some(0), &Context::default())
        .unwrap();
    assert!(out.commits.is_empty());
    assert_eq!(out.selection, vec![0]);
    assert!(f.log().contains("Hello Automation 4 World!"));

    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "levels.lua",
        "aegisub.register_macro('L', '', function() aegisub.debug.out(4, 'hidden') aegisub.log(3, 'shown %d', 5) aegisub.log('plain') end)\n",
    );
    s.run_macro(0, &d, &[0], None, &Context::default()).unwrap();
    let log = f.log();
    assert!(
        log.contains("shown 5plain") && !log.contains("hidden"),
        "{log}"
    );
}

#[test]
fn untouched_lines_round_trip_byte_exact() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "rewrite.lua",
        r#"aegisub.register_macro('Rewrite', '', function(subs)
            for i = 1, #subs do subs[i] = subs[i] end
            local l = subs[#subs]; l.text = l.text .. '!'; subs[#subs] = l
        end)"#,
    );
    for name in ["kara-templater-retime.ass", "furi-test.ass"] {
        let src = std::fs::read_to_string(aegisub_test(name)).unwrap();
        let d = Document::parse(&src);
        let n = d.event_count();
        let out = s
            .run_macro(0, &d, &[0], Some(0), &Context::default())
            .unwrap();
        assert_eq!(out.commits.len(), 1);
        let (desc, new) = &out.commits[0];
        assert_eq!(desc, "Rewrite");
        let text = new.to_ass_string();
        let changed: Vec<_> = src
            .lines()
            .zip(text.lines())
            .filter(|(a, b)| a != b)
            .collect();
        assert_eq!(changed.len(), 1, "{name}: {changed:?}");
        assert!(changed[0].1.ends_with('!'));
        // ids of untouched events survive, so the grid keeps its selection
        assert_eq!(new.event_id(0), d.event_id(0));
        assert_eq!(new.event_count(), n);
    }
}

#[test]
fn selection_set_and_undo_points() {
    let f = fixture();
    let s = f.load(&aegisub_test("selection-set-test.lua"));
    let d = doc("kara-templater-retime.ass");
    let n = d.event_count();
    let row = n - 2; // the karaoke line
    let out = s
        .run_macro(0, &d, &[row], Some(row), &Context::default())
        .unwrap();
    // one undo point with all the changes; nothing after it
    assert_eq!(out.commits.len(), 1);
    assert_eq!(out.commits[0].0, "Insert+select Stuff");
    let new = &out.commits[0].1;
    assert_eq!(new.event_count(), n + 2);
    // the copy inserted before the line got the new text; the original moved down one row
    assert_eq!(new.event(row).unwrap().text, "A4 was here!");
    assert_eq!(new.event(row + 1).unwrap().text, d.event(row).unwrap().text);
    assert_eq!(new.event_id(row + 1), d.event_id(row));
    assert_eq!(new.event(n + 1).unwrap().text, d.event(row).unwrap().text);
    assert_eq!(out.selection, vec![row - 1, row, row + 1]);
    // no active row returned: Aegisub keeps the index that was active, now the copy
    assert_eq!(out.active, Some(row));
}

#[test]
fn delete_many_lines_and_select_by_index() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "delete.lua",
        "aegisub.register_macro('D', '', function(subs, sel) subs.delete(sel) return {sel[1]}, sel[1] end)\n",
    );
    let d = doc("kara-templater-retime.ass");
    let n = d.event_count();
    let out = s
        .run_macro(0, &d, &[0, 2], Some(0), &Context::default())
        .unwrap();
    let new = &out.commits.last().unwrap().1;
    // both lines gone, the rest in order; the first selected index now holds the old second line
    assert_eq!(new.event_count(), n - 2);
    assert_eq!(new.event_id(0), d.event_id(1));
    assert_eq!(new.event_id(n - 3), d.event_id(n - 1));
    assert_eq!(out.selection, vec![0]);
    assert_eq!(out.active, Some(0));
}

#[test]
fn cancel_discards_changes_and_read_only_validate() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "cancel.lua",
        r#"
        aegisub.register_macro('C', '', function(subs) subs.delete(1) aegisub.cancel() end,
          function(subs, sel, act)
            local ok = pcall(function() subs[1] = subs[1] end)
            return not ok and act ~= nil and #sel == 1, 'dynamic help'
          end)
        saved = nil
        aegisub.register_macro('Keep', '', function(subs) saved = subs end)
        function use_saved() return #saved end
        "#,
    );
    let d = doc("kara-templater-retime.ass");
    let ctx = Context::default();
    assert!(matches!(
        s.run_macro(0, &d, &[0], Some(0), &ctx),
        Err(RunError::Cancelled)
    ));
    assert_eq!(
        s.validate_macro(0, &d, &[0], Some(0), &ctx),
        (true, Some("dynamic help".into()))
    );
    assert!(!s.validate_macro(0, &d, &[0], None, &ctx).0);
    s.run_macro(1, &d, &[0], Some(0), &ctx).unwrap();
    let err = s.eval("return use_saved()").unwrap_err().to_string();
    assert!(err.contains("Subtitles object is no longer valid"), "{err}");
}

#[test]
fn karaoke_parse_test_script() {
    let f = fixture();
    let s = f.load(&aegisub_test("karaoke-parse-test.lua"));
    let d = doc("kara-templater-retime.ass");
    let row = d.event_count() - 2;
    let out = s
        .run_macro(0, &d, &[row], Some(row), &Context::default())
        .unwrap();
    let new = &out.commits[0].1;
    let texts: Vec<_> = (row + 1..row + 4)
        .map(|i| new.event(i).unwrap().text.clone())
        .collect();
    assert_eq!(
        texts,
        vec![
            r"{\t(0,300,\fscx50)}ha",
            r"{\t(300,500,\fscx50)}hi",
            r"{\t(500,1000,\fscx50)}ho"
        ]
    );
}

#[test]
fn kara_templater_retime() {
    let f = fixture();
    let s = f.load(&crate_dir().join("automation/autoload/kara-templater.lua"));
    let apply = s
        .macros
        .iter()
        .position(|m| m.name == "Apply karaoke template")
        .unwrap();
    let d = doc("kara-templater-retime.ass");
    let n = d.event_count();
    let ctx = Context::default();
    assert!(s.validate_macro(apply, &d, &[], None, &ctx).0);
    let out = s.run_macro(apply, &d, &[n - 2], Some(n - 2), &ctx).unwrap();
    assert!(!f.log().contains("error"), "{}", f.log());
    let new = &out.commits.last().unwrap().1;
    let fx: Vec<_> = new
        .events()
        .iter()
        .map(|e| &e.event)
        .filter(|e| e.effect == "fx")
        .collect();
    assert_eq!(fx.len(), 27, "9 templates x 3 syllables");
    let by_color = |c: &str| -> Vec<(i64, i64)> {
        fx.iter()
            .filter(|e| e.text.contains(c))
            .map(|e| (e.start.0, e.end.0))
            .collect()
    };
    assert_eq!(by_color(r"\1c&H0000FF&"), vec![(1000, 2000); 3]); // abs
    assert_eq!(
        by_color(r"\1c&H00FF00&"),
        vec![(10000, 10300), (10300, 10500), (10500, 11000)]
    ); // syl
    assert_eq!(by_color(r"\1c&H000000&"), vec![(9900, 10000); 3]); // preline
    assert!(fx.iter().all(|e| e.text.starts_with(r"{\pos(")));
    let kara = new
        .events()
        .iter()
        .find(|e| e.event.text.contains(r"{\k30}ha"))
        .unwrap();
    assert!(kara.event.is_comment() && kara.event.effect == "karaoke");
    // running again replaces the fx lines instead of adding more
    let again = s.run_macro(apply, new, &[0], Some(0), &ctx).unwrap();
    let new2 = &again.commits.last().unwrap().1;
    assert_eq!(
        new2.events()
            .iter()
            .filter(|e| e.event.effect == "fx")
            .count(),
        27
    );
}

#[test]
fn karaskel_furigana_layout() {
    let f = fixture();
    f.env.trace_level.store(5, Ordering::Relaxed);
    let s = f.load(&aegisub_test("test-furi.lua"));
    let d = doc("furi-test.ass");
    let out = s
        .run_macro(0, &d, &[0], Some(0), &Context::default())
        .unwrap();
    let log = f.log();
    assert!(log.contains("Karaoke syllables"), "{log}");
    assert!(
        log.contains("Syllable: text='ふ'") || log.contains("stripped='振'"),
        "{log}"
    );
    // the furigana style already exists, so nothing new is inserted for it
    if let Some((_, new)) = out.commits.last() {
        assert!(new.style("Default-furigana").is_some());
    }
}

#[test]
fn legacy_line_classes_are_rejected_like_aegisub() {
    // appended-lines.lua predates Aegisub 3: it appends `head` lines, which
    // modern Aegisub rejects with the same message.
    let f = fixture();
    let s = f.load(&aegisub_test("appended-lines.lua"));
    let d = doc("kara-templater-retime.ass");
    let Err(RunError::Lua(msg)) = s.run_macro(0, &d, &[0], Some(0), &Context::default()) else {
        panic!("expected an error")
    };
    assert!(
        msg.ends_with("Found line with unknown class: head"),
        "{msg}"
    );

    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "append.lua",
        r#"aegisub.register_macro('A', '', function(subs)
            subs.append({class = 'info', key = 'Added', value = '1'})
            local st = subs[1]; for i = 1, #subs do if subs[i].class == 'style' then st = subs[i] break end end
            st.name = 'New'; subs.append(st)
            local ev; for i = 1, #subs do if subs[i].class == 'dialogue' then ev = subs[i] end end
            ev.text = 'last'; subs[0] = ev
            subs.insert(1, ev)   -- a dialogue inserted at the top still ends up in [Events]
        end)"#,
    );
    let out = s
        .run_macro(0, &d, &[0], Some(0), &Context::default())
        .unwrap();
    let new = &out.commits.last().unwrap().1;
    assert_eq!(new.info("Added"), Some("1"));
    assert!(new.style("New").is_some());
    assert_eq!(new.event_count(), d.event_count() + 2);
    assert_eq!(new.event(0).unwrap().text, "last");
    assert_eq!(new.event(new.event_count() - 1).unwrap().text, "last");
    assert_eq!(
        Document::parse(&new.to_ass_string()).to_ass_string(),
        new.to_ass_string()
    );
}

#[test]
fn text_extents_from_lua_and_dialogs() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "ext.lua",
        r#"
        include 'karaskel.lua'
        aegisub.register_macro('E', '', function(subs)
          local meta, styles = karaskel.collect_head(subs, false)
          local w, h, d, e = aegisub.text_extents(styles.Default, 'Hello')
          assert(w > 50 and h > 40 and d > 0 and e >= 0, ('%f %f %f %f'):format(w, h, d, e))
          assert(not pcall(aegisub.text_extents, {class = 'dialogue'}, 'x'))
          local btn, res = aegisub.dialog.display({
            {class = 'label', label = 'x'},
            {class = 'intedit', name = 'n', value = 7},
            {class = 'checkbox', name = 'c', value = true},
            {class = 'color', name = 'col', value = '&H0000FF&'},
          }, {'Go', 'Stop'}, {ok = 'Go', cancel = 'Stop'})
          assert(btn == 'Go' and res.n == 7 and res.c == true and res.col == '#FF0000', tostring(res.col))
          local btn2 = aegisub.dialog.display({{class = 'edit', name = 'e'}})
          assert(btn2 == '')
        end)
        "#,
    );
    let d = doc("kara-templater-retime.ass");
    s.run_macro(0, &d, &[0], Some(0), &Context::default())
        .unwrap();
}

#[test]
fn export_filters_and_context() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "filter.lua",
        r#"
        aegisub.register_filter('Upper', 'desc', 100, function(subs, cfg)
          assert(aegisub.dialog == nil)
          assert(not pcall(aegisub.set_undo_point, 'x'))
          for i = 1, #subs do
            local l = subs[i]
            if l.class == 'dialogue' then l.text = l.text:upper() .. (cfg.suffix or '') subs[i] = l end
          end
        end, function(subs, stored)
          return {{class = 'edit', name = 'suffix', value = '?'}}
        end)
        aegisub.register_macro('Ctx', '', function()
          local w, h = aegisub.video_size()
          assert(w == 640 and h == 480)
          assert(aegisub.ms_from_frame(1) == 21 and aegisub.frame_from_ms(21) == 1)
          assert(#aegisub.keyframes() == 2 and aegisub.keyframes()[2] == 48)
          assert(aegisub.file_name() == 'ep01.ass')
          assert(aegisub.project_properties().video_position == 500)
          local a, b = aegisub.get_audio_selection()
          assert(a == 100 and b == 200)
          aegisub.gui.set_cursor(3)
        end)
        "#,
    );
    let d = doc("kara-templater-retime.ass");
    let ctx = Context {
        subtitle_file: Some(dir.path().join("ep01.ass")),
        video_size: Some((640, 480, 4.0 / 3.0, 0)),
        framerate: Some(fs_core::vfr::Framerate::from_rational(24000, 1001, false).unwrap()),
        keyframes: vec![0, 48],
        audio_selection: Some((100, 200)),
        ..Default::default()
    };
    let cfg = s.filter_config(0, &d, &ctx).unwrap().unwrap();
    assert_eq!(cfg.controls.len(), 1);
    let settings = cfg.accept_defaults().values;
    let out = s.run_filter(0, &d, &settings, &ctx).unwrap();
    assert!(
        out.events()
            .iter()
            .any(|e| e.event.text == r"{\K30}HA{\K20}HI{\K50}HO?")
    );
    let r = s.run_macro(0, &d, &[0], Some(0), &ctx).unwrap();
    assert_eq!(r.edit.map(|e| e.cursor), Some(3));
}

/// luabins as Aegisub ships it: the real module, with its deprecation warning on load.
#[test]
fn luabins_round_trips_with_aegisub_warning() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "bins.lua",
        r#"
        local luabins = require 'luabins'
        local saved = luabins.save(1.5, "x\0y", true, nil, {a = {2, 3}})
        local ok, n, str, b, none, t = luabins.load(saved)
        assert(ok and n == 1.5 and str == "x\0y" and b == true and none == nil and t.a[2] == 3)
        assert(not luabins.load("junk"))
        assert(luabins._VERSION and require('luabins') == luabins)
        aegisub.register_macro('Bins', '', function() end)
        "#,
    );
    assert_eq!(s.macros.len(), 1);
    let log = f.log();
    assert!(log.contains("Warning in Automation script 'bins'"), "{log}");
    assert!(log.contains("uses the library luabins"), "{log}");
}

/// A filter's settings as Aegisub keeps them for the file between exports: serialised in
/// control order, put back into the next config dialog by name.
#[test]
fn filter_settings_round_trip_like_aegisub() {
    use fs_automation::{
        DialogValue, inline_string_decode, inline_string_encode, serialise_settings,
    };
    assert_eq!(inline_string_encode("a|b:c,#d\ne"), "a#7Cb#3Ac#2C#23d#0Ae");
    assert_eq!(inline_string_decode("a#7Cb#3Ac#2C#23d#0Ae"), "a|b:c,#d\ne");
    assert_eq!(inline_string_decode("zażółć #"), "zażółć #");
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "cfg.lua",
        r#"
        aegisub.register_filter('Cfg', '', 0, function() end, function()
          return {
            {class = 'label', label = 'x'},
            {class = 'edit', name = 'text', value = ''},
            {class = 'intedit', name = 'n', value = 1},
            {class = 'floatedit', name = 'f', value = 0},
            {class = 'checkbox', name = 'on', value = false},
            {class = 'dropdown', name = 'pick', items = {'a', 'b|c'}, value = 'a'},
            {class = 'coloralpha', name = 'col', value = '#000000'},
          }
        end)
        "#,
    );
    let d = doc("kara-templater-retime.ass");
    let ctx = Context::default();
    let first = s.filter_config(0, &d, &ctx).unwrap().unwrap();
    assert_eq!(
        first.accept_defaults().values["pick"],
        DialogValue::Text("a".into())
    );
    let set = [
        ("text", DialogValue::Text("a:b|c, ż".into())),
        ("n", DialogValue::Int(-7)),
        ("f", DialogValue::Float(0.1234567)),
        ("on", DialogValue::Bool(true)),
        ("pick", DialogValue::Text("b|c".into())),
        ("col", DialogValue::Color("#10203040".into())),
    ];
    let stored = serialise_settings(set.iter().map(|(n, v)| (*n, v)));
    assert_eq!(
        stored,
        "text:a#3Ab#7Cc#2C ż|n:-7|f:0.1234567|on:1|pick:b#7Cc|col:#2310203040"
    );
    let mut next = s.filter_config(0, &d, &ctx).unwrap().unwrap();
    next.unserialise(&stored);
    let values = next.accept_defaults().values;
    for (name, v) in &set {
        assert_eq!(values.get(*name), Some(v), "{name}");
    }
    // a name the dialog no longer has, or a value that is no number, is no harm
    next.unserialise("gone:1|n:x|bad");
    assert_eq!(next.accept_defaults().values["n"], DialogValue::Int(0));
}

#[test]
fn moonscript_autoload_and_paths() {
    let f = fixture();
    let files = fs_automation::autoload_files(&f.env.paths);
    assert!(files.iter().any(|p| p.ends_with("select-overlaps.moon")));
    let s = f.load(
        files
            .iter()
            .find(|p| p.ends_with("select-overlaps.moon"))
            .unwrap(),
    );
    assert_eq!(s.macros.len(), 1);
    let p = &f.env.paths;
    let user = p.user.to_string_lossy().into_owned();
    let sep = std::path::MAIN_SEPARATOR;
    assert_eq!(p.decode("?user", None), user);
    assert_eq!(p.decode("?user/a/b", None), format!("{user}{sep}a{sep}b"));
    assert_eq!(p.decode("?script/x", None), format!("?script{sep}x"));
    assert_eq!(p.decode("plain/path", None), format!("plain{sep}path"));
}

fn copy_tree(src: &Path, dst: &Path) {
    std::fs::create_dir_all(dst).unwrap();
    for e in std::fs::read_dir(src).unwrap() {
        let e = e.unwrap();
        let to = dst.join(e.file_name());
        if e.file_type().unwrap().is_dir() {
            copy_tree(&e.path(), &to);
        } else {
            std::fs::copy(e.path(), to).unwrap();
        }
    }
}

/// DependencyControl smoke test against a checkout named by FS_DEPCTRL_DIR
/// (github.com/TypesettingTools/DependencyControl); skipped when unset.
#[test]
fn dependency_control_toolbox_loads() {
    let Ok(dir) = std::env::var("FS_DEPCTRL_DIR") else {
        eprintln!("FS_DEPCTRL_DIR not set, skipping");
        return;
    };
    let dir = PathBuf::from(dir);
    let f = fixture();
    copy_tree(
        &dir.join("modules"),
        &f.env.paths.user.join("automation/include"),
    );
    let auto = f.env.paths.user.join("automation/autoload");
    std::fs::create_dir_all(&auto).unwrap();
    let macro_file = auto.join("l0.DependencyControl.Toolbox.moon");
    std::fs::copy(
        dir.join("macros/l0.DependencyControl.Toolbox.moon"),
        &macro_file,
    )
    .unwrap();
    let s = f.load(&macro_file);
    assert!(!s.macros.is_empty(), "{}", f.log());
    eprintln!("{} macros, log:\n{}", s.macros.len(), f.log());

    // a typical script: versioned through DepCtrl, macro registered by it
    let user = auto.join("fs.DepCtrlUser.lua");
    std::fs::write(
        &user,
        r#"script_name = "DepCtrl user"
script_description = "edits the selected line"
script_version = "0.1.0"
script_namespace = "fs.DepCtrlUser"
local DependencyControl = require "l0.DependencyControl"
local depctrl = DependencyControl{}
depctrl:registerMacro(function(subs, sel)
    local l = subs[sel[1]]; l.text = "via depctrl"; subs[sel[1]] = l
end)
"#,
    )
    .unwrap();
    let s = f.load(&user);
    assert_eq!(s.macros.len(), 1, "{}", f.log());
    let src = std::fs::read_to_string(aegisub_test("furi-test.ass")).unwrap();
    let d = Document::parse(&src);
    let row = d.event_count() - 1;
    let out = s
        .run_macro(0, &d, &[row], Some(row), &Context::default())
        .unwrap_or_else(|e| panic!("{e:?}\n{}", f.log()));
    assert_eq!(out.commits.len(), 1, "{}", f.log());
    assert_eq!(out.commits[0].1.event(row).unwrap().text, "via depctrl");
}

/// Finalizers that call back into the host must run before the state closes
/// (DepCtrl's lock finalizers log; inside `lua_close` that overflowed the stack).
#[test]
fn finalizers_run_before_close() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    // decode_path reaches Rust; inside lua_close it must fail cleanly, and the
    // marker file proves the finalizer still ran with its globals intact
    let gc = "local p, decode, marker = newproxy(true), aegisub.decode_path, aegisub.decode_path('?user/gc.txt')\n\
              getmetatable(p).__gc = function()\n\
                pcall(decode, '?user')\n\
                local f = io.open(marker, 'a') f:write('gc ') f:close()\n\
              end\n\
              keep = p\n";
    let p = dir.path().join("fails.lua");
    std::fs::write(&p, format!("script_name='x'\n{gc}error('boom')\n")).unwrap();
    let s = Script::load(&p, &f.env);
    assert!(
        s.error.as_deref().unwrap().ends_with("boom"),
        "{:?}",
        s.error
    );
    drop(s);
    let s = f.script(dir.path(), "ok.lua", &format!("script_name='x'\n{gc}"));
    drop(s);
    let marks = std::fs::read_to_string(f.env.paths.user.join("gc.txt")).unwrap();
    assert_eq!(marks, "gc gc ");
}

const SMALL: &str = "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 480\n\n[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\nStyle: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,one\nDialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,,two\nDialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,three\n";

#[test]
fn line_fields_extradata_and_insert_like_aegisub() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    // [3 info lines, 1 style, 3 events]: the events are lines 5 to 7
    let s = f.script(
        dir.path(),
        "fields.lua",
        r#"
        aegisub.register_macro('F', '', function(subs, sel, act)
          assert(#sel == 1 and sel[1] == 6 and act == 6)
          local l = subs[5]
          l.text = 0.1 + 0.2
          l.actor = 'A, B'
          l.extra = {k = 'v', n = 5}
          -- fields come through the metatable, as lua_getfield reads them
          subs[5] = setmetatable({}, {__index = l})
          local r = subs[5]
          assert(r.text == '0.3', r.text)
          assert(r.extra.n == '5' and r.extra.k == 'v')
          assert(r.raw:find('A; B', 1, true) and r.raw:match('{=0=1}0%.3$'), r.raw)
          -- inserting after the last line appends: a style lands among the styles
          local style = subs[4]
          style.name = 'Added'
          subs.insert(#subs + 1, style)
          assert(subs[5].class == 'style' and subs[5].name == 'Added')
          local w, h = subs.script_resolution()
          assert(w == 640 and h == 480)
        end)
        "#,
    );
    let d = Document::parse(SMALL);
    let out = s.run_macro(0, &d, &[1], Some(1), &Context::default());
    let out = out.unwrap_or_else(|e| panic!("{e}\n{}", f.log()));
    let new = &out.commits.last().unwrap().1;
    assert_eq!(new.event(0).unwrap().text, "{=0=1}0.3");
    assert_eq!(new.event(0).unwrap().actor, "A, B");
    let text = new.to_ass_string();
    assert!(text.contains(",A; B,") && text.contains("Data: 0,") && text.contains("Data: 1,"));
    assert!(text.contains("Style: Added,"));
    // nothing returned: the selected event keeps its row although a style went in above it
    assert_eq!((out.selection, out.active), (vec![1], Some(1)));
}

#[test]
fn undo_points_commit_what_changed() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "undo.lua",
        r#"
        aegisub.register_macro('U', '', function(subs)
          for i = 5, 7 do
            local l = subs[i]
            l.text = l.text .. '!'
            subs[i] = l
            aegisub.set_undo_point('step ' .. i)
          end
          subs.delete(5)
        end)
        aegisub.register_macro('C', '', function(subs)
          subs.delete(5)
        end)
        "#,
    );
    let d = Document::parse(SMALL);
    let out = s
        .run_macro(0, &d, &[0], Some(0), &Context::default())
        .unwrap();
    let names: Vec<&str> = out.commits.iter().map(|c| c.0.as_str()).collect();
    assert_eq!(names, ["step 5", "step 6", "step 7", "U"]);
    let texts = |doc: &Document| -> Vec<String> {
        doc.events().iter().map(|e| e.event.text.clone()).collect()
    };
    assert_eq!(texts(&out.commits[0].1), ["one!", "two", "three"]);
    assert_eq!(texts(&out.commits[2].1), ["one!", "two!", "three!"]);
    assert_eq!(texts(&out.commits[3].1), ["two!", "three!"]);
    // a commit shares the lines it did not change with the one before
    let (a, b) = (out.commits[0].1.events(), out.commits[1].1.events());
    assert!(Arc::ptr_eq(&a[0], &b[0]) && Arc::ptr_eq(&a[2], &d.events()[2]));
    assert_eq!(a[0].id, d.events()[0].id);

    // Cancel in the progress window drops the changes of a run that ended normally
    f.host.1.store(true, Ordering::Relaxed);
    let r = s.run_macro(1, &d, &[0], Some(0), &Context::default());
    f.host.1.store(false, Ordering::Relaxed);
    assert!(matches!(r, Err(RunError::Cancelled)));
}

#[test]
fn dialog_values_coerce_like_aegisub() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "dlg.lua",
        r#"
        aegisub.register_macro('D', '', function()
          local btn, res = aegisub.dialog.display({
            {class = 'edit', name = 'e', value = 5},
            {class = 'dropdown', name = 'd', items = {1, 2, 3}},
            {class = 'floatedit', name = 'f', step = 0.5, max = 1000, value = 750},
            {class = 'intedit', name = 'i', value = '12'},
            setmetatable({}, {__index = {class = 'checkbox', name = 'c', value = true}}),
          }, {1, 'Cancel'}, {ok = 1, cancel = 'Cancel'})
          assert(btn == '1', tostring(btn))
          assert(res.e == '5' and res.d == '' and res.i == 12 and res.c == true and res.f == 750)
        end)
        "#,
    );
    let d = Document::parse(SMALL);
    let r = s.run_macro(0, &d, &[0], Some(0), &Context::default());
    r.unwrap_or_else(|e| panic!("{e}\n{}", f.log()));
    let dlg = f.host.2.lock().unwrap().clone().unwrap();
    assert_eq!(dlg.controls[1].items, ["", "1", "2", "3"]);
    assert_eq!(
        (dlg.controls[2].min, dlg.controls[2].max),
        (Some(0.0), Some(1000.0))
    );
    assert_eq!(dlg.buttons, ["1", "Cancel"]);
}

#[test]
fn deleting_line_by_line_stays_linear() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    // kara-templater's cleanup: walk the file and delete the fx lines one by one
    let s = f.script(
        dir.path(),
        "cleanup.lua",
        r#"
        aegisub.register_macro('X', '', function(subs)
          local i, n = 1, #subs
          while i <= n do
            local l = subs[i]
            if l.class == 'dialogue' and l.effect == 'fx' then
              subs.delete(i)
              n = n - 1
            else
              i = i + 1
            end
          end
        end)
        "#,
    );
    let mut text = String::from(
        "[Script Info]\nScriptType: v4.00+\n\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n",
    );
    for i in 0..40_000 {
        let fx = if i % 2 == 0 { "fx" } else { "" };
        text += &format!("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,{fx},l{i}\n");
    }
    let d = Document::parse(&text);
    let t = std::time::Instant::now();
    let out = s
        .run_macro(0, &d, &[0], Some(0), &Context::default())
        .unwrap();
    // moving the rest of the file per delete took minutes here
    assert!(t.elapsed().as_secs() < 30, "{:?}", t.elapsed());
    let new = &out.commits[0].1;
    assert_eq!(new.event_count(), 20_000);
    assert_eq!(new.event(0).unwrap().text, "l1");
    assert!(Arc::ptr_eq(&new.events()[0], &d.events()[1]));
}

#[test]
fn regex_group_before_the_search_start_and_absolute_include() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let inc = dir.path().join("inc");
    std::fs::create_dir_all(&inc).unwrap();
    std::fs::write(inc.join("abs.lua"), "return 42\n").unwrap();
    let s = f.script(dir.path(), "re.lua", "script_name = 're'\n");
    // the lookbehind's group lies before where the search starts
    let got = s
        .eval("local re = require 'aegisub.re' local m = re.match('abb', '(?<=(.))b', 3) return m[2].str, m[2].first")
        .unwrap();
    assert_eq!(got, ["b", "2"]);
    let abs = inc
        .join("abs.lua")
        .to_string_lossy()
        .replace(std::path::MAIN_SEPARATOR, "/");
    assert_eq!(s.eval(&format!("return include('{abs}')")).unwrap(), ["42"]);
}

#[test]
fn stop_ends_a_script_that_never_checks_cancel() {
    let f = fixture();
    let dir = tempfile::tempdir().unwrap();
    let s = f.script(
        dir.path(),
        "hang.lua",
        r#"
        aegisub.register_macro('Busy', '', function(subs)
          subs.delete(5)
          local t = os.clock()
          -- swallows errors and never asks is_cancelled; ends on its own after 20 s
          while os.clock() - t < 20 do
            pcall(function() aegisub.progress.set(50) end)
          end
        end)
        aegisub.register_macro('Pure', '', function()
          local t, n = os.clock(), 0
          while os.clock() - t < 20 do n = n + 1 end
        end)
        aegisub.register_macro('Quick', '', function(subs) subs.delete(5) end)
        "#,
    );
    let stop = s.interrupter().unwrap();
    let d = Document::parse(SMALL);
    for i in [0, 1] {
        let t = std::time::Instant::now();
        let r = std::thread::scope(|sc| {
            sc.spawn(|| {
                std::thread::sleep(std::time::Duration::from_millis(300));
                stop.trigger();
            });
            s.run_macro(i, &d, &[0], Some(0), &Context::default())
        });
        assert!(matches!(r, Err(RunError::Cancelled)), "macro {i}");
        assert!(t.elapsed().as_secs() < 10, "macro {i}: {:?}", t.elapsed());
    }
    // the next run is not stopped by the last one's stop
    let out = s
        .run_macro(2, &d, &[0], Some(0), &Context::default())
        .unwrap();
    assert_eq!(out.commits[0].1.event_count(), 2);
    // a stop between runs is forgotten when the next one starts
    stop.trigger();
    assert!(
        s.run_macro(2, &d, &[0], Some(0), &Context::default())
            .is_ok()
    );
    drop(s);
    stop.trigger(); // the state is closed: nothing to stop
}
