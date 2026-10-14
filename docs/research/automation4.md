# Aegisub Automation 4 (Lua) — implementation spec for FS Editor

Status: research document, written 2026-10-13.
Target: an `mlua` (LuaJIT) host inside FS Editor that runs existing Aegisub
Automation 4 scripts **unchanged**. That includes karaskel/kara-templater,
DependencyControl-managed scripts, and MoonScript scripts.

## 0. Sources and versions examined

| Repo | Branch / commit | Notes |
|---|---|---|
| TypesettingTools/Aegisub | `master` @ `dddc5ba` (2026-10-12), project version **3.5.0** | Active upstream. It has absorbed most of arch1t3cht's work since 3.4.0. |
| arch1t3cht/Aegisub | default branch `feature` @ `9bfd500` (2025-06-15) | The de-facto fork for 2023–2025. It has two Lua APIs that TT master does **not** have: `aegisub.get_frame` and `aegisub.gui.*`. |
| TypesettingTools/DependencyControl | `main` @ `c4101ae` (v0.9.1, 2026-10-13), plus tag `v0.6.4-alpha` (legacy, still widely installed) | |

Key files. Paths are the same in both Aegisub repos unless noted.

- `src/auto4_lua.cpp`: the `aegisub` table, macro and filter registration, threading, and selection handling.
  - https://github.com/TypesettingTools/Aegisub/blob/master/src/auto4_lua.cpp
  - https://github.com/arch1t3cht/Aegisub/blob/feature/src/auto4_lua.cpp
- `src/auto4_lua_assfile.cpp`: the subtitles object and line tables, `parse_karaoke_data`, `set_undo_point`.
- `src/auto4_lua_progresssink.cpp`: `aegisub.progress.*`, `aegisub.debug.out`, `aegisub.log`, `aegisub.dialog.*`.
- `src/auto4_lua_dialog.cpp`: dialog controls.
- `src/auto4_base.cpp`: `CalculateTextExtents`, include paths, autoload, local scripts, trace level.
- `libaegisub/lua/{modules.cpp,script_reader.cpp,utils.cpp}`: preloads, the module loader, MoonScript loading, and error stack traces.
- `libaegisub/lua/modules/{re,unicode,lfs}.cpp`: the native halves of `aegisub.re`, `aegisub.unicode` and `aegisub.lfs`.
- `libaegisub/include/libaegisub/lua/ffi.h` and `type_name.h`: how native functions are handed to LuaJIT FFI.
- `libaegisub/common/path.cpp`, `libaegisub/windows/path_win.cpp`, `libaegisub/unix/path.cpp`, `src/main.cpp`: path tokens.
- `libaegisub/common/vfr.cpp`: the exact `frame_from_ms` / `ms_from_frame` math.
- `automation/include/**`, `automation/autoload/**`: shipped Lua/Moon files.
- `automation/v4-docs/*.txt`: the original API docs. **Partly stale.** This spec follows the C++ where the two disagree, and flags each disagreement.
- `automation/tests/modules/*.moon`: busted specs for `re`, `unicode`, `lfs` and `argcheck`. They are ISC-licensed and reusable as a conformance suite.
- `src/libresrc/default_config.json`: default paths and trace level.

Notation in this document:
- **[C]** means implemented in C++ (we implement it in Rust).
- **[L]** means implemented in Lua/Moon (we can vendor the file).
- **[arch]** means only in the arch1t3cht fork.
- **[TT]** means only in TypesettingTools master.

---

## 1. Script runtime environment (what every script state looks like)

Aegisub creates **one independent `lua_State` per script file**: `luaL_newstate()`, in `LuaScript::Create`. Autoload scripts are loaded **in parallel** (`std::async` per file in `AutoloadScriptManager::Reload`), so the host must allow several LuaJIT states to initialise concurrently on different threads.

### 1.1 Initialisation order (`LuaScript::Create`)

1. `preload_modules(L)` (`libaegisub/lua/modules.cpp`):
   - `luaL_openlibs(L)` loads **all** LuaJIT libs: base, package, table, io, os, string, math, debug, bit, jit, ffi.
   - It registers these `package.preload` entries:
     - `aegisub.__re_impl` → native regex table (§5.4)
     - `aegisub.__unicode_impl` → native case-mapping table
     - `aegisub.__lfs_impl` → native filesystem table
     - `lpeg` → LPeg **0.10** (C, bundled `libaegisub/lua/modules/lpeg.c`)
     - `luabins` → luabins (C). TT wraps it to raise a deprecation warning via `aegisub.__raise_warning`.
2. `dofile = nil`, `loadfile = nil`. Both are removed. `loadstring`, `load` and `require` remain.
3. Global `include(filename)` [C] is defined (§1.3).
4. `Install(L, include_path[, support_path])` (`script_reader.cpp`):
   - It sets `package.path`. For each directory `D` in `include_path`, in order, it appends `D/?.lua;D/?/init.lua;`.
     - `include_path` is built in `Script::Script` (`auto4_base.cpp`): first the **script's own directory**, then every entry of option `Path/Automation/Include` that decodes to an absolute, existing directory. The default value is `?user/automation/include/|?data/automation/include/`.
     - On non-Windows the original LuaJIT default `package.path` is appended. **On Windows the default is dropped.**
     - The strings use native separators, e.g. `C:\Users\x\AppData\Roaming\Aegisub\automation\include\/?.lua`. That form is harmless.
   - It replaces `package.loaders[2]` (the Lua-file searcher) with Aegisub's `module_loader`, which:
     - takes the module name and replaces `.` with the directory separator;
     - for each `;`-separated template in the **current** `package.path` (scripts may modify it), substitutes `?`;
     - if the template ends in `.lua` and a sibling file with extension `.moon` exists, **loads the `.moon` instead**;
     - loads the first existing file with `LoadFile` (UTF-8 paths, BOM stripped, MoonScript compiled);
     - on a load error, raises `Error loading Lua module "<path>":\n<msg>`;
     - if nothing is found, returns nothing. The standard "module not found" message comes from `require`.
   - `package.cpath` and loaders 1, 3 and 4 are left untouched, so C modules are loadable via cpath.
   - Windows only: it runs `unicode-monkeypatch.lua` (§5.2), which patches `io.open`, `os.rename`, `os.remove` and `os.execute` to accept UTF-8 paths. It does nothing when the process ANSI code page is already UTF-8.
   - It runs `moonscript.lua`. That file registers all `package.preload['moonscript.*']` modules and returns the `moonscript` module. Aegisub then:
     - stores `moonscript.loadstring` in the registry under `"moonscript"`;
     - sets `package.loaded.moonscript` to the module.
     - **[TT]** both support files are loaded by exact path from `?data/automation/include/`. This is a 2026 security fix, so that a `moonscript.lua` next to an untrusted script can't hijack loading. arch uses `require('moonscript')`, i.e. the first match on `package.path`.
5. It stores in the registry `filename` = file **stem** (used for macro command names) and `aegisub` = the script object pointer.
6. It creates the global `aegisub` table (§2).
7. `LoadFile(L, path)`, then `lua_pcall` with `add_stack_trace` as the message handler. The top level runs and registers features.
8. If the global `version == 3`, it rejects the script as Automation 3.
9. It reads the globals `script_name`, `script_description`, `script_author` and `script_version`. Each is read only if `lua_isstring`, so numbers are accepted. An empty name falls back to the pretty file name.
10. **[TT]** It drains the registry list `warnings` (filled by `aegisub.__raise_warning` during load) and then swaps `aegisub.__raise_warning` for a version that immediately logs `Warning in Automation script '<stem>':\n<msg>`.

### 1.2 Chunk names, errors and stack traces

- `LoadFile` uses `luaL_loadbuffer(L, buf, size, path_string)`. The chunk name is the **plain path string without a leading `@`**. As a result:
  - `debug.getinfo(f,"S").source` is the raw path, e.g. `C:\...\script.lua`.
  - Error messages look like `[string "C:\...\script.lua"]:12: msg`.
  - Some scripts parse `source` to find their own directory, so we must reproduce this exactly.
- MoonScript files are compiled with `moonscript.loadstring(text, path)`, which uses the same chunk name. The raw `.moon` text is stored in the registry under `"raw moonscript: <path>"`, so that `moonscript.line_tables` can map Lua line numbers back to `.moon` lines in stack traces.
- `add_stack_trace` (`utils.cpp`) is the message handler for script load and macro or filter runs:
  - If the error value is not a string, it is returned unchanged. This is how `aegisub.cancel()` (nil error) passes through silently.
  - Otherwise it strips the leading `[string "..."]:N: ` once.
  - It then appends frames, newest last, each formatted as `    File "<source>", line <n>\n<function>`. The function is `<main>`, `?` for C, a named function, or `<anonymous function at lines a-b>`. `.moon` sources get mapped line numbers.
- Runtime errors from macros are written to the progress-dialog log as `\n\nLua reported a runtime error:\n<trace>`, and the subtitle changes are rolled back.

### 1.3 `include(filename)` [C]

