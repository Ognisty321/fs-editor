-- The part of the busted API that Aegisub's module specs use
-- (describe/it/before_each/after_each and chained luassert assertions).
-- Runs one spec file and returns "passed failed" plus failure messages.

local spec_path = ...

local function deep_equal(a, b)
  if a == b then return true end
  if type(a) ~= 'table' or type(b) ~= 'table' then return false end
  for k, v in pairs(a) do if not deep_equal(v, b[k]) then return false end end
  for k in pairs(b) do if a[k] == nil then return false end end
  return true
end

local checks = {
  equal = function(e, a) return e == a, ('expected %s, got %s'):format(tostring(e), tostring(a)) end,
  equals = function(e, a) return e == a, ('expected %s, got %s'):format(tostring(e), tostring(a)) end,
  same = function(e, a) return deep_equal(e, a), 'tables differ' end,
  ['nil'] = function(v) return v == nil, 'got ' .. tostring(v) end,
  ['true'] = function(v) return v == true, 'got ' .. tostring(v) end,
  ['false'] = function(v) return v == false, 'got ' .. tostring(v) end,
  truthy = function(v) return not not v, 'got ' .. tostring(v) end,
  falsy = function(v) return not v, 'got ' .. tostring(v) end,
  error = function(f, ...) return not pcall(f, ...), 'no error raised' end,
}
checks.errors = checks.error

local function chain(negate)
  return setmetatable({}, {
    __index = function(_, key)
      local neg = negate
      local last
      for part in key:gmatch('[^_]+') do
        if part == 'not' or part == 'no' then
          neg = not neg
        elseif checks[part] then
          last = part
        end
      end
      if not last then return chain(neg) end
      return function(...)
        local ok, msg = checks[last](...)
        if neg then ok = not ok end
        if not ok then
          error(('assertion %s%s failed: %s'):format(neg and 'not ' or '', last, tostring(msg)), 2)
        end
      end
    end,
    __call = function(_, v, msg, ...)
      if not v then error(msg or 'assertion failed!', 2) end
      return v, msg, ...
    end,
  })
end

local root = { name = '', children = {}, before = {}, after = {} }
local current = root

local env = setmetatable({
  assert = chain(false),
  describe = function(name, f)
    local node = { name = name, children = {}, before = {}, after = {}, parent = current }
    table.insert(current.children, node)
    local saved = current
    current = node
    f()
    current = saved
  end,
  it = function(name, f) table.insert(current.children, { name = name, test = f, parent = current }) end,
  before_each = function(f) table.insert(current.before, f) end,
  after_each = function(f) table.insert(current.after, f) end,
  setup = function(f) f() end,
  teardown = function(f) end,
  pending = function() end,
}, { __index = _G })

local moon = require 'moonscript'
local chunk = assert(moon.loadfile(spec_path))
setfenv(chunk, env)
chunk()

local passed, failed, messages = 0, 0, {}

local function hooks(node, field, out)
  if not node then return out end
  hooks(node.parent, field, out)
  for _, f in ipairs(node[field]) do table.insert(out, f) end
  return out
end

local function run(node, prefix)
  for _, c in ipairs(node.children) do
    local name = prefix .. ' ' .. c.name
    if c.test then
      local ok, err = pcall(function()
        for _, f in ipairs(hooks(node, 'before', {})) do f() end
        c.test()
      end)
      for _, f in ipairs(hooks(node, 'after', {})) do pcall(f) end
      if ok then
        passed = passed + 1
      else
        failed = failed + 1
        table.insert(messages, name .. ': ' .. tostring(err))
      end
    else
      run(c, name)
    end
  end
end
run(root, '')

return passed, failed, table.concat(messages, '\n')
