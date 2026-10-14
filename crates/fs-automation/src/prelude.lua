-- FS Editor Automation 4 host, Lua half. Mirrors Aegisub's
-- libaegisub/lua/{modules,script_reader,utils}.cpp and auto4_lua.cpp.
-- Called once per script state with the native helper table from Rust;
-- returns the internal API table Rust uses to run features.

local fs, script_path, script_dir, include_dirs, support_dir, is_windows, init_clipboard, script_stem = ...

local api = {}
local raw_moon = {}
local warnings = {}
local macros, filters = {}, {}

-- Rust helpers return (true, values...) or (false, message); raise the
-- message without position info like Aegisub's C error() does.
local function check(ok, ...)
  if ok then return ... end
  error((...), 0)
end
local function wrap(f) return function(...) return check(f(...)) end end

local function check_string(v, n, fname)
  local t = type(v)
  if t == 'string' then return v end
  if t == 'number' then return tostring(v) end
  error(("bad argument #%d to '%s' (string expected, got %s)"):format(n, fname, t), 0)
end

dofile = nil
loadfile = nil

local function join(dir, name)
  local last = dir:sub(-1)
  if last == '/' or last == '\\' then return dir .. name end
  return dir .. '/' .. name
end

local moon_loadstring

local function load_file(path)
  local text, canonical = fs.read_file(path)
  if not text then return nil, canonical end
  if text:sub(1, 3) == '\239\187\191' then text = text:sub(4) end
  if canonical:lower():sub(-5) == '.moon' then
    raw_moon[canonical] = text
    return moon_loadstring(text, canonical)
  end
  return loadstring(text, canonical)
end
api.load_file = load_file