- If `filename` contains `/` or `\`, it resolves **relative to the script's directory**. It is never relative to the include paths.
- Otherwise it searches `include_path` in order (script dir, `?user/automation/include`, `?data/automation/include`).
- On failure it raises `Lua include not found: <name>`. On a load error it raises `Error loading Lua include "<name>":\n<msg>`.
- The file is executed every time `include` is called. There is no caching, unlike `require`.
- It returns whatever the chunk returns (`LUA_MULTRET`).
- `.moon` files can be `include`d and are compiled.

### 1.4 Script globals read by the host

| Global | Used by | Notes |
|---|---|---|
| `script_name` | Automation manager name, filter and macro default names in karaskel | string or number |
| `script_description` | manager | |
| `script_author` | manager | |
| `script_version` | manager | string or number |
| `version` | rejection when `== 3` | |
| `script_namespace` | **not read by Aegisub**. It is a DependencyControl convention (`l0.ScriptName`). DepCtrl's `PackageRecord` reads it and exports it if it is missing. | |

### 1.5 Threading model

- Macro and filter processing functions run on a **background thread** (`BackgroundScriptRunner` with a modal progress dialog). The Lua state is used by one thread at a time.
- GUI interactions marshal synchronously to the main thread with `agi::dispatch::Main().Sync`: dialogs, file pickers, clipboard, and **[TT]** `text_extents`.
- Validation (`validate`) and `is_active` functions run on the **GUI thread**, e.g. on every menu open. They have no progress sink.
- The config-dialog provider of export filters runs on the GUI thread.
- mlua needs the `send` feature, or one dedicated worker thread per script, to reproduce this.

---

## 2. The global `aegisub` table

The table is created in `LuaScript::Create`. Some fields are installed or replaced dynamically:

- **(P)** = only present while a `LuaProgressSink` exists, i.e. during a macro or filter *processing* run.
- **(S)** = (re)installed on the global table each time a subtitles object is created.

### 2.1 Field list

| Field | Kind | Signature → returns | Semantics / edge cases |
|---|---|---|---|
| `register_macro` | C | `(name, description, process, validate?, is_active?)` → nothing | §3.1 |
| `register_filter` | C | `(name, description, priority, process, config_provider?)` → nothing | §3.3 |
| `text_extents` | C | `(style_table, text)` → `width, height, descent, ext_lead` | §10 |
| `frame_from_ms` | C | `(ms)` → `frame` or `nil` | `nil` unless timecodes are loaded (video, or a timecodes file). It reads the **last stack arg** (`lua_tointeger(L,-1)`). Uses `FrameAtTime(ms, START)`, see §2.3. |
| `ms_from_frame` | C | `(frame)` → `ms` or `nil` | `nil` without timecodes. Uses `TimeAtFrame(frame, START)`: **not** the exact frame start, but the midpoint rule in §2.3. |
| `video_size` | C | `()` → `w, h, ar_value, ar_type` or a single `nil` | `nil` if no video provider. `ar_value` is a double. `ar_type` is an int: 0 Default, 1 Fullscreen (4:3), 2 Widescreen (16:9), 3 Cinematic (2.35), 4 Custom. |
| `keyframes` | C | `()` → array table of **0-based frame numbers** (1-based Lua array) | Empty table if no keyframes. `nil` only if there is no project context, e.g. at script load. |
| `decode_path` | C | `(path)` → string | §8. It uses the project's path object if a context exists, else the global one, which lacks `?script`, `?video` and `?audio`. |
| `cancel` | C | `()` → does not return | Pushes `nil` and raises a Lua error **with a nil error value**. The runner treats a nil error as a silent cancel: nothing is logged, all subtitle modifications are discarded (`LuaAssFile::Cancel`), and nothing is committed. A pcall inside the script can catch it (`pcall` returns `false, nil`). |
| `lua_automation_version` | number | `4` | |
| `file_name` | C | `()` → file name **only** (no directory) of the current subtitle file, or `nil` if unsaved or no context | |
| `gettext` | C | `(str)` → translated string | `wxGetTranslation` in Aegisub's catalog. An identity function is acceptable. Scripts use `local tr = aegisub.gettext`. |
| `project_properties` | C | `()` → table or `nil` (no context) | See below. |
| `get_audio_selection` | C | `()` → `start_ms, end_ms` or `nil` | The active line's range in the audio timing controller. `nil` when no timing controller exists (no audio). |
| `set_status_text` | C | `(text)` → nothing | Asynchronous status-bar message (timeout). Returns `nil` if no frame. **The v4-docs call it `set_status_bar_text`, but the registered name is `set_status_text`.** |
| `__init_clipboard` | C | `()` → FFI lib table `{get, set}` | Internal; used by `aegisub/clipboard.lua` (§5.4). |
| `__raise_warning` | C [TT] | `(msg)` | During load it queues a warning; after load it logs immediately. Internal. |
| `get_frame` | C [arch] | `(frame_number, with_subtitles?)` → `VideoFrame` userdata or `nil` | §2.4 |
| `gui` | table [arch] | `get_cursor()`, `set_cursor(pos)`, `get_selection()`, `set_selection(start, end)`, `is_modified()` | §2.4 |
| `progress` | table (P) | `set(percent)`, `task(msg)`, `title(title)`, `is_cancelled()` → bool | `set` takes 0–100. `task` and `title` require strings (`check_string`). |
| `debug` | table (P) | `out([level,] fmt, ...)` | Level filter and format, see §2.2. |
| `log` | (P) | same function as `debug.out` | |
| `dialog` | table (P, **macros only**) | `display(controls, buttons?, button_ids?)`, `open(title, dir, file, wildcard, multiple?, must_exist?)`, `save(title, dir, file, wildcard, dont_prompt_overwrite?)` | §4. Not installed for export-filter runs (`can_open_config=false`). |
| `parse_karaoke_data` | (S) | `(dialogue_line_table)` → syllable array | §2.5 |
| `set_undo_point` | (S) | `(description)` | §2.6 |

`project_properties()` fields: `automation_scripts`, `export_filters`, `export_encoding`, `style_storage`, `video_zoom`, `ar_value`, `scroll_position`, `active_row`, `ar_mode`, `video_position` (a frame number), `audio_file`, `video_file`, `timecodes_file`, `keyframes_file`. The four file fields are made **absolute relative to `?script`**. `automation_settings` is **not** exposed.

**Not implemented despite being in v4-docs:** `register_reader`, `register_writer`, and the file streams API. Don't implement them, but leave the names free.

**Lifecycle quirks to replicate or defuse:**

- `progress` and `debug` are set to `nil` again after each run. `log` and `dialog` are **left dangling** in Aegisub, holding a stale pointer, which is a latent use-after-free.
  - FS Editor should install all four per run and remove all four afterwards. Alternatively, make them raise `"not available in this context"` outside a run.
  - At script **load time** (top-level code) `aegisub.debug` and `aegisub.log` **do not exist** in Aegisub, so scripts guard with `if aegisub.log then` (DepCtrl's Logger does exactly this).
  - Offering them at load time is a compatible superset, but some scripts may branch on their presence. Safest is to match Aegisub: absent at load.
- The context functions read a registry light-userdata `project_context`, which is set before validate, run and config calls and never cleared. At load time the context is null:
  - `video_size`, `frame_from_ms`, `ms_from_frame`, `keyframes`, `file_name`, `project_properties`, `get_audio_selection` and `set_status_text` all return `nil`;
  - `decode_path` uses the global path object;
  - **[arch]** `gui.*` crash because they dereference null. Return `nil` instead.

### 2.2 `aegisub.debug.out` / `aegisub.log` exact semantics

```
out(level:number, fmt:string, ...)   -- level given
out(fmt:string, ...)                 -- no level = always shown
```

1. If arg 1 is a **number** and `level > trace_level`, return silently. Otherwise drop the level argument.
   - `trace_level` = option `Automation/Trace Level`, **default 3**.
   - Levels: 0 fatal, 1 error, 2 warning, 3 hint, 4 debug, 5 trace. Level 0 does **not** abort.
2. If **more than one** argument remains, call `string.format(...)`. On a format error, raise a Lua error with position info (`luaL_where(1)` + msg).
3. Log `check_string(arg1)` to the progress dialog log window. A single non-string, non-number arg raises "string expected".

No newline is appended; scripts add `\n` themselves.

### 2.3 Frame/time math (must be bit-exact; `libaegisub/common/vfr.cpp`)

- `frame_from_ms(ms)` = `FrameAtTime(ms, START)` = `FrameAtTime_EXACT(ms - 1) + 1`.
- `FrameAtTime_EXACT(ms)`:
  - if `ms < 0`: `int((ms*num/den - 999)/1000)`;
  - if `ms > last timecode`: extrapolate with `((ms+1)*num - last - num/2 + (1000*den - 1)) / (1000*den) + tc.size() - 2`;
  - else: the index of the last timecode `<= ms`.
- `ms_from_frame(f)` = `TimeAtFrame(f, START)` = `prev + (cur - prev + 1) / 2`, where `prev = TimeAtFrame_EXACT(f-1)` and `cur = TimeAtFrame_EXACT(f)`. Integer division.
- `TimeAtFrame_EXACT(f)`:
  - `0` if `num == 0`;
  - `f*den*1000/num` for `f < 0`;
  - extrapolated past the end;
  - else `timecodes[f]`.
- Example at 24000/1001: `ms_from_frame(1) = 0 + (42 - 0 + 1)/2 = 21`, and `frame_from_ms(21) = 1`.

### 2.4 arch-only APIs (implement them: arch-targeted scripts use them)

- `aegisub.get_frame(n, with_subs=false)`:
  - Returns `nil` unless timecodes are loaded.
  - Otherwise it returns a userdata with metatable `"VideoFrame"`, whose `__index` is the metatable itself. Methods:
    - `frame:width()`, `frame:height()`;
    - `frame:getPixel(x, y)` → `r, g, b` (0–255), or **`nil` plus two missing values** when out of range;
    - `frame:getPixelFormatted(x, y)` → `"&HBBGGRR&"`, or `nil`;
    - `frame:data()` → `lightuserdata ptr` (BGRA bytes), `pitch`, `flipped`.
  - The frame buffer stays alive until the userdata is collected (`__gc`). If `flipped`, the getter uses `y = height - y` (sic, off-by-one in arch).
  - The args are read as `lua_tointeger(L,1)` and `lua_toboolean(L,2)`; `with_subs=true` renders subtitles onto the frame.
- `aegisub.gui`. All indices are **1-based byte positions** in the edit box. The setters are *staged* and applied after the macro's subtitle changes are committed; only the last staged value wins. The getters return the staged state.
  - `get_cursor()` → number;
  - `set_cursor(pos)`;
  - `get_selection()` → `start, end` with `start <= end`;
  - `set_selection(start, end)`, where `end` may be `< start` (cursor on the left);
  - `is_modified()` → bool (unsaved changes).
  - Staging is dropped at the start of each macro run (`DropStagedChanges`) and committed at the end (`CommitStagedChanges`).

### 2.5 `aegisub.parse_karaoke_data(line)` (S)

- The argument is a **full dialogue line table**, not a string, despite v4-docs. It is converted with `LuaToAssEntry`, so all dialogue fields are required. A non-dialogue line raises argerror `Subtitle line must be a dialogue line`.
- It returns a table with integer keys starting at **0**:
  - `[0] = {duration=0, start_time=0, end_time=0, tag="", text="", text_stripped=""}`. This is always present.
  - `[1..n]`: one per syllable, from `ParseKaraokeSyllables` (`src/ass_karaoke.cpp`). Rules:
    - Every override tag starting with `\k` (case-insensitive; includes `\k`, `\K`, `\kf`, `\ko`, `\kt`) starts a new syllable. Its duration is `param*10` ms.
    - A syllable is emitted only if its duration is > 0 or its text is non-empty. The first, implicit syllable has tag `\k`.
    - `\K` is normalised to `\kf`.
    - `tag` **includes the backslash**: `"\\k"`, `"\\kf"`, `"\\ko"`, … (the v4-docs claim `"k"`; the code says otherwise).
    - `start_time` and `end_time` are relative to the line start. `end_time = start_time + duration`.
    - `text` is the syllable text with the non-`\k` override blocks, comments and drawings re-inserted at their positions. Adjacent tags are merged into one `{...}`, and the `\k` tag itself is removed.
    - `text_stripped` is plain text only (no tags, comments or drawings).
    - There is no normalisation to the line end and no auto-split (`SetKaraokeLine(kara, dia, false, false)`).

### 2.6 `aegisub.set_undo_point(description)` (S)

- It errors with `Attempt to set an undo point in a context where it makes no sense to do so.` unless the subtitles object was created with `can_set_undo`. That is true **only for macro processing**: not filters, validate, is_active, or config.
- If anything was modified since the last undo point, it snapshots the current line vector with the description into `pending_commits` and resets the modification mask. Otherwise it is a **no-op**.
- When the run ends successfully, each pending commit is applied in order and committed (multiple undo steps). Remaining modifications are then committed with the **macro's display name** as the undo description. A macro that never calls `set_undo_point` still gets one undo step.
- Calling it after the subtitles object has expired raises `Subtitles object is no longer valid`.

---

## 3. Feature registration

### 3.1 `aegisub.register_macro(name, description, process, validate?, is_active?)`

- `name`: `check_string`. A `/` in the name creates **submenus** in the Automation menu (`src/menu.cpp` splits on `/`). DepCtrl relies on this.
- `description`: may be nil (`lua_tostring`). It is the help or status text.
- `process` must be a function. Otherwise: `The macro processing function must be a function`.
- `validate` and `is_active` are optional functions. Non-functions are treated as absent.
- Command id: `automation/lua/<script file stem>/<name>`. This is the key used for hotkeys and toolbar entries.
- A duplicate macro name **within the same script** raises `A macro named '<name>' is already defined in script '<script_name>'`.
- Registration is only meaningful during top-level execution, because features are collected while the script loads.

**Line indices passed to macro callbacks are absolute subtitles-object indices.** That is: row in the Events list + `#Info + #Styles` + 1.

