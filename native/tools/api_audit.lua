-- Rank Spring, gl, and GG names by how many Lua files touch them.
-- A sandboxed load records names that only appear when a chunk runs.
-- The ranking itself is the lexical file count, because most gadgets
-- call VFS.Include and will not execute in this host.

local root = arg[1]
local out_path = arg[2]
if not root or not out_path then
  io.stderr:write("usage: api_audit.lua ROOT OUT\n")
  os.exit(1)
end

local function list_lua(dir)
  local files = {}
  local pipe = io.popen('find "' .. dir .. '" -name "*.lua" -type f')
  if not pipe then
    return files
  end
  for line in pipe:lines() do
    files[#files + 1] = line
  end
  pipe:close()
  return files
end

local roots = {
  root .. "/luarules/gadgets",
  root .. "/luaui/Widgets",
}

local file_hits = {}
local files_scanned = 0

local function touch(api, name, path)
  local key = api .. "." .. name
  local bucket = file_hits[key]
  if not bucket then
    bucket = {}
    file_hits[key] = bucket
  end
  bucket[path] = true
end

local function scan_text(path, text)
  local seen = {}
  local function add(api, name)
    local key = api .. "." .. name
    if not seen[key] then
      seen[key] = true
      touch(api, name, path)
    end
  end
  for name in text:gmatch("Spring%.([%a_][%w_]*)") do
    add("Spring", name)
  end
  for name in text:gmatch("gl%.([%a_][%w_]*)") do
    add("gl", name)
  end
  for name in text:gmatch("GG%.([%a_][%w_]*)") do
    add("GG", name)
  end
end

local function stub(api, path)
  return setmetatable({}, {
    __index = function(_, key)
      if type(key) == "string" then
        touch(api, key, path)
      end
      return function()
        return nil
      end
    end,
    __newindex = function() end,
  })
end

local loaded_ok = 0
local loaded_err = 0

local function try_load(path, text)
  local env = {
    Spring = stub("Spring", path),
    gl = stub("gl", path),
    GG = stub("GG", path),
    Script = stub("Script", path),
    VFS = stub("VFS", path),
    widget = stub("widget", path),
    gadget = stub("gadget", path),
    pairs = pairs,
    ipairs = ipairs,
    type = type,
    tostring = tostring,
    tonumber = tonumber,
    print = function() end,
    pcall = pcall,
    error = error,
    assert = assert,
    unpack = unpack,
    select = select,
    next = next,
    setmetatable = setmetatable,
    getmetatable = getmetatable,
    rawget = rawget,
    rawset = rawset,
    math = math,
    string = string,
    table = table,
    os = { clock = os.clock, time = os.time, date = os.date },
    coroutine = coroutine,
  }
  env._G = env
  env.include = function()
    return nil
  end
  env.VFS.Include = function()
    return nil
  end
  local chunk, err = loadstring(text, path)
  if not chunk then
    loaded_err = loaded_err + 1
    return
  end
  setfenv(chunk, env)
  local ok = pcall(chunk)
  if ok then
    loaded_ok = loaded_ok + 1
  else
    loaded_err = loaded_err + 1
  end
end

for _, dir in ipairs(roots) do
  local files = list_lua(dir)
  for i = 1, #files do
    local path = files[i]
    local handle = io.open(path, "r")
    if handle then
      local text = handle:read("*a") or ""
      handle:close()
      files_scanned = files_scanned + 1
      scan_text(path, text)
      try_load(path, text)
    end
  end
end

local function count_files(bucket)
  local n = 0
  for _ in pairs(bucket) do
    n = n + 1
  end
  return n
end

local ranked = {}
for key, bucket in pairs(file_hits) do
  local api = key:match("^(%w+)%.")
  if api == "Spring" or api == "gl" or api == "GG" then
    ranked[#ranked + 1] = { key = key, n = count_files(bucket) }
  end
end
table.sort(ranked, function(a, b)
  if a.n == b.n then
    return a.key < b.key
  end
  return a.n > b.n
end)

local out = io.open(out_path, "w")
if not out then
  io.stderr:write("could not write " .. out_path .. "\n")
  os.exit(1)
end
out:write("# API file-touch ranking\n")
out:write("# files_scanned " .. files_scanned .. "\n")
out:write("# sandbox_load_ok " .. loaded_ok .. "\n")
out:write("# sandbox_load_err " .. loaded_err .. "\n")
out:write("# rank is how many gadget or widget files mention the name\n")
for i = 1, #ranked do
  out:write(string.format("%6d %s\n", ranked[i].n, ranked[i].key))
end
out:close()
io.write("api_audit files " .. files_scanned .. " names " .. #ranked .. "\n")