-- package.path from the include dirs (the script's own dir first)
do
  local p = {}
  for _, d in ipairs(include_dirs) do
    p[#p + 1] = d .. '/?.lua;' .. d .. '/?/init.lua;'
  end
  local path = table.concat(p)
  if not is_windows then path = path .. package.path end
  package.path = path
end

local dirsep = package.config:sub(1, 1)

-- package.loaders[2]: like the stock searcher but prefers a sibling .moon
-- and reads UTF-8 paths.
package.loaders[2] = function(name)
  local module = name:gsub('%.', dirsep):gsub('%%', '%%%%')
  for tok in package.path:gmatch('[^;]+') do
    local path = tok:gsub('%?', module)
    if path:lower():sub(-4) == '.lua' then
      local moon = path:sub(1, -5) .. '.moon'
      if fs.file_exists(moon) then path = moon end
    end
    if fs.file_exists(path) then
      local f, err = load_file(path)
      if not f then
        error(('Error loading Lua module "%s":\n%s'):format(path, tostring(err)), 0)
      end
      return f
    end
  end
end

function include(name)
  name = check_string(name, 1, 'include')
  local path
  if name:find('[/\\]') then
    local c = join(script_dir, name)
    if fs.file_exists(c) then path = c end
  else
    for _, d in ipairs(include_dirs) do
      local c = join(d, name)
      if fs.file_exists(c) then path = c; break end
    end
  end
  if not path then error('Lua include not found: ' .. name, 0) end
  local f, err = load_file(path)
  if not f then
    error(('Error loading Lua include "%s":\n%s'):format(name, tostring(err)), 0)
  end
  return f()
end

-- Support files are loaded by exact path from ?data/automation/include so a
-- moonscript.lua next to a script can't hijack loading.
if is_windows then
  local f = assert(load_file(join(support_dir, 'unicode-monkeypatch.lua')))
  f()
end
do
  local f = assert(load_file(join(support_dir, 'moonscript.lua')))
  local moon = f()
  assert(type(moon) == 'table', 'Bundled moonscript.lua did not return a module table')
  moon_loadstring = assert(moon.loadstring, 'Bundled moonscript.lua has no loadstring function')
  package.loaded.moonscript = moon
end

-- Map a Lua line of compiled MoonScript back to the .moon source line.
local function moon_line(source, line)
  local ok, tables = pcall(require, 'moonscript.line_tables')
  local tbl, raw = ok and tables[source], raw_moon[source]
  if not tbl or not raw or line < 0 then return line end
  local l, pos = line, nil
  while l > 0 and not pos do pos = tbl[l]; l = l - 1 end
  if not pos then return line end
  local n = 1
  for _ in raw:sub(1, pos):gmatch('\n') do n = n + 1 end
  return n
end

-- `level`: the first frame to report (the function that raised the error).
local function add_stack_trace(err, level)
  if type(err) == 'number' then err = tostring(err) end
  if type(err) ~= 'string' then return err end
  local frames = { (err:gsub('^%[string ".*"%]:%d+: ', '', 1)) }
  while true do
    local ar = debug.getinfo(level, 'Snl')
    if not ar then break end
    level = level + 1
    if ar.what == 'tail' then
      frames[#frames + 1] = '(tail call)'
    else
      local file, is_moon = ar.source, false
      if file == '=[C]' then
        file = '<C function>'
      elseif file:sub(-5) == '.moon' then
        is_moon = true
      end
      local function real(l) return is_moon and moon_line(ar.source, l) or l end
      local fname = ar.name or ''
      if ar.what == 'main' then
        fname = '<main>'
      elseif ar.what == 'C' then
        fname = '?'
      elseif not ar.namewhat or ar.namewhat == '' then
        fname = ('<anonymous function at lines %d-%d>'):format(real(ar.linedefined), real(ar.lastlinedefined - 1))
      end
      frames[#frames + 1] = ('    File "%s", line %d\n%s'):format(file, real(ar.currentline), fname)
    end
  end
  local out = {}
  for i = #frames, 1, -1 do out[#out + 1] = frames[i] end
  return table.concat(out, '\n')
end

-- An error inside the handler would re-enter it on the C stack until it overflows.
local function safe_stack_trace(err)
  -- levels: add_stack_trace, pcall, safe_stack_trace, then the raiser
  local ok, out = pcall(add_stack_trace, err, 4)
  if ok then return out end
  return tostring(err) .. '\n(stack trace unavailable: ' .. tostring(out) .. ')'
end

function api.run(f, ...)
  return xpcall(f, safe_stack_trace, ...)
end

-- ---- the aegisub table -----------------------------------------------------

local function last_arg(...)
  local n = select('#', ...)
  if n == 0 then return nil end
  return (select(n, ...))
end

aegisub = {
  lua_automation_version = 4,

  register_macro = function(name, description, process, validate, is_active)
    name = check_string(name, 1, 'register_macro')
    if type(process) ~= 'function' then
      error('The macro processing function must be a function', 0)
    end
    for _, m in ipairs(macros) do
      if m.name == name then
        error(("A macro named '%s' is already defined in script '%s'"):format(name, script_stem), 0)
      end
    end
    macros[#macros + 1] = {
      name = name,
      description = type(description) == 'string' and description or (type(description) == 'number' and tostring(description)) or '',
      process = process,
      validate = type(validate) == 'function' and validate or nil,
      is_active = type(is_active) == 'function' and is_active or nil,
    }
  end,

  register_filter = function(name, description, priority, process, config)
    name = check_string(name, 1, 'register_filter')
    if type(process) ~= 'function' then
      error('The filter processing function must be a function', 0)
    end
    filters[#filters + 1] = {
      name = name,
      description = type(description) == 'string' and description or (type(description) == 'number' and tostring(description)) or '',
      priority = math.floor(tonumber(priority) or 0),
      process = process,
      config = type(config) == 'function' and config or nil,
    }
  end,

  text_extents = wrap(fs.text_extents),
  frame_from_ms = function(...) return fs.frame_from_ms(tonumber(last_arg(...))) end,
  ms_from_frame = function(...) return fs.ms_from_frame(tonumber(last_arg(...))) end,
  video_size = fs.video_size,
  keyframes = fs.keyframes,
  decode_path = function(p) return fs.decode_path(check_string(p, 1, 'decode_path')) end,
  cancel = function() error(nil, 0) end,
  file_name = fs.file_name,
  gettext = function(s) return s end,
  project_properties = fs.project_properties,
  get_audio_selection = fs.get_audio_selection,
  set_status_text = function(s) return fs.set_status_text(check_string(s, 1, 'set_status_text')) end,
  __init_clipboard = init_clipboard,
  __raise_warning = function(msg) warnings[#warnings + 1] = tostring(msg) end,
  get_frame = function(n, with_subs) return fs.get_frame(math.floor(tonumber(n) or 0), not not with_subs) end,
  gui = {
    get_cursor = fs.gui_get_cursor,
    set_cursor = function(p) return fs.gui_set_cursor(math.floor(tonumber(p) or 0)) end,
    get_selection = fs.gui_get_selection,
    set_selection = function(a, b) return fs.gui_set_selection(math.floor(tonumber(a) or 0), math.floor(tonumber(b) or 0)) end,
    is_modified = fs.gui_is_modified,
  },
}

-- luabins is deprecated in Aegisub 3.5; keep the name but warn.
package.preload.luabins = function()
  aegisub.__raise_warning('luabins is deprecated and not available in FS Editor')
  return {}
end

-- ---- per-run tables ----------------------------------------------------------

local function debug_out(...)
  local n = select('#', ...)
  local args = { ... }
  local first = 1
  if type(args[1]) == 'number' then
    if args[1] > fs.trace_level() then return end
    first = 2
  end
  local msg
  if n - first + 1 > 1 then
    local ok, res = pcall(string.format, unpack(args, first, n))
    if not ok then error(res, 2) end
    msg = res
  else
    msg = check_string(args[first], first, 'out')
  end
  fs.log(msg)
end

function api.begin_run(with_progress, with_dialog)
  if with_progress then
    aegisub.progress = {
      set = function(p) fs.progress_set(tonumber(p) or 0) end,
      task = function(s) fs.progress_task(check_string(s, 1, 'task')) end,
      title = function(s) fs.progress_title(check_string(s, 1, 'title')) end,
      is_cancelled = fs.is_cancelled,
    }
    aegisub.debug = { out = debug_out }
    aegisub.log = debug_out
  end
  if with_dialog then
    aegisub.dialog = {
      display = wrap(fs.dialog_display),
      open = function(title, dir, file, wildcard, multiple, must_exist)
        return check(fs.dialog_open(check_string(title, 1, 'open'), check_string(dir, 2, 'open'),
          check_string(file, 3, 'open'), check_string(wildcard, 4, 'open'), not not multiple,
          must_exist == nil or not not must_exist))
      end,
      save = function(title, dir, file, wildcard, dont_prompt)
        return check(fs.dialog_save(check_string(title, 1, 'save'), check_string(dir, 2, 'save'),
          check_string(file, 3, 'save'), check_string(wildcard, 4, 'save'), not not dont_prompt))
      end,
    }
  end
end

function api.end_run()
  aegisub.progress, aegisub.debug, aegisub.log, aegisub.dialog = nil, nil, nil, nil
end

-- The subtitles object: a userdata proxy over the Rust handle.
function api.make_subs(h)
  local obj = newproxy(true)
  local mt = getmetatable(obj)
  local closures = {
    delete = function(...) return check(h:delete(...)) end,
    deleterange = function(...) return check(h:deleterange(...)) end,
    insert = function(...) return check(h:insert(...)) end,
    append = function(...) return check(h:append(...)) end,
    script_resolution = function() return check(h:script_resolution()) end,
  }
  mt.__index = function(_, k)
    local t = type(k)
    if t == 'number' then return check(h:get(k)) end
    if t == 'string' then
      if k == 'n' then return check(h:len()) end
      local c = closures[k]
      if c then
        check(h:len()) -- expiry check
        return c
      end
      error(("Invalid indexing in Subtitle File object: '%s'"):format(k), 0)
    end
    error(("Attempt to index a Subtitle File object with value of type '%s'."):format(t), 0)
  end
  mt.__newindex = function(_, k, v)
    if type(k) ~= 'number' then
      error(("Attempt to index a Subtitle File object with value of type '%s'."):format(type(k)), 0)
    end
    check(h:set(k, v))
  end
  mt.__len = function() return check(h:len()) end
  mt.__ipairs = function(self)
    local i = 0
    return function()
      i = i + 1
      if i <= check(h:len()) then return i, self[i] end
    end, self, 0
  end
  mt.__tostring = function() return 'Subtitle File object' end
  aegisub.parse_karaoke_data = function(line) return check(h:parse_karaoke(line)) end
  aegisub.set_undo_point = function(desc) check(h:set_undo_point(desc)) end
  return obj
end

function api.features()
  return macros, filters
end

-- Warnings raised while loading are returned once; afterwards they are
-- logged immediately (Aegisub 3.5 behaviour).
function api.take_warnings()
  local w = warnings
  warnings = {}
  aegisub.__raise_warning = function(msg)
    fs.log(("Warning in Automation script '%s':\n%s\n"):format(script_stem, tostring(msg)))
  end
  return w
end

return api