`process(subs, selected_lines, active_line)`:
- `subs`: a read-**write** subtitles object (`can_modify=true`, `can_set_undo=true`).
- `selected_lines`: an array of absolute indices of the selected dialogue lines, **sorted ascending**.
- `active_line`: the absolute index of the active line, or **`0`** if there is none. It is never nil here.
- Return values. At most 2 are read:
  1. **New selection** (optional table of absolute indices). Each entry:
     - Non-numbers are skipped.
     - An index out of `1..#lines` (counted after processing) logs `Selected row %d is out of bounds (must be 1-%u)` and **stops processing the remaining entries**.
     - An index pointing at a non-dialogue line logs `Selected row %d is not a dialogue line` and also stops.
  2. **New active line** (optional number):
     - Out of range logs an error and is ignored, keeping the original active row number.
     - Active-line choice: if the returned active index is one of the selected rows, use it. Otherwise use the first selected row encountered in iteration order, unless the current active line is still in the selection.
     - Exactly: `new_active = current; if active_line && (active_idx > 0 || !sel.contains(current)) new_active = active_line`.
     - If the selection is empty, it becomes `{new_active}`.
  - If **no table is returned**: the old selection is re-mapped **by row position**. The k-th selected event row maps to whichever event now sits at that absolute index; the walk stops at the end. If empty, select the first event. If the active line falls outside the selection, the active line becomes the first selected line.
- Errors: logged, all modifications discarded, no commit.
- `aegisub.cancel()`: silent; modifications discarded.
- The user pressing Cancel in the progress dialog does **not** stop the Lua code. The script must poll `aegisub.progress.is_cancelled()`.

`validate(subs, selected_lines, active_line)`:
- `subs` is **read-only**. Modifying calls raise `Attempt to modify subtitles in read-only feature context.`
- `active_line` is `nil` if there is none.
- Return `ok[, new_description]`. Truthiness of value 1 decides enablement. If value 2 is a non-empty string, it **replaces the macro's help text** (dynamic help).
- Errors are logged as warnings and the macro is reported invalid.
- No `progress`, `debug` or `dialog` tables exist here (unless stale, see §2.1).

`is_active(subs, selected_lines, active_line)` → bool:
- It decides the checked state of toggle menu items.
- `subs` is read-only. Errors are logged and treated as false.
- Bug in Aegisub: if there is no active line, only 2 args are pushed while `lua_pcall(L,3,...)` is used. Pass `nil` as the third argument instead.

### 3.2 Script metadata

See §1.4. `script_name` is also used by karaskel (`karaskel.use_fx_library`, `use_classic_adv`) as the default macro and filter name.

### 3.3 `aegisub.register_filter(name, description, priority, process, config_provider?)`

- `name`: `check_string`. `description`: `lua_tostring` (nil allowed). `priority`: `lua_tointeger`. Higher runs earlier by default. Built-ins are listed in the v4-docs.
- `process` must be a function. Otherwise: `The filter processing function must be a function`.
- `process(subs, settings)`:
  - `subs` is writable but has **no undo**, so `set_undo_point` errors.
  - It runs on the export copy of the file.
  - `settings` is the result table of the config dialog (name → value), or `{}` when there is no config provider.
  - `aegisub.dialog` is **not** available.
- `config_provider(subs, stored_settings)` must return a **dialog control array** (same format as §4). `subs` is read-only, and `stored_settings` is currently **always an empty table** (TODO in Aegisub). The returned table is converted to a dialog **without buttons** and embedded in the Export dialog.
- Persistence: after export, control values are serialised and stored in project properties.
  - Format: `name:value|name:value`. Names and string values use `inline_string_encode`.
  - Value forms: int and float use `std::to_string`, checkbox uses `1`/`0`, color uses the hex string.
  - Storage key: `Automation Settings <inline_string_encode(filter name)>` in `[Aegisub Project Garbage]`.
  - The values are restored by matching control `name` the next time the config window is created.

---

## 4. Dialogs (`aegisub.dialog.*`, `src/auto4_lua_dialog.cpp`)

### 4.1 `display(controls[, buttons[, button_ids]])`

- `controls` must be a table at stack index 1. Otherwise: `Cannot create config dialog from something non-table`.
- Each element must be a table with a known `class` (lowercased before comparison). Otherwise: `bad control table entry`. Iteration order follows `lua_for_each` (`lua_next`), which defines tab order.
- `buttons`: an optional array of label strings. Non-strings raise an error.
- `button_ids`: an optional table `{id_name = label}`. `id_name` is one of `ok`, `yes`, `save`, `apply`, `close`, `no`, `cancel`, `help`, `context_help` (unknown names map to -1). The label must equal one of `buttons`, otherwise `Invalid button for id %s`.
- Button behaviour:
  - With no buttons, the default is OK and Cancel, with **empty-string labels** (the platform stock labels are shown).
  - Ids `ok`, `yes` and `save` become the default (Enter) button. `close` and `no` become the Escape button.
  - If no button has id -1, the buttons go into a `wxStdDialogButtonSizer` in platform order. Otherwise they are laid out in the given order.
- Return values: `button, results`.
  - `button` is `false` if the dialog was closed without a button (button index -1) **or** the pressed button has id `cancel`. Otherwise it is the pressed button's **label string**.
  - With the default buttons, OK returns `""`. That is an empty string, which is truthy. **The v4-docs claim `true`, but the code returns `""`.** Scripts test truthiness, so either works, but match `""`.
  - `results` is a table keyed by control `name`; label controls contribute `nil`. It is filled even on cancel, with the values as they were.
- Runs on the GUI thread via `ShowDialog`. The dialog title is the macro name (BackgroundScriptRunner title), and the parent is the progress dialog.

### 4.2 Layout

