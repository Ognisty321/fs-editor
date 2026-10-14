//! Runs Aegisub's own automation tests and bundled scripts through the host:
//! the busted specs for aegisub.re/unicode/lfs/argcheck, the Automation 4
//! test macros, and kara-templater on Aegisub's karaoke test files.

use fs_automation::{Context, Env, Host, Paths, RunError, Script};
use fs_core::Document;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

fn crate_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

#[derive(Default)]
struct LogHost(Mutex<String>);

impl Host for LogHost {
    fn log(&self, text: &str) {
        self.0.lock().unwrap().push_str(text);
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
    // the copy inserted before the line got the new text; the original
    // (still the active line) moved down one row
    assert_eq!(new.event(row).unwrap().text, "A4 was here!");
    assert_eq!(new.event(row + 1).unwrap().text, d.event(row).unwrap().text);
    assert_eq!(new.event_id(row + 1), d.event_id(row));
    assert_eq!(new.event(n + 1).unwrap().text, d.event(row).unwrap().text);
    assert_eq!(out.selection, vec![row - 1, row, row + 1]);
    assert_eq!(out.active, Some(row + 1));
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
    let mut f = fixture();
    f.env.trace_level = 5;
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