- `wxGridBagSizer(vgap=4, hgap=4)`. Each control sits at **row = `y`, column = `x`**, spanning `height` rows by `width` columns.
- Defaults: `x=0`, `y=0`, `width=1`, `height=1`. Numbers are read with `lua_tointeger`.
- Sizer flags: `wxEXPAND` for all controls except labels (`ALIGN_CENTRE_VERTICAL | ALIGN_LEFT`).
- **[TT 2026-10, "Compensate for wxGridBagSizer behavior change"]** Each control's min size is raised to at least `EmptyCellSize * span`. Older wx gave every spanned row and column a min size, and scripts use `height` to make tall textboxes. Reproduce this: a `textbox` with `height=10` must be visibly about 10 rows tall.
- The grid sizer sits above the button sizer (5 px gap).
- `textbox` min height is 30 px. `color` button size is `50*width × 10*height`.

### 4.3 Control classes and properties

Common properties:
- `class` (required);
- `name` (result key; recommended);
- `x`, `y`, `width`, `height`;
- `hint` (tooltip). Labels ignore it.

Values are only read if they have the right Lua type (`get_if_right_type`); otherwise the default is kept silently.

| class | properties | default value | result type |
|---|---|---|---|
| `label` | `label` (string) | — | `nil` (no entry) |
| `edit` | `value` (string), **also `text`**, which overrides `value` if present | `""` | string |
| `textbox` | same as `edit`; multiline | `""` | string (may contain `\n`) |
| `intedit` | `value` (int), `min`, `max`. If `min >= max`, the range becomes unbounded. Spin control. | `0` | integer |
| `floatedit` | `value`, `min`, `max`, `step`. If `min >= max`, unbounded. If `step ≠ 0` (spin control), an unbounded min becomes 0 and max becomes 100. If `step == 0` it is a text box with a range validator. | `0.0` | number |
| `dropdown` | `items` (array of strings; non-strings skipped), `value` (string) | `""` | string (the selected item; read-only combobox) |
| `checkbox` | `label`, `value` (bool) | `false` | boolean |
| `color` | `value`, parsed as an `agi::Color` (formats below) | black | `"#RRGGBB"` |
| `coloralpha` | `value` | black | `"#RRGGBBAA"` |
| `alpha` | **Implemented as a plain `edit`** (FIXME in source) | `""` | string |

Color parsing (`libaegisub/common/parser.cpp`, used for dialog colors and **style colorN fields**):
- Accepted forms:
  - an SSA decimal int;
  - `[&][H|h]` + 8 or 6 hex digits + `[&]` (ABGR or BGR);
  - `#RRGGBB`, `#RGB`, `rgb(r,g,b)`, `rgba(r,g,b,a)`.
- The whole string must be consumed. On failure the value stays `{0,0,0,0}` and **no error** is raised.
- Note: `#RRGGBBAA` does **not** parse (Aegisub bug), so a `coloralpha` initialised from its own output resets to black. Replicate the bug, or accept it as a harmless superset.

Value serialisation (for filters) uses `inline_string_encode` (`libaegisub/ass/string_codec`). It escapes `#`, `,`, `:`, `|` and control chars as `#XX`.

### 4.4 `open` and `save`

- `open(title, dir, file, wildcard, multiple=false, must_exist=true)`:
  - The first four args are **required strings** (`check_string`).
  - Arg 2 is used as the **default directory** and arg 3 as the **default file name**. This follows the `wxFileDialog(parent, message, defaultDir, defaultFile, …)` call. **The online docs list `(title, default_file, default_dir, …)`; the code wins.**
  - `wildcard` uses wx syntax: `"Desc (*.ext)|*.ext|All|*.*"`.
  - `must_exist` defaults to true when nil.
  - Returns `nil` on cancel, a path string (UTF-8), or, if `multiple`, an array of paths.
- `save(title, dir, file, wildcard, dont_prompt_overwrite=false)` → path string or `nil`.

---

## 5. Standard include files, native modules, FFI bridge, licences

### 5.1 Inventory (`automation/include/`, installed to `?data/automation/include/`)

| File | Kind | What it does | Licence |
|---|---|---|---|
| `karaskel.lua` | L | `include "karaskel-auto4.lua"` | BSD-3 (Niels Martin Hansen, Rodrigo Braz Monteiro) |
| `karaskel-auto4.lua` | L | Defines the `karaskel` global: `collect_head`, `preproc_line_text`, `preproc_line_size`, `preproc_line_pos`, `do_basic_layout`, `do_furigana_layout`, `preproc_line`, `use_fx_library(_furi)`, `use_classic_adv`. It includes `utils.lua` and `unicode.lua`. | BSD-3 |
| `utils.lua` | L | `include "utils-auto4.lua"` | BSD-3 |
| `utils-auto4.lua` | L | `util = require 'aegisub.util'`. Sets the globals `table.copy`, `copy_line`, `table.copy_deep`, `ass_color`, `ass_alpha`, `ass_style_color`, `extract_color`, `alpha_from_style`, `color_from_style`, `HSV_to_RGB`, `HSL_to_RGB`, `clamp`, `interpolate`, `interpolate_color`, `interpolate_alpha`, `string.headtail`, `string.trim`, `string.words` | BSD-3 |
| `unicode.lua` | L | `_G.unicode = require 'aegisub.unicode'` | ISC (Thomas Goyne) |
| `re.lua` | L | `_G.re = require 'aegisub.re'` | ISC |
| `clipboard.lua` | L | `_G.clipboard = require 'aegisub.clipboard'` | ISC |
| `lfs.lua` | L | global `lfs = require 'aegisub.lfs'`; makes `require 'lfs'` work | BSD-3 (no header; covered by top-level `LICENCE`) |
| `cleantags.lua` | L | `cleantags(text)`, `cleantags_version="1.302"` | **MIT** (Muhammad Lukman Nasaruddin) |
| `moonscript.lua` | L | Amalgamated MoonScript compiler. **TT: v0.7.0; arch: v0.5.0.** It populates `package.preload['moonscript.*']`. | MoonScript is **MIT** (Leaf Corcoran). The file has no header, so regenerate it from upstream and ship upstream's LICENSE. |
| `unicode-monkeypatch.lua` | L | Windows UTF-8 patch of `io.open`, `os.rename`, `os.remove`, `os.execute` via `_wfreopen`, `_wrename`, `_wremove`, `_wsystem` and `MultiByteToWideChar`. A no-op if `GetACP()==65001` or the OS is not Windows. It errors if `io.open` is already patched. | BSD-3 (no header) |
| `aegisub/util.moon` | L | `copy`, `deep_copy`, `ass_color`, `ass_alpha`, `ass_style_color`, `extract_color`, `alpha_from_style`, `color_from_style`, `HSV_to_RGB`, `HSL_to_RGB`, `trim`, `headtail`, `words`, `clamp`, `interpolate`, `interpolate_color`, `interpolate_alpha` | ISC |
| `aegisub/re.moon` | L + native | Regex API (§5.5) | ISC |
| `aegisub/unicode.moon` | L + native | `charwidth(s,i)`, `chars(s)`, `len(s)`, `codepoint(s)`, `to_upper_case`, `to_lower_case`, `to_fold_case` | header is BSD-3 (Hansen/Monteiro) |
| `aegisub/lfs.moon` | L + native | `attributes`, `chdir`, `currentdir`, `dir`, `mkdir`, `rmdir`, `touch` (§5.6) | ISC |
| `aegisub/clipboard.lua` | L + native | `get()`, `set(str)` | ISC |
| `aegisub/internal/argcheck.moon` [TT] / `aegisub/argcheck.moon` [arch] | L | `check'string ?number ...'` type-check decorator | ISC |
| `aegisub/internal/ffi.moon` [TT] / `aegisub/ffi.moon` [arch] | L | `string(cdata)` (frees `char*`) and `err_arg_to_multiple_return` | ISC |

Notes on the inventory:

- **Ship both module paths**, `aegisub.argcheck`/`aegisub.ffi` and `aegisub.internal.argcheck`/`aegisub.internal.ffi`, because third-party scripts written against arch or older Aegisub `require 'aegisub.argcheck'`. Make one a two-line re-export of the other.
- Lua-side `aegisub.re`, `aegisub.unicode`, `aegisub.util`, `aegisub.lfs` and `aegisub.clipboard` are **modules obtained with `require`**. They are **not** fields of the global `aegisub` table: `require 'aegisub.re'` does not set `aegisub.re`.
- Autoload scripts shipped in `automation/autoload/`:
  - `kara-templater.lua` and `karaoke-auto-leadin.lua` (BSD-3);
  - `cleantags-autoload.lua` (MIT);
  - `macro-1-edgeblur.lua` and `macro-2-mkfullwitdh.lua` (no header → BSD-3);
  - `strip-tags.lua` and `select-overlaps.moon` (ISC).
- Native libs bundled in Aegisub:
  - LuaJIT 2.1 (`04dca79`, static, amalgam, **`lua52compat=true`**): MIT;
  - LPeg 0.10: MIT (Lua.org);
  - luabins: MIT.
- **Licensing conclusion:** every Lua/Moon file is BSD-3, ISC or MIT. All are compatible with BSD-3 redistribution. Keep each file's header and add a `THIRD_PARTY_NOTICES` entry. For files without a header (`lfs.lua`, `unicode-monkeypatch.lua`, the demo macros), include Aegisub's top-level `LICENCE` text (Copyright (c) 2004-2012, Aegisub Project). The C++ we re-implement in Rust is ISC (Goyne) or BSD-3, so porting is also fine with attribution.
- json: **Aegisub does not ship a json module.** DepCtrl bundles dkjson (MIT) and registers it as `json`/`dkjson` (§7).

### 5.2 Required LuaJIT build properties

- **`LUAJIT_ENABLE_LUA52COMPAT` is mandatory.** Use mlua feature `luajit52` + `vendored`.
  - The subtitles object implements `__ipairs`, and karaskel does `for i, l in ipairs(subs)`.
  - DependencyControl ≥0.7 asserts `__pairs` support at load.
  - Aegisub's meson build fails if the system LuaJIT isn't 5.2-compat.
- The full stdlib, including `ffi`, `debug`, `io`, `os` and `jit`, must be available:
  - in mlua use `unsafe { Lua::unsafe_new_with(StdLib::ALL, LuaOptions::default()) }`, because `Lua::new()` omits `ffi` and `debug`;
  - `io.popen` and `os.execute` are used by petzku-style encode scripts;
  - `ffi.load` of system and user DLLs is used by DepCtrl, Yutils and ffi-experiments.
- **Windows CRT:** Aegisub requires the app and LuaJIT's `ffi.C` to use the **same dynamically linked CRT** (`meson.build` comment). Native functions return `malloc`'d `char*` that Lua frees with `ffi.C.free`.
  - In Rust, allocate those strings with `libc::malloc` from the dynamic UCRT, which is the MSVC default. **Never build with `+crt-static`.**
  - `ffi.C` on Windows resolves the exe's exports, the CRT, kernel32, user32 and gdi32. `unicode-monkeypatch.lua` needs `GetACP`, `MultiByteToWideChar`, `_wfreopen`, `_wrename`, `_wremove`, `_wsystem` and `strerror`.

### 5.3 How Aegisub exposes native functions to FFI: **no exported symbols**

**No C symbols need to be exported from the binary.** `agi::lua::register_lib_table` (`ffi.h`/`modules.cpp`) does the following:

1. `require('ffi')`; for each opaque type name, `ffi.cdef("typedef struct T T;")`;
2. for each function, `ffi.cast("<C function-pointer type string>", lightuserdata(fnptr))`, stored in a fresh table under the given name;
3. returns that table. It becomes the value of `package.preload['aegisub.__xxx_impl']`, or the return of `aegisub.__init_clipboard()`.

**FS Editor approach.** Write the same three steps in a tiny Lua shim, e.g.

```lua
ffi.cdef"typedef struct T T;"
lib[name] = ffi.cast(sig, ptr)
```

Pass `ptr` from Rust as `LightUserData(fn as *mut c_void)`, then use `#[no_mangle]`-free `extern "C" fn`s.

- The exact type strings below are what Aegisub generates through `type_name<>`: `const` is postfix and `&` means reference, which LuaJIT FFI accepts.
- We may substitute equivalent pointer types, e.g. `u32regex*` for `u32regex&`.
- **Except for return types.** `aegisub.internal.ffi.string()` frees the result **only if** `ffi.typeof(result) == ffi.typeof('char *')`. Functions returning malloc'd strings must therefore be typed `char*`, and functions returning static strings `const char*`.
- `size_t`, `time_t` and `uintmax_t` are printed as `unsigned long long` / `long long` on Win64 and `unsigned long` / `long` on LP64.

**`aegisub.__re_impl`** (types `agi_re_match`, `u32regex`; `agi_re_flag` is cdef'd by `re.moon` *before* the require):

| Lua name | C signature (TT) | Semantics |
|---|---|---|
| `search` | `int* (*)(u32regex&, char const*, size_t len, size_t start, char** err)` | Search `str[start..len)`, with `match_prev_avail|match_not_bob` if `start>0`. Returns a malloc'd `int[2]` = {1-based first byte, 1-based last byte} in absolute string coordinates (`start + pos + 1`, `start + pos + len`), or NULL. **[arch]** has no `err` param. |
| `match` | `agi_re_match* (*)(u32regex&, char const*, size_t len, int start, char** err)` | Returns a match object (all groups) or NULL. |
| `get_match` | `int* (*)(agi_re_match&, size_t idx)` | Group `idx` (0 = whole match). Returns a pointer to `int[2]` **owned by the match** = {first, last}, 1-based and **relative to `start`**. NULL if `idx > size` or the group is unmatched. |
| `replace` | `char* (*)(u32regex&, char const* replacement, char const* str, size_t len, int max_count, char** err)` | Replace up to `max_count` matches using **boost Perl format-string syntax** (`$&`, `$0`–`$9`, `${n}`, `` $` ``, `$'`, `$$`, `\n` `\t` escapes, `\l \u \L \U \E` case escapes). Returns a malloc'd `char*`. **[arch]** has no `err`. |
| `compile` | `u32regex* (*)(char const* pattern, int flags, char** err)` | `boost::make_u32regex(pattern, perl|flags)`, i.e. ICU-backed Perl syntax on UTF-8 with Unicode classes. On error, `*err = strdup(msg)`. |
| `get_flags` | `agi_re_flag const* (*)()` | Static array of `{const char* name; int value;}` terminated by `name==NULL`. Names: `ICASE`, `NOSUB`, `COLLATE`, `NEWLINE_ALT`, `NO_MOD_M`, `NO_MOD_S`, `MOD_S`, `MOD_X`, `NO_EMPTY_SUBEXPRESSIONS`. The values are boost's constants but **opaque to Lua**: `re.moon` only ORs them and passes them back to `compile`, so we may choose our own bits. Each becomes `re.ICASE` etc. as **`agi_re_flag` cdata**; `ffi.istype` is used to separate flags from arguments. |
| `match_free` | `void (*)(agi_re_match*)` | used as `ffi.gc` finalizer |
| `regex_free` | `void (*)(u32regex*)` | used as `ffi.gc` finalizer |

**`aegisub.__unicode_impl`** (no types): `to_upper_case`, `to_lower_case` and `to_fold_case`, each `char* (*)(char const* str, char** err)`.
- They use ICU full case mapping in the root locale (`"ß"` upper becomes `"SS"`) and ICU `foldCase()` default folding.
- Return a malloc'd UTF-8 string. Invalid input sets `*err` to `Converting "<str>" to a unicode string failed.`
- In Rust, `str::to_uppercase`/`to_lowercase` match ICU root closely enough; for folding use ICU4X `icu_casemap::CaseMapper::fold_string`.

**`aegisub.__lfs_impl`** (type `DirectoryIterator`). Every function takes a trailing `char** err`, because `lfs.moon` wraps every entry with `err_arg_to_multiple_return`, which calls `f(arg, err)` or `f(err)`:

| name | signature | semantics |
|---|---|---|
| `chdir` | `bool (*)(char const*, char**)` | `std::filesystem::current_path(p)` |
| `currentdir` | `char* (*)(char**)` | malloc'd cwd |
| `mkdir` | `bool (*)(char const*, char**)` | **`create_directories`**: recursive, and **true (not an error) if the directory already exists**. Differs from real LuaFileSystem. |
| `rmdir` | `bool (*)(char const*, char**)` | `std::filesystem::remove`: removes an empty dir **or a file**; a non-empty dir is an error |
| `touch` | `bool (*)(char const*, char**)` | create the file or update its mtime |
| `get_mtime` | `time_t (*)(char const*, char**)` | seconds since epoch; error if missing |
| `get_mode` | `char const* (*)(char const*, char**)` | static string: `file`, `directory`, `link`, `block device`, `char device`, `fifo`, `socket`, `other`; **NULL (no error) if not found** |
| `get_size` | `uintmax_t (*)(char const*, char**)` | file size; **error `NotAFile` for directories** |
| `dir_new` | `DirectoryIterator* (*)(char const*, char**)` | error `cannot open <p>: <msg>` if it can't be opened [TT] |
| `dir_next` | `char* (*)(DirectoryIterator&, char**)` | next name (malloc'd) or NULL at end; **never yields `.` or `..`** |
| `dir_close` | `void (*)(DirectoryIterator&, char**)` | **[arch]** has no `err` param |
| `dir_free` | `void (*)(DirectoryIterator*, char**)` | **[arch]** has no `err` param |

**`aegisub.__init_clipboard()`** returns `{get = char* (*)() [TT; arch: char const*], set = bool (*)(char const*)}`.
- `get` returns a malloc'd UTF-8 string, or NULL when the clipboard is empty, in which case `clipboard.get()` returns nil.
- `set` returns success.
- Both run synchronously on the main thread.

### 5.4 Resulting Lua-visible module APIs (what scripts call)

- **`aegisub.re`** (`re.moon`):
  - Module functions: `re.compile(pattern, ...flags)` → RegEx. The rest take `(str, pattern, ...extra args, ...flags)`; flags must come **after** the non-flag args:
    - `re.find` → array of `{str, first, last}` or nil;
    - `re.gfind` → iterator returning `str, first, last`;
    - `re.match` → array of `{str, first, last}` (index 1 = whole match, then groups), or nil;
    - `re.gmatch` → iterator of match tables;
    - `re.split(str, pattern, skip_empty?, max_split?)` → array;
    - `re.gsplit` → iterator;
    - `re.sub(str, pattern, repl:string|function, max_count?)`.
  - RegEx methods take `:` syntax. Calling with `.` errors with `re method called with invalid self. You probably used . when : is needed.`
  - Errors:
    - an empty pattern raises `Regular expression must not be empty`;
    - a compile error is re-raised at the caller's level;
    - argument checks produce `Argument #n should be a <type>, is <type> (<value>)` and `Expected n arguments, got m`.
  - Positions are 1-based byte offsets. With a **function** replacement:
    - single-group regexes pass the whole match, multi-group regexes pass each capture group `(str, first, last)`;
    - a non-string return keeps the original text;
    - every iteration consumes at least one char.
- **`aegisub.unicode`**:
  - `charwidth(s, i=1)`: bytes of the UTF-8 char at `i`, decided by the leading byte: <128 → 1, <224 → 2, <240 → 3, else 4. `nil` → 1.
  - `chars(s)` → iterator of `char, index`.
  - `len(s)`.
  - `codepoint(s)`: naive decode of the first char.
  - Case functions as in §5.3.
- **`aegisub.lfs`**:
  - `attributes(path[, field])`:
    - with field `'mode'`, `'modification'` or `'size'` it returns that value, or `nil, err`;
    - with no field it returns `{mode, modification, size}`;
    - a missing path returns **`nil, nil`**;
    - **a directory with no field returns `nil, "<...> is not a file"`**, because the size lookup fails. That is an Aegisub quirk; DepCtrl only asks for `"mode"`.
    - No other lfs fields (`access`, `ino`, …) exist.
  - `chdir(p)`, `mkdir(p)`, `rmdir(p)` and `touch(p)` return `true` or `nil, err` [TT]. arch returns `nil` on success, a bug that DepCtrl tolerates.
  - `currentdir()` → string, err.
  - `dir(p)` → `iter.next, iter`, for use as `for name in lfs.dir(p)`. It raises on error.
  - **Not provided:** `lock`, `unlock`, `link`, `symlinkattributes`, `setmode`, `lock_dir`.
- **`aegisub.clipboard`**: `get()` → string or nil; `set(str)` → bool.
- **`aegisub.util`**: see the table in §5.1. The functions are pure Lua, so vendor them.

### 5.5 Regex engine choice for Rust

Boost.Regex Perl mode is used with the ICU (`u32regex`) adapter: lookahead and lookbehind, backreferences, possessive and atomic groups, `\Q…\E`, named groups, inline modifiers, and Unicode properties.
- The Rust `regex` crate is **insufficient** (no backreferences or lookaround).
- Use **PCRE2** (BSD-3) via the `pcre2` crate, with UTF + UCP. Port boost's Perl format-string replacer, or test it against `automation/tests/modules/re.moon`.
- Map the flags:
  - `ICASE` → caseless;
  - `MOD_S` → dotall;
  - `NO_MOD_S`: default;
  - `MOD_X` → extended;
  - `NO_MOD_M`: boost's default is multiline (`^`/`$` match at newlines) **unless** `no_mod_m`, so enable PCRE2 MULTILINE by default and clear it for `NO_MOD_M`;
  - `NOSUB`, `COLLATE`, `NEWLINE_ALT` and `NO_EMPTY_SUBEXPRESSIONS` can be no-ops or approximated.

---

## 6. MoonScript support

- File extensions accepted by the script factory: `*.lua` and `*.moon` (`LuaScriptFactory`). Both the main scripts and `require`d modules can be `.moon`.
- Compilation happens **inside each script's own Lua state** via the registry `moonscript` function, i.e. `moonscript.base.loadstring(text, chunkname)` from the bundled `moonscript.lua`. There is no precompile cache.
  - Errors from `loadstring` (nil + message) propagate as load errors.
- Bundled versions:
  - **TT 3.5: MoonScript 0.7.0** (`package.preload['moonscript.version']` → `"0.7.0"`);
  - **arch: 0.5.0**;
  - **LPeg 0.10** (C) in both.
  - The amalgamation's `parse` modules `require("lpeg")`, which is satisfied by the preload.
- The amalgamation populates `package.preload` for `moonscript.version`, `util`, `types`, `transform.*`, `parse.*`, `line_tables`, `errors`, `dump`, `data`, `compile.*`, `cmd.*` and `base`. These must be **require-able by scripts**:
  - DepCtrl does `require 'moonscript.version'` and asserts ≥ 0.3.0;
  - DepCtrl's MoonCats uses `moonscript.parse` and `moonscript.util`;
  - the line-mapping code uses `moonscript.line_tables`.
- The moonscript module does **not** install its own `package.loaders` entry, and `package.moonpath` is not set. `.moon` resolution is solely Aegisub's `module_loader` (`.lua` template → sibling `.moon`).
- Recommendation for FS Editor:
  - Build `moonscript.lua` ourselves from upstream MoonScript 0.5.x/0.6 tag sources plus LPeg. 0.7.0 is TT's choice, so prefer it for parity.
  - Either vendor LPeg ≥1.0 as C compiled via the `cc` crate (MIT), or Aegisub's 0.10. MoonScript works with both.
  - Keep luabins only if needed. It is deprecated in TT (a warning is raised); a stub that warns is reasonable.

---

## 7. DependencyControl (DepCtrl): host requirements

DepCtrl is **user-installed** into `?user/automation/{autoload,include}`; it is not part of Aegisub. Two generations matter.

### 7.1 Legacy DepCtrl v0.6.x (still common in user installs)

- It requires these modules from the user's include dir:
  - `json`;
  - `lfs`, i.e. Aegisub's `lfs.lua` → `aegisub.lfs`;
  - `aegisub.re`, `aegisub.util`;
  - `ffi`;
  - **ffi-experiments**: `requireffi.requireffi`, `BM.BadMutex`, `DM.DownloadManager` (libcurl-based), `PT.PreciseTimer`.
- The ffi-experiments modules are Lua wrappers plus **native shared libraries**, e.g. `?user/automation/include/BM/BadMutex/libBadMutex.dll`.
  - `requireffi` finds a library by substituting the module path into each `package.path` template and calling `ffi.load(<absolute path>)`.
  - The host must therefore support `ffi.load` with absolute UTF-8 paths and a Windows-style `package.path` built from the include dirs (§1.1).
- It uses `aegisub.decode_path` with `?user/config/l0.DependencyControl.json`, `?user/automation/autoload`, `?user/automation/include`, `?user/log`, `?user/feedDump/` and `?temp`.
- It also uses `aegisub.register_macro`, `aegisub.log`, `aegisub.dialog.display`, `aegisub.cancel` and `aegisub.progress`.

### 7.2 Current DepCtrl v0.7–0.9.1 (`modules/l0/DependencyControl*.moon`)

- Asserts:
  - `require 'moonscript.version'` ≥ 0.3.0;
  - **`__pairs` honoured** (LUA52COMPAT).
- Bundles and self-registers (via a searcher appended to `package.loaders`):
  - `json`/`dkjson` → `l0.dkjson` (MIT);
  - shims `BM.BadMutex`, `DM.DownloadManager`, `PT.PreciseTimer` implemented in pure FFI. ffi-experiments DLLs are no longer needed.
- Pure-FFI system dependencies (all via `ffi.cdef`, `ffi.C` and `ffi.load`):
  - Windows:
    - `ffi.load`: `kernel32`, `ntdll` (`RtlAreLongPathsEnabled`), `shell32`, `ws2_32`, `winInet` (downloads), `advapi32` (SHA-1);
    - `ffi.C`: `QueryPerformanceCounter`, `Sleep`, `CreateSemaphoreA`, `WaitForSingleObject`, `ReleaseSemaphore`, `GetCurrentProcessId`, `fopen`, `fclose`, `LockFileEx`-family calls.
  - Linux: `ffi.load("libcurl.so.4"|"libcurl.so"|"curl")`, `libcrypto.so.3`/`.1.1`, plus `ffi.C` `flock`, `open`, `close`, `sem_open`, `sem_*`, `clock_gettime`, `poll`, `getpid`, `usleep`, `getaddrinfo`.
  - macOS: `libcurl.4.dylib`, and CommonCrypto from libSystem.
  - **Host implication:** `ffi.C` must resolve libc/CRT and kernel32. Never sandbox `ffi.load`.
- `lfs`: `attributes(p,"mode")`, `mkdir`, `rmdir`, `dir`, plus the others above. DepCtrl explicitly tolerates Aegisub's `rmdir` returning nothing.
- `aegisub.*` used:
  - `decode_path` (heavily: `?user`, `?temp`, `?data`, custom roots);
  - `dialog.display`;
  - `log` (guarded by `if aegisub.log`);
  - `cancel`;
  - `register_macro` (names joined with `/` for submenus);
  - `text_extents`.
- **Headless detection:** DepCtrl treats `aegisub.__depCtrl ~= nil` as "running outside Aegisub" (`environment.isHeadless`). **The host must not define `aegisub.__depCtrl`.**
- Install locations (`domain.getAutomationDir`):
  - macros go to `?user/automation/autoload`;
  - modules go to `?user/automation/include`;
  - tests go to `?user/automation/tests/DepUnit/{macros,modules}`;
  - config `?user/config/l0.DependencyControl.json`, logs `?user/log`, cache `?user/cache`.
- Uses `debug` library, `os.getenv`, `os.time`, `os.date`, `os.tmpname`, `io.open`, `io.popen` (git), and `math.random`.

### 7.3 How DepCtrl-managed scripts load

1. The user puts `l0.DependencyControl.Toolbox.moon` and the macros in `?user/automation/autoload/`, and the modules (`l0/DependencyControl.moon`, `l0/DependencyControl/*.moon`, `a-mo/*.moon`, `ASSFoundation/*`, …) in `?user/automation/include/`.
2. A macro does `DependencyControl = require "l0.DependencyControl"`. Aegisub's searcher finds `<include>/l0/DependencyControl.lua` → prefers `.moon` → compiles it.
3. `depctrl = DependencyControl{feed=..., {"a-mo.LineCollection", version="1.3.0", url=..., feed=...}, ...}`. Then `depctrl:requireModules()` (`require`s with version checks, may download), then `depctrl:registerMacro(fn)` → `aegisub.register_macro("<customMenu>/<name>", ...)`.
4. DepCtrl may unload and re-require modules after updates by setting `package.loaded[name] = nil`.

Requirement summary for FS Editor:
- default include path `?user/automation/include`;
- `.moon` module resolution identical to §1.1;
- `require` of `moonscript.*`;
- LUA52COMPAT;
- unrestricted `ffi`;
- Aegisub-compatible `lfs` semantics;
- `decode_path` tokens.

**Compatibility recommendation:** point `?user` at FS Editor's own data dir, and add **Aegisub's real user automation dirs** as extra autoload and include paths (opt-in). Already-installed DepCtrl setups then work, but DepCtrl will write its config under *our* `?user/config`.

---

## 8. Path specifiers (`libaegisub/common/path.cpp`)

Tokens (exact strings): `?audio`, `?data`, `?dictionary`, `?local`, `?script`, `?temp`, `?user`, `?video`.

`decode_path(p)` algorithm:
1. Token detection:
   - `p.size() >= 5 && p[0]=='?'`, keyed on `p[1]+p[4]`, then `starts_with(token)`.
   - **Prefix match only.** `?userfoo` decodes as `?user` + `foo`, and a token in the middle is not expanded.
2. If there is no token, **or the token's path is unset** (e.g. `?script` with an unsaved file, `?video` with no video): return `p` unchanged except separators are converted to native (`make_preferred`; `/`→`\` on Windows).
3. Strip the token, then strip **one** leading `/` or native separator. If nothing remains, return the token dir itself (no trailing separator). Otherwise return `tokendir / rest` with native separators.

Token values:

| Token | Windows | Linux | macOS |
|---|---|---|---|
| `?user` | `%APPDATA%\Aegisub` | `~/.aegisub` | `~/Library/Application Support/Aegisub` |
| `?local` | `%LOCALAPPDATA%\Aegisub` | `~/.aegisub` | same as `?user` |
| `?data` | directory of the exe | `P_DATA` (e.g. `/usr/share/aegisub`); exe dir for AppImage | bundle `SharedSupport` |
| `?dictionary` | `?data/dictionaries` | `/usr/share/hunspell` (`?data/dictionaries` for AppImage) | `?data/dictionaries` |
| `?temp` | `std::filesystem::temp_directory_path()` | same | same |
| `?script` | dir of the current subtitle file, unset if unsaved | | |
| `?video` | dir of the loaded video | | |
| `?audio` | dir of the loaded audio | | |

- `SetToken` with a *file* stores its parent dir; a relative value clears the token.
- **Portable mode:** if `?data/config.json` exists, `?user` and `?local` are set to `?data` (`src/main.cpp`).
- Defaults (`default_config.json`):
  - `Path/Automation/Autoload` = `?user/automation/autoload/|?data/automation/autoload/`;
  - `Path/Automation/Include` = `?user/automation/include/|?data/automation/include/`;
  - `Path/Automation/Base` = `?data/automation/` (base for `$` local-script references).

**Local (per-file) scripts:**
- Stored in project property `Automation Scripts` as a `|`-list with a prefix char per entry:
  - `~` = relative to the subtitle file's dir;
  - `$` = relative to `Path/Automation/Base`;
  - `/` = absolute.
- TT asks the user for trust before loading them, which is a security dialog.

Autoload:
- every file directly in each autoload dir (non-recursive; duplicates removed);
- `.lua` and `.moon` only;
- failures are reported as one summary warning.

---

## 9. The subtitles object (`src/auto4_lua_assfile.cpp`)

### 9.1 Model

- Modern Aegisub stores only **three groups**: Script Info (`AssInfo` key/value), Styles (`AssStyle`) and Events (`AssDialogue`).
- The Lua view is a flat vector `lines` = `[Info..., Styles..., Events...]`, indexed **1-based across all of them**.
- **There are no `format`, `head`, `clear`, `comment` or `unknown` lines anymore** (they are listed in the stale v4-docs). Scripts filtering on them simply never match.
- What is **not** exposed:
  - `[Fonts]`, `[Graphics]` (attachments);
  - `[Aegisub Project Garbage]` (exposed via `project_properties`);
  - `[Aegisub Extradata]` (exposed per line via `extra`);
  - `;` comment lines in Script Info, which are dropped on parse;
  - the `Collisions:` key, which is dropped;
  - any `[Script Info]` key consumed as a project property: `Automation Scripts`, `Export Filters`, `Export Encoding`, `Last Style Storage`, `Audio File`/`Audio URI`, `Video File`, `Timecodes File`, `Keyframes File`, `Video Zoom Percent`/`Aegisub Video Zoom Percent`, `Scroll Position`/`Aegisub Scroll Position`, `Active Line`/`Aegisub Active Line`, `Video Position`/`Aegisub Video Position`, `Video AR Mode`, `Video AR Value`, and `Automation Settings *`.
- Each `subs[i]` read **creates a new table** (a snapshot). Mutating it has no effect until it is assigned back.

### 9.2 Line tables (read: `AssEntryToLua`)

Common fields: `class`, `raw`, `section`. `section` is exactly `"[Script Info]"`, `"[V4+ Styles]"` or `"[Events]"`.

**info** (`class="info"`):

| field | value |
|---|---|
| `key` | string |
| `value` | string; leading whitespace after `:` trimmed on parse |
| `raw` | `key .. ": " .. value` |

**style** (`class="style"`):

| field | type | notes |
|---|---|---|
| `name`, `fontname` | string | |
| `fontsize` | number | double |
| `color1`, `color2`, `color3`, `color4` | string | `"&HAABBGGRR&"` (uppercase hex, trailing `&`). Primary, secondary, outline, shadow. |
| `bold`, `italic`, `underline`, `strikeout` | boolean | |
| `scale_x`, `scale_y` | number | percent |
| `spacing`, `angle` | number | double |
| `borderstyle` | int | |
| `outline`, `shadow` | number | widths |
| `align` | int | numpad 1–9 |
| `margin_l`, `margin_r` | int | |
| `margin_t` **and** `margin_b` | int | **both = vertical margin** (`Margin[2]`) |
| `encoding` | int | |
| `relative_to` | 2 | constant |
| `raw` | | `Style: ...` in V4+ format |

There is **no `margin_v`** field (karaskel adds `l.margin_v = l.margin_t` itself) and **no `relative`** field, only `relative_to`.

**dialogue** (`class="dialogue"`):

| field | type | notes |
|---|---|---|
| `comment` | boolean | true = `Comment:` line |
| `layer` | int | |
| `start_time`, `end_time` | int | ms; stored unrounded in memory, clamped to `[0, 35999994]` on write |
| `style`, `actor`, `effect`, `text` | string | |
| `margin_l`, `margin_r` | int | |
| `margin_t` **and** `margin_b` | int | **both = `MarginV`**; no `margin_v` field |
| `extra` | table | key → value strings from Aegisub extradata; `{}` if none |
| `raw` | string | `Dialogue: L,H:MM:SS.cc,H:MM:SS.cc,Style,Actor,ML,MR,MV,Effect,{=id=id}Text`; CR/LF removed |

### 9.3 Line tables (write: `LuaToAssEntry`)

- The table must have a string `class`, which is **case-insensitive**. Otherwise:
  - `Can't convert a non-table value to AssEntry`;
  - `Table lacks 'class' field, can't convert to AssEntry`;
  - `Found line with unknown class: <c>`.
- Required fields per class (a missing or wrongly typed field raises `Invalid or missing field '<f>' in '<class>' class subtitle line (expected <type>)`):
  - info: `key`, `value` (strings; numbers accepted).
  - style: `name`, `fontname`, `fontsize`, `color1`–`color4`, `bold`, `italic`, `underline`, `strikeout`, `scale_x`, `scale_y`, `spacing`, `angle`, `borderstyle`, `outline`, `shadow`, `align`, `margin_l`, `margin_r`, `margin_t`, `encoding`.
    - The booleans must be real booleans.
    - Colors are any parseable `agi::Color` (§4.3); an unparseable color silently becomes black.
    - **`margin_b` is ignored.**
  - dialogue: `comment` (bool), `layer`, `start_time`, `end_time`, `style`, `actor`, `margin_l`, `margin_r`, `margin_t`, `effect`, `text`; `extra` is optional.
    - **`margin_b` is ignored.**
    - `extra` must be a table or nil, otherwise `dialogue extradata must be a table`.
    - Non-string keys in `extra` are skipped. Non-string values become `"<not a string>"`, numbers are stringified.
    - Extradata ids are sorted. Identical key/value pairs are deduplicated by `AssFile::AddExtradata`.
- `raw` and `section` are **ignored** on write. The section is inferred from `class`.
- Integer fields are truncated with `lua_tointeger`. Numeric strings are accepted where `lua_isnumber`.

### 9.4 Operations

| Lua | Behaviour |
|---|---|
| `#subs`, `subs.n` | the current `#lines` (all groups) |
| `subs[i]` (number) | bounds `1..n`, else `Requested out-of-range line from subtitle file: %d` |
| `subs["delete" \| "deleterange" \| "insert" \| "append" \| "script_resolution"]` | returns a closure. Use `subs.delete(...)`, not `subs:delete`. Calling with `:` passes the userdata as the first arg, which `check_uint` then rejects. |
| `subs.<other string>` | error `Invalid indexing in Subtitle File object: '<k>'` |
| `subs[<other type>]` | error `Attempt to index a Subtitle File object with value of type '<t>'.` |
| `subs[i] = line`, `i > 0` | bounds-checked `1..n`; replaces the entry. **Cannot append via `subs[n+1]`.** |
| `subs[i] = nil`, `i > 0` | `delete(i)` |
| `subs[0] = line` | `append(line)` |
| `subs[-i] = line` | `insert(i, line)` |
| `subs.append(l1, l2, ...)` | for each line: inserted **after the last existing line of the same group** (info after the last info, style after the last style, dialogue after the last dialogue). If no line of that group exists, it goes at the end of the vector. |
| `subs.insert(i, l1, ...)` | `i` in `1..n+1` (`n+1` = append semantics), else argerror `Out of range line index`. Inserts **before** `i`, in order. Any class may be inserted anywhere in the vector. |
| `subs.delete(i1, i2, ...)` or `subs.delete({i1, ...})` | Indices refer to numbering **before** the call. Each is range-checked. The ids are sorted, then removed in one pass. **Bug:** duplicate indices stop deletion of later ids. FS Editor should de-duplicate first (superset behaviour). |
| `subs.deleterange(a, b)` | inclusive; `a` is clamped to ≥1 and `b` to ≤n; nothing happens if `a > b`; negative values raise "must be >= 0" |
| `subs.script_resolution()` | `PlayResX, PlayResY` with Gabest defaults. Both 0 → 384×288. Only one set → the other is derived: `1280↔1024` special case, else 4:3. |
| `ipairs(subs)` | via the `__ipairs` metamethod (needs 5.2 compat). Yields `i, line_table` for `i = 1..n`, reading `#lines` live at each step. |
| `pairs(subs)` | not supported |
| any modifying op in a read-only context | `Attempt to modify subtitles in read-only feature context.` |
| use after the run has ended | `Subtitles object is no longer valid` |

- **Section inference and commit:** on success (`ProcessingComplete`), Aegisub clears its Info (only if info was touched), Styles and Events, then walks `lines` in order and pushes each entry into the list for its **class**. Groups are therefore re-formed by class, keeping relative order within each class. A dialogue inserted at index 2 still ends up in Events.
  - The macro's returned selection indices refer to the **final `lines` vector**, not to the re-grouped file.
- The commit type is the OR of the per-line masks: info → SCRIPTINFO, style → STYLES, dialogue → DIAG_ADDREM. This is used for UI refresh.

---

## 10. `aegisub.text_extents(style, text)` (`auto4_base.cpp: CalculateTextExtents`)

**Validation:**
- Arg 1 must be a table whose `class` lowercases to `"style"`. Otherwise: `Not a style entry`.
- Arg 2 must be a string (numbers are allowed).
- The style table is fully converted with `LuaToAssEntry`, so **every style field is required** (§9.3).
- **[TT]** The computation runs synchronously on the main thread.

**Windows algorithm (GDI, the reference behaviour the scripts were tuned for):**

1. `fontsize = style.fontsize * 64`, `spacing = style.spacing * 64`.
2. `CreateCompatibleDC(NULL)`, `MM_TEXT`, then `CreateFontIndirect` with:
   - `lfHeight = (LONG)fontsize`. **Positive means cell height**: ascent+descent = size*64, not em size.
   - `lfWeight = bold ? FW_BOLD(700) : FW_NORMAL(400)`.
   - `lfItalic`, `lfUnderline`, `lfStrikeOut` from the style.
   - `lfCharSet = style.encoding`.
   - `OUT_TT_PRECIS`, `CLIP_DEFAULT_PRECIS`, `ANTIALIASED_QUALITY`, `DEFAULT_PITCH|FF_DONTCARE`.
   - `lfFaceName` = the first 31 UTF-16 units of `fontname`.
3. Text is converted to UTF-16.
   - If `spacing != 0`: for **each UTF-16 code unit**, `GetTextExtentPoint32` of that unit. `width += cx + spacing`, so spacing is added after every unit, including the last. `height = cy` of the last unit. There is **no kerning**.
   - Else: `GetTextExtentPoint32` of the whole string, giving `width=cx`, `height=cy`. Kerning and shaping follow GDI.
4. `GetTextMetrics`: `descent = tmDescent`, `extlead = tmExternalLeading`.
5. Scaling:
   - `width = scale_x/100 * width / 64`;
   - `height = scale_y/100 * height / 64`;
   - `descent = scale_y/100 * descent / 64`;
   - `extlead = scale_y/100 * extlead / 64`.
6. Return `width, height, descent, extlead`. These are doubles with sub-pixel precision thanks to ×64.

**Non-Windows (wx):** `wxFont(pointSize=(int)fontsize, …, faceName)` plus `GetTextExtent`.
- Each metric is multiplied by `scaling = fontsize / measured_height`, so height is normalised to the requested size. Then the same /64 and scale is applied.
- With spacing it is per character: `width += (a + spacing) * scaling`, and the maximum height, descent and extlead are kept.

**Not handled** (by design): override tags, `\N`, line breaks, drawings and wrapping. Text is measured literally. karaskel passes stripped text.

**FS Editor implementation:**
- On Windows, call GDI exactly as above. This gives bit-compatible results with Aegisub, which matters for karaoke templates and typesetting scripts such as ASSFoundation, a-mo and line0 tools.
- Elsewhere, use FreeType via the same font lookup as the renderer (libass/fontconfig), emulating GDI semantics:
  - size the face so that `winAscent + winDescent` (OS/2) = `fontsize*64` in 26.6 units;
  - width = sum of advances (with kerning when spacing = 0);
  - height = ascent + descent;
  - descent = winDescent scaled;
  - extlead = `max(0, hhea.lineGap - ((winAscent + winDescent) - (hhea.ascender - hhea.descender)))`. This is GDI's tmExternalLeading rule as reimplemented by Wine; verify it against real GDI output;
  - fake bold and italic as GDI would.
- Results will match Aegisub-on-Linux better than Aegisub-on-Windows. Document that difference.

---

## 11. Karaoke templater (`automation/autoload/kara-templater.lua`, v2.1.7; identical in TT and arch)

It is a plain BSD-3 script and needs no host support beyond the standard API:

- `include("karaskel.lua")`, and through it `utils.lua` → `aegisub.util`, and `unicode.lua` → `aegisub.unicode`, which needs the `__unicode_impl` FFI.
- `aegisub.gettext`. Its `script_name` and `script_description` are translated strings.
- `register_macro(tr"Apply karaoke template", …, macro_apply_templates, macro_can_template)`:
  - The **validator** (`macro_can_template`) iterates `#subs` and `subs[i]`, looking for an `effect` whose first word is `template` within the first 50 dialogue lines. It must work with a read-only subs object and **without** `aegisub.progress`.
- `register_filter(tr"Karaoke template", …, 2000, filter_apply_templates)`.
- Runtime uses:
  - `aegisub.progress.set`, `aegisub.progress.task`, `aegisub.progress.is_cancelled`;
  - `aegisub.debug.out` at levels 2–5;
  - `aegisub.cancel()` on template errors;
  - `aegisub.set_undo_point("apply karaoke template")` in the macro path only;
  - `aegisub.text_extents`, per character for `char` templates and via karaskel;
  - `aegisub.parse_karaoke_data` (via karaskel), which relies on `tag` including the backslash and on syllable 0 existing;
  - `subs.delete(i)`, `subs.append`, `subs[i] = l`, `subs[-i]` (karaskel inserts furigana styles with `subs[-first_style_line] = style`);
  - `subs.script_resolution()`, `aegisub.video_size()` (only `video_x`/`video_y` for the correction factor; `nil` means no correction).
- It uses `loadstring` and `setfenv` for template code. LuaJIT provides both, and `setfenv` works even with 5.2 compat. The template environment exposes `_G`, `string` and `math`.
- No `dofile` or `loadfile` (both are removed in Aegisub; templater doesn't need them).
- `karaskel.collect_head` iterates `ipairs(subs)` and **stops at the first non-info, non-style line**, so the host must order the vector as info → styles → events. It errors `Karaskel error: No styles were found in the file, bug?!` without styles.

---

## 12. FS Editor implementation checklist (mlua)

1. **Build:** mlua with features `luajit52`, `vendored`, and `send` (if scripts run on worker threads). Use the dynamic CRT on Windows.
2. **State:** one Lua state per script, created with `Lua::unsafe_new_with(StdLib::ALL, …)`. Then:
   - preload `aegisub.__re_impl`, `aegisub.__unicode_impl`, `aegisub.__lfs_impl`, `lpeg`, and optionally `luabins`;
   - set `dofile = nil`, `loadfile = nil`, and define `include`;
   - build `package.path` from the script dir and the include dirs; replace `package.loaders[2]` with a `.moon`-aware UTF-8 loader;
   - on Windows, run the monkeypatch;
   - load `moonscript.lua` from **our data dir by exact path**.
3. **Chunk names:** plain path, no `@`. Port `add_stack_trace`, including MoonScript line mapping.
4. **`aegisub` table:** §2.1. Implement `cancel` as Lua `error(nil)`, or as an mlua error variant mapped to a nil error value, and treat a nil error as a silent rollback. Install `progress`, `debug`, `log` and `dialog` per processing run and remove them afterwards.
5. **Subtitles object:** a userdata with `__index`, `__newindex`, `__len`, `__ipairs` and `__gc`.
   - It works on a copy-on-write line vector, and commits by class-regrouping.
   - It supports undo-point snapshots, read-only mode and expiry.
6. **Macro protocol:** absolute indices; the `0` vs `nil` active-line difference between process and validate; selection and active rules (§3.1); a `/` submenu tree; dynamic help from validate.
7. **Filters:** config dialog without buttons, serialised settings in project properties, no `dialog`, no undo.
8. **FFI-backed modules:**
   - vendor `aegisub/*.moon`, `aegisub/internal/*.moon` plus the `aegisub/argcheck.moon` and `aegisub/ffi.moon` aliases, `clipboard.lua`, `re.lua`, `unicode.lua`, `lfs.lua`, `utils*.lua`, `karaskel*.lua`, `cleantags.lua`;
   - implement the native tables of §5.3 as `extern "C"` Rust functions handed over as `ffi.cast` function pointers;
   - use **`char*` return type only for malloc'd strings**.
9. **Paths:** §8 tokens with FS Editor dirs. Offer Aegisub's user automation dirs as extra autoload and include roots for drop-in compatibility. Implement trust prompts for local scripts.
10. **Conformance:** run Aegisub's `automation/tests/modules/{re,unicode,lfs,argcheck}.moon` busted specs and `automation/tests/automation/*.lua` in FS Editor's headless host. Run DepCtrl's own test suite (`l0.DependencyControl.Toolbox/test.moon`), kara-templater on `kara-templater-retime.ass` and `furi-test.ass`, and a smoke test of ASSFoundation, Aegisub-Motion, unanimated, petzku and arch1t3cht script packs.
