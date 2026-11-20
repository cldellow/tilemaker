-- tilemaker: derive the way_memo / node_memo settings of a profile by static analysis.
--
-- The memo (see osm_lua_processing.h) replays way_function/node_function for objects that read the
-- same as an earlier one. That is only right if the profile keeps promises tilemaker can't check
-- while it runs: no state carried from one object to the next; tag values of "value tags" only
-- tested for "" and copied into attributes; MercatorArea() only compared with constants; the
-- relation from NextRelation() only tested for nil. This module proves them, or finds where they
-- fail, from the profile's source:
--
--   1. It runs in the profile's own Lua state, after init_function, so it sees the configuration as
--      tilemaker will: globals, tables, and the functions' upvalues (debug.getupvalue).
--   2. It interprets way_function/node_function abstractly on the AST (luacheck's parser), inlining
--      the profile's functions, pruning branches the configuration decides, unrolling loops over
--      constant tables, and following values through locals, tables and calls.
--
-- It never calls the profile's code and reads tables raw, so no metamethod of the profile runs.
-- Whatever it can't follow, it doesn't prove: the memo of that function stays off.
--
-- analyze(opts) -> report. opts:
--   api       {[function] = name}: tilemaker's Lua functions, as registered (before init_function)
--   builtins  {[function] = name}: the standard library's functions, before the profile was loaded
--   read_file function(path) -> source or nil (default: io.open)

local parser = require "luacheck.parser"
local decoder = require "luacheck.decoder"

local unpack = table.unpack or unpack
local rawlen = rawlen or function(t) return #t end
local upvalueid = debug.upvalueid
local getinfo, getupvalue, getmetatable_raw = debug.getinfo, debug.getupvalue, debug.getmetatable

local MAX_UNROLL = 256
local MAX_CONSTS = 64
local MAX_DEPTH = 40
local MAX_LOOP_ITERATIONS = 64
local WIDEN_AFTER = 4

-- ---------------------------------------------------------------- the tilemaker API

local API_REPLAYED = { Layer = true, LayerAsCentroid = true, Attribute = true, AttributeNumeric = true,
	AttributeBoolean = true, AttributeInteger = true, MinZoom = true, ZOrder = true, Score = true,
	ModifyId = true, MinZoomByArea = true }
-- Objects whose path reaches these run through Lua every time: safe, but never replayed
local API_LUA_ONLY = { Id = true, OsmType = true, AllKeys = true, AllTags = true, HasTags = true,
	SetTag = true, FindIntersecting = true, Intersects = true, FindCovering = true, CoveredBy = true,
	IsMultiPolygon = true, Area = true, Length = true, Centroid = true, RestartRelations = true,
	Accept = true, AreaIntersecting = true, SetData = true, GetData = true }

local MEMO_ENTRIES = { { "way_function", "way" }, { "node_function", "node" } }
local OTHER_ENTRIES = { "relation_scan_function", "relation_function", "relation_postscan_function" }

local PURE_BUILTINS = { tonumber = true, tostring = true, type = true, select = true, ipairs = true,
	pairs = true, next = true, rawequal = true, rawlen = true, rawget = true, unpack = true,
	["table.unpack"] = true, ["table.concat"] = true, assert = true, error = true }
local IMPURE_BUILTINS = { ["math.random"] = true, ["math.randomseed"] = true, print = true,
	require = true, dofile = true, loadfile = true, load = true, loadstring = true,
	collectgarbage = true, setmetatable = true, getmetatable = true, rawset = true,
	setfenv = true, getfenv = true, module = true }
local TABLE_MUTATORS = { ["table.insert"] = true, ["table.remove"] = true, ["table.sort"] = true }
-- Builtins that are safe to run on constants (to fold them)
local FOLDABLE_PREFIX = { "string.", "math.", "utf8." }

local function is_pure_builtin(name)
	if IMPURE_BUILTINS[name] then return false end
	if PURE_BUILTINS[name] then return true end
	for _, p in ipairs(FOLDABLE_PREFIX) do
		if name:sub(1, #p) == p then return true end
	end
	return false
end

local function has_prefix(s, p) return s:sub(1, #p) == p end

-- ---------------------------------------------------------------- abstract values
-- V: { c = set of constants (keys: enc(value)) or nil (unknown), cn = count, nn = known not nil/false,
--      t = set of taints, tab = a persistent table, lt = a table made in this run, fn = function info }
-- Taints: "v\1key" a tag value from Find, "l\1key" its length, "area" MercatorArea(), "rel" a relation.

local NIL = {}
local EMPTY = {}
local function enc(x) if x == nil then return NIL end return x end
local function dec(k) if k == NIL then return nil end return k end

local ids = setmetatable({}, { __mode = "k" })
local next_id = 0
local function id_of(x)
	local i = ids[x]
	if not i then next_id = next_id + 1; i = next_id; ids[x] = i end
	return i
end

local function set_of(list) local s = {} for _, k in ipairs(list) do s[k] = true end return s end
local function set_union(a, b)
	if a == EMPTY or next(a) == nil then return b end
	if b == EMPTY or next(b) == nil then return a end
	local s = {}
	for k in pairs(a) do s[k] = true end
	for k in pairs(b) do s[k] = true end
	return s
end
local function set_count(s) local n = 0 for _ in pairs(s) do n = n + 1 end return n end

local function V(fields)
	fields.t = fields.t or EMPTY
	return fields
end
local function const(x) return V { c = { [enc(x)] = true }, cn = 1 } end
local function top(t, nn) return V { t = t or EMPTY, nn = nn or false } end
local NILV = const(nil)

local function truthy(x) return x ~= nil and x ~= false end

local function is_known(v) return v.c ~= nil and v.tab == nil and v.lt == nil and v.fn == nil end
local function single(v)
	if is_known(v) and v.cn == 1 then return true, dec((next(v.c))) end
	return false
end
local function truth(v)
	if v.tab or v.lt or v.fn then return true end
	if v.c == nil then if v.nn then return true end return nil end
	local seen_t, seen_f = false, false
	for k in pairs(v.c) do if truthy(dec(k)) then seen_t = true else seen_f = true end end
	if seen_t and not seen_f then return true end
	if seen_f and not seen_t then return false end
	return nil
end
local function strings(v)
	if not is_known(v) then return nil end
	local out = {}
	for k in pairs(v.c) do
		if type(k) ~= "string" then return nil end
		out[#out + 1] = k
	end
	table.sort(out)
	return out
end

local function repr_const(k)
	if k == NIL then return "nil" end
	local ty = type(k)
	if ty == "number" then return "n" .. string.format("%.17g", k) end
	if ty == "string" then return "s" .. k end
	return ty:sub(1, 1) .. tostring(k)
end
local function vkey(v)
	local parts = {}
	if v.c then
		local cs = {}
		for k in pairs(v.c) do cs[#cs + 1] = repr_const(k) end
		table.sort(cs)
		parts[1] = "c" .. table.concat(cs, "\2")
	else
		parts[1] = v.nn and "T!" or "T"
	end
	local ts = {}
	for k in pairs(v.t) do ts[#ts + 1] = k end
	table.sort(ts)
	parts[2] = table.concat(ts, "\2")
	parts[3] = v.tab and id_of(v.tab) or ""
	parts[4] = v.lt and id_of(v.lt) or ""
	parts[5] = v.fn and (v.fn.kind .. ":" .. tostring(v.fn.name or "") .. ":" .. (v.fn.f and id_of(v.fn.f) or "")) or ""
	return table.concat(parts, "\3")
end

local function fn_same(a, b)
	return a and b and a.kind == b.kind and a.name == b.name and a.f == b.f
end

local function join(a, b)
	if a == nil then return b end
	if b == nil then return a end
	if a == b then return a end
	local t = set_union(a.t, b.t)
	if is_known(a) and is_known(b) then
		local c, n = {}, 0
		for k in pairs(a.c) do c[k] = true; n = n + 1 end
		for k in pairs(b.c) do if not c[k] then c[k] = true; n = n + 1 end end
		if n <= MAX_CONSTS then return V { c = c, cn = n, t = t } end
		local nn = true
		for k in pairs(c) do if not truthy(dec(k)) then nn = false end end
		return top(t, nn)
	end
	if a.tab and a.tab == b.tab then return V { tab = a.tab, t = t } end
	if a.fn and fn_same(a.fn, b.fn) then return V { fn = a.fn, t = t } end
	if a.lt and a.lt == b.lt then return V { lt = a.lt, t = t } end
	return top(t, truth(a) == true and truth(b) == true)
end
local function join_all(list)
	local out
	for _, v in ipairs(list) do out = join(out, v) end
	return out or NILV
end
local function widen(v)
	if v.c and v.cn > 1 then
		local nn = true
		for k in pairs(v.c) do if not truthy(dec(k)) then nn = false end end
		return top(v.t, nn)
	end
	return v
end

-- ---------------------------------------------------------------- the analyzer

local A = {}
A.__index = A

local function new_analyzer(opts)
	local self = setmetatable({}, A)
	self.api = opts.api or {}
	self.builtins = opts.builtins or {}
	self.read_file = opts.read_file or function(path)
		local f = io.open(path, "rb")
		if not f then return nil end
		local s = f:read("*a")
		f:close()
		return s
	end
	self.funcs = {}          -- source -> line -> Function node
	self.parsed = {}         -- source -> true | error message
	self.writes = {}         -- pname -> list of {entry, line, protected}
	self.unsafe_reads = {}   -- pname -> list: read before written in the same run
	self.mutations = {}      -- table id -> list
	self.table_reads = {}    -- table id -> list
	self.pname_desc = {}
	return self
end

function A:reset_events()
	self.ev = {}     -- taint -> {enc(event) = {kind, detail, line}}
	self.notes = {}  -- kind -> {key = {detail, line}}
	self.relreads = {}
end

function A:event(taint, kind, detail, line)
	local s = self.ev[taint]
	if not s then s = {}; self.ev[taint] = s end
	s[kind .. "\1" .. tostring(detail) .. "\1" .. tostring(line)] = { kind, detail, line }
end
function A:note(kind, detail, line)
	local s = self.notes[kind]
	if not s then s = {}; self.notes[kind] = s end
	s[tostring(detail) .. "\1" .. tostring(line)] = { detail, line }
end
-- v is used in a way that isn't one of `allow`: a use that disqualifies each of its taints
function A:use(v, detail, line, allow)
	for t in pairs(v.t) do
		local kind = t:match("^(%a+)") or t
		if not (allow and allow[t:sub(1, 1)]) then self:event(t, "other", detail, line) end
	end
end
local function push(list_map, key, item)
	local l = list_map[key]
	if not l then l = {}; list_map[key] = l end
	l[#l + 1] = item
end

-- ---- functions and their source
function A:function_node(f)
	local info = getinfo(f, "S")
	if info.what ~= "Lua" then return nil, "a C function" end
	local source, line = info.source, info.linedefined
	if self.parsed[source] == nil then
		self.parsed[source] = true
		self.funcs[source] = {}
		local src
		if source:sub(1, 1) == "@" then src = self.read_file(source:sub(2)) end
		if not src then
			self.parsed[source] = "its source can't be read (" .. source .. ")"
		else
			local ok, ast = pcall(parser.parse, decoder.decode(src))
			if not ok then
				self.parsed[source] = "its source can't be parsed: " .. tostring(type(ast) == "table" and ast.msg or ast)
			else
				local index = self.funcs[source]
				local function walk(n)
					if type(n) ~= "table" then return end
					if n.tag == "Function" and index[n.line] == nil then index[n.line] = n
					elseif n.tag == "Function" then index[n.line] = false end  -- two functions on one line
					for _, c in ipairs(n) do walk(c) end
				end
				walk(ast)
			end
		end
	end
	if self.parsed[source] ~= true then return nil, self.parsed[source] end
	local node = self.funcs[source][line]
	if not node then return nil, "its definition at " .. source:gsub("^@", "") .. ":" .. line .. " can't be found" end
	return node, nil, source
end

-- A live Lua value -> V
function A:from_lua(x)
	-- tilemaker's functions first: the binding may register them as userdata
	if x ~= nil and self.api[x] then return V { fn = { kind = "api", name = self.api[x] } } end
	local ty = type(x)
	if ty == "nil" or ty == "boolean" or ty == "number" or ty == "string" then return const(x) end
	if ty == "table" then return V { tab = x } end
	if ty == "function" then
		local api = self.api[x]
		if api then return V { fn = { kind = "api", name = api } } end
		local b = self.builtins[x]
		if b then return V { fn = { kind = "builtin", name = b, f = x } } end
		if getinfo(x, "S").what == "Lua" then return V { fn = { kind = "lua", f = x } } end
		return V { fn = { kind = "other", f = x } }
	end
	return top(nil, true)
end

-- The upvalues of a Lua function: name -> {value, key}
function A:upvalues_of(f)
	local out = {}
	local i = 1
	while true do
		local name, value = getupvalue(f, i)
		if name == nil then break end
		if name ~= "_ENV" then
			local key
			if upvalueid then key = "U\1" .. tostring(upvalueid(f, i)) else key = "U\1?" .. name end
			out[name] = { value = value, key = key }
			self.pname_desc[key] = "module local " .. name
		elseif name == "_ENV" and value ~= _G then
			out[name] = { custom_env = true }
		end
		i = i + 1
	end
	return out
end

-- ---- interpreter state
local function new_frame(upvals, ctx)
	return { scopes = { {} }, upvals = upvals, ctx = ctx, loops = {} }
end
local function copy_frame(f)
	local scopes = {}
	for i, s in ipairs(f.scopes) do
		local c = {}
		for k, v in pairs(s) do c[k] = v end
		scopes[i] = c
	end
	local loops = {}
	for i, l in ipairs(f.loops) do loops[i] = l end
	return { scopes = scopes, upvals = f.upvals, ctx = f.ctx, loops = loops }
end
local function new_state(frame)
	return { frame = frame, gw = {}, defw = {}, alive = true, luaonly = false }
end
local function copy_state(s)
	local gw, defw = {}, {}
	for k, v in pairs(s.gw) do gw[k] = v end
	for k in pairs(s.defw) do defw[k] = true end
	return { frame = copy_frame(s.frame), gw = gw, defw = defw, alive = s.alive, luaonly = s.luaonly }
end

function A:state_key(st)
	local parts = {}
	for i, scope in ipairs(st.frame.scopes) do
		local ks = {}
		for k in pairs(scope) do ks[#ks + 1] = k end
		table.sort(ks)
		for _, k in ipairs(ks) do parts[#parts + 1] = i .. ":" .. k .. "=" .. vkey(scope[k]) end
	end
	local gs = {}
	for k in pairs(st.gw) do gs[#gs + 1] = k end
	table.sort(gs)
	for _, k in ipairs(gs) do parts[#parts + 1] = "g" .. k .. "=" .. vkey(st.gw[k]) .. (st.defw[k] and "!" or "") end
	parts[#parts + 1] = st.luaonly and "L" or ""
	return table.concat(parts, "\4")
end

function A:join_states(states)
	local live = {}
	for _, s in ipairs(states) do if s.alive then live[#live + 1] = s end end
	if #live == 0 then
		local d = copy_state(states[1])
		d.alive = false
		return d
	end
	if #live == 1 then return live[1] end
	local out = copy_state(live[1])
	for i = 2, #live do
		local s = live[i]
		for si, scope in ipairs(out.frame.scopes) do
			local other = s.frame.scopes[si]
			if other then
				for k, v in pairs(scope) do
					if other[k] ~= nil then scope[k] = join(v, other[k])
					else scope[k] = nil end
				end
			end
		end
		for k, v in pairs(s.gw) do out.gw[k] = join(out.gw[k], v) end
		for k in pairs(out.defw) do if not s.defw[k] then out.defw[k] = nil end end
		out.luaonly = out.luaonly and s.luaonly
	end
	return out
end

-- ---- names
local function lookup_local(st, name)
	local scopes = st.frame.scopes
	for i = #scopes, 1, -1 do
		if scopes[i][name] ~= nil then return scopes[i] end
	end
	return nil
end

function A:pname(st, name)
	local up = st.frame.upvals[name]
	if up and up.key then return up.key, up end
	local k = "G\1" .. name
	self.pname_desc[k] = "global " .. name
	return k, nil
end

function A:read_name(st, name, line)
	if name == "_ENV" or (st.frame.upvals._ENV and st.frame.upvals._ENV.custom_env) then
		self:note("unsupported", "_ENV", line)
		return top()
	end
	local scope = lookup_local(st, name)
	if scope then return scope[name] end
	local pn, up = self:pname(st, name)
	-- On Lua 5.1 (no debug.upvalueid) two module locals with one name can't be told apart:
	-- an upvalue read then counts as unprotected, whatever was written before
	local protected = st.defw[pn] and not (up and pn:sub(1, 3) == "U\1?")
	if not protected then push(self.unsafe_reads, pn, { self.entry, line, st.luaonly }) end
	local snap
	if up then snap = self:from_lua(up.value) else snap = self:from_lua(rawget(_G, name)) end
	if st.gw[pn] then
		if st.defw[pn] then return st.gw[pn] end
		return join(st.gw[pn], snap)
	end
	return snap
end

function A:write_name(st, name, v, line, is_local)
	if is_local then st.frame.scopes[#st.frame.scopes][name] = v; return end
	if name == "_ENV" then self:note("unsupported", "_ENV", line) end
	local scope = lookup_local(st, name)
	if scope then scope[name] = v; return end
	local pn = self:pname(st, name)
	push(self.writes, pn, { self.entry, line, st.luaonly })
	st.gw[pn] = v
	st.defw[pn] = true
end

-- (get, set) for a local, or a persistent name written in this run
function A:var_slot(st, name)
	local scope = lookup_local(st, name)
	if scope then
		return function() return scope[name] end, function(v) scope[name] = v end
	end
	local pn = self:pname(st, name)
	if st.defw[pn] then
		return function() return st.gw[pn] end, function(v) st.gw[pn] = v end
	end
	return nil
end

-- ---- running an entry point
function A:run_entry(name)
	local f = rawget(_G, name)
	if type(f) ~= "function" then return false end
	self.entry = name
	local fv = self:from_lua(f)
	if fv.fn.kind ~= "lua" then return false end
	local st = new_state(new_frame({}, { returns = {} }))
	self:call_lua(fv, {}, st, 0, 0)
	return true
end

function A:call_lua(fv, args, st, line, depth)
	local fnode, why = self:function_node(fv.fn.f)
	if not fnode or depth > MAX_DEPTH then
		self:note("unanalysed-call", why or "calls nested too deep", line)
		for _, a in ipairs(args) do self:use(a, "passed to a function memo analysis could not follow", line) end
		return { top() }
	end
	local ctx = { returns = {} }
	local cst = new_state(new_frame(self:upvalues_of(fv.fn.f), ctx))
	for k, v in pairs(st.gw) do cst.gw[k] = v end
	for k in pairs(st.defw) do cst.defw[k] = true end
	cst.luaonly = st.luaonly
	local params = fnode[1]
	for i, p in ipairs(params) do
		if p.tag == "Dots" then
			local rest = {}
			for j = i, #args do rest[#rest + 1] = args[j] end
			cst.frame.scopes[1]["..."] = #rest > 0 and join_all(rest) or NILV
			break
		end
		cst.frame.scopes[1][p[1]] = args[i] or NILV
	end
	local fin = self:exec_block(fnode[2], cst, depth)
	local outs = { fin }
	local results = {}
	if fin.alive then results[#results + 1] = {} end
	for _, r in ipairs(ctx.returns) do outs[#outs + 1] = r[1]; results[#results + 1] = r[2] end
	local any_alive = false
	for _, o in ipairs(outs) do if o.alive then any_alive = true end end
	if any_alive then
		local st2 = self:join_states(outs)
		st.gw, st.defw, st.luaonly = st2.gw, st2.defw, st2.luaonly
	end
	local n = 1
	for _, r in ipairs(results) do if #r > n then n = #r end end
	local res = {}
	for i = 1, n do
		local vs = {}
		for _, r in ipairs(results) do vs[#vs + 1] = r[i] or NILV end
		res[i] = join_all(vs)
	end
	return res
end

-- ---- statements
function A:exec_block(block, st, depth)
	table.insert(st.frame.scopes, {})
	for _, stmt in ipairs(block) do
		if not st.alive then break end
		st = self:exec_stmt(stmt, st, depth)
	end
	table.remove(st.frame.scopes)
	return st
end

function A:exec_stmt(s, st, depth)
	local tag, line = s.tag, s.line
	if tag == "Local" then
		local vals = s[2] and self:eval_list(s[2], st, depth, #s[1]) or {}
		for i, t in ipairs(s[1]) do self:write_name(st, t[1], vals[i] or NILV, line, true) end
		return st
	elseif tag == "Localrec" then
		self:note("unsupported", "local function defined while processing an object", line)
		self:write_name(st, s[1][1][1], top(nil, true), line, true)
		return st
	elseif tag == "Set" then
		local vals = self:eval_list(s[2], st, depth, #s[1])
		for i, t in ipairs(s[1]) do
			local v = vals[i] or NILV
			if t.tag == "Id" then self:write_name(st, t[1], v, line)
			elseif t.tag == "Index" then self:assign_index(t, v, st, depth, line)
			else self:note("unsupported", "assignment to " .. tostring(t.tag), line) end
		end
		return st
	elseif tag == "Call" or tag == "Invoke" then
		self:eval_multi(s, st, depth)
		return st
	elseif tag == "If" then
		return self:exec_if(s, 1, st, depth)
	elseif tag == "Do" then
		return self:exec_block(s, st, depth)
	elseif tag == "Return" then
		local vals = self:eval_list(s, st, depth, nil)
		table.insert(st.frame.ctx.returns, { copy_state(st), vals })
		st.alive = false
		return st
	elseif tag == "Break" then
		local loops = st.frame.loops
		if #loops > 0 then table.insert(loops[#loops], copy_state(st)) end
		st.alive = false
		return st
	elseif tag == "While" then
		return self:exec_loop(st, depth, s[1], s[2], false)
	elseif tag == "Repeat" then
		return self:exec_loop(st, depth, s[2], s[1], true)
	elseif tag == "Fornum" then
		return self:exec_fornum(s, st, depth)
	elseif tag == "Forin" then
		return self:exec_forin(s, st, depth)
	elseif tag == "Goto" or tag == "Label" then
		self:note("unsupported", "goto/label", line)
		return st
	end
	self:note("unsupported", tostring(tag), line)
	return st
end

function A:exec_if(s, i, st, depth)
	local cond, block = s[i], s[i + 1]
	if block == nil then  -- the else block (or none)
		if cond then return self:exec_block(cond, st, depth) end
		return st
	end
	local c = self:eval(cond, st, depth)
	local t = truth(c)
	local outs = {}
	if t ~= false then
		local a = copy_state(st)
		self:refine(cond, a, true, depth)
		outs[#outs + 1] = self:exec_block(block, a, depth)
	end
	if t ~= true then
		local b = copy_state(st)
		self:refine(cond, b, false, depth)
		outs[#outs + 1] = self:exec_if(s, i + 2, b, depth)
	end
	return self:join_states(outs)
end

local LITERAL = { String = true, Number = true, Nil = true, True = true, False = true }
local function literal(e)
	if e.tag == "String" then return e[1] end
	if e.tag == "Number" then return tonumber(e[1]) end
	if e.tag == "True" then return true end
	if e.tag == "False" then return false end
	return nil
end

-- What a branch learns from its test: x == "c" makes x "c"; x makes x non-nil
function A:refine(test, st, t, depth)
	local tag = test.tag
	if tag == "Paren" then return self:refine(test[1], st, t, depth) end
	if tag == "Op" then
		local op = test[1]
		if op == "not" then return self:refine(test[2], st, not t, depth) end
		if op == "and" and t then self:refine(test[2], st, true, depth); self:refine(test[3], st, true, depth); return end
		if op == "or" and not t then self:refine(test[2], st, false, depth); self:refine(test[3], st, false, depth); return end
		if op == "eq" or op == "ne" then
			local eq = (op == "eq") == t
			for _, pair in ipairs({ { test[2], test[3] }, { test[3], test[2] } }) do
				local var, other = pair[1], pair[2]
				if var.tag == "Id" and LITERAL[other.tag] then
					local get, put = self:var_slot(st, var[1])
					if not get then return end
					local cur = get()
					local c = enc(literal(other))
					if eq then
						put(V { c = { [c] = true }, cn = 1, t = cur.t })
					elseif is_known(cur) and cur.c[c] then
						local rest, n = {}, 0
						for k in pairs(cur.c) do if k ~= c then rest[k] = true; n = n + 1 end end
						if n > 0 then put(V { c = rest, cn = n, t = cur.t }) end
					end
					return
				end
			end
		end
		return
	end
	if tag == "Id" then
		local get, put = self:var_slot(st, test[1])
		if not get then return end
		local cur = get()
		if is_known(cur) then
			local keep, n = {}, 0
			for k in pairs(cur.c) do if truthy(dec(k)) == t then keep[k] = true; n = n + 1 end end
			if n > 0 then put(V { c = keep, cn = n, t = cur.t }) end
		elseif t then
			put(V { c = cur.c, cn = cur.cn, nn = true, t = cur.t, tab = cur.tab, lt = cur.lt, fn = cur.fn })
		end
	end
end

-- Runs a loop body to a fixpoint: each pass starts from the join of the entry and the last pass's end.
-- `iterate(state)` runs one pass and returns the state after it (alive or not); `exits` collects the
-- states that leave the loop.
function A:fixpoint(st, depth, iterate, line)
	local entry = st
	local key = self:state_key(entry)
	for n = 1, MAX_LOOP_ITERATIONS do
		local out = iterate(copy_state(entry))
		if not out.alive then return true end
		local nxt = self:join_states({ entry, out })
		if n >= WIDEN_AFTER then
			for _, scope in ipairs(nxt.frame.scopes) do for k, v in pairs(scope) do scope[k] = widen(v) end end
			for k, v in pairs(nxt.gw) do nxt.gw[k] = widen(v) end
		end
		local nk = self:state_key(nxt)
		if nk == key then return true end
		entry, key = nxt, nk
	end
	self:note("unsupported", "a loop memo analysis could not bring to a fixpoint", line)
	return false
end

function A:exec_loop(st, depth, test, body, is_repeat)
	local breaks, exits = {}, {}
	table.insert(st.frame.loops, breaks)
	local line = test and test.line
	self:fixpoint(st, depth, function(entry)
		if not is_repeat then
			local c = truth(self:eval(test, entry, depth))
			if c ~= true then
				local e = copy_state(entry)
				self:refine(test, e, false, depth)
				exits[#exits + 1] = e
			end
			if c == false then local d = copy_state(entry); d.alive = false; return d end
			self:refine(test, entry, true, depth)
		end
		local out = self:exec_block(body, entry, depth)
		if is_repeat and out.alive then
			-- (the test sees the body's locals: evaluate it in the body's scope)
			local c = truth(self:eval(test, out, depth))
			if c ~= false then exits[#exits + 1] = copy_state(out) end
			if c == true then local d = copy_state(out); d.alive = false; return d end
		end
		return out
	end, line)
	table.remove(st.frame.loops)
	for _, b in ipairs(breaks) do exits[#exits + 1] = b end
	if #exits == 0 then local d = copy_state(st); d.alive = false; return d end
	local res = self:join_states(exits)
	while #res.frame.scopes > #st.frame.scopes do table.remove(res.frame.scopes) end
	return res
end

function A:exec_fornum(s, st, depth)
	local body = s[#s]
	local a = self:eval(s[2], st, depth)
	local b = self:eval(s[3], st, depth)
	local c = (#s == 5) and self:eval(s[4], st, depth) or const(1)
	for _, v in ipairs({ a, b, c }) do self:use(v, "used as a numeric for bound", s.line) end
	local vals
	local oka, lo = single(a)
	local okb, hi = single(b)
	local okc, step = single(c)
	if oka and okb and okc and type(lo) == "number" and type(hi) == "number" and type(step) == "number"
		and step ~= 0 and math.abs((hi - lo) / step) < MAX_UNROLL then
		vals = {}
		local i = lo
		while (step > 0 and i <= hi) or (step < 0 and i >= hi) do
			vals[#vals + 1] = { const(i) }
			i = i + step
		end
	end
	return self:run_iterations({ s[1] }, body, st, depth, vals, { top(nil, true) }, s.line)
end

function A:exec_forin(s, st, depth)
	local names, exprs, body = s[1], s[2], s[3]
	local iters
	local tops = { top(nil, true), top() }
	local it = (#exprs == 1) and exprs[1] or nil
	if it and it.tag == "Call" then
		local f = self:eval(it[1], st, depth)
		local fname = f.fn and f.fn.kind == "builtin" and f.fn.name or nil
		local tv = (#it == 2 and (fname == "ipairs" or fname == "pairs")) and self:eval(it[2], st, depth) or top()
		if (fname == "ipairs" or fname == "pairs") and tv.tab then
			if self:table_has_meta(tv.tab, s.line) then
				iters = nil
			else
				self:table_read(tv.tab, st, s.line)
				local items = {}
				if fname == "ipairs" then
					local i = 1
					while rawget(tv.tab, i) ~= nil and #items <= MAX_UNROLL do
						items[#items + 1] = { const(i), self:from_lua(rawget(tv.tab, i)) }
						i = i + 1
					end
				else
					-- pairs: every element is analysed, so the order doesn't matter
					local k, v = next(tv.tab)
					while k ~= nil and #items <= MAX_UNROLL do
						items[#items + 1] = { self:from_lua(k), self:from_lua(v) }
						k, v = next(tv.tab, k)
					end
				end
				if #items <= MAX_UNROLL then iters = items end
			end
		elseif (fname == "ipairs" or fname == "pairs") and tv.lt then
			tops = { top(nil, true), top(tv.lt.taints) }
		else
			local vals = self:eval_multi(it, st, depth)
			local t = EMPTY
			for _, v in ipairs(vals) do t = set_union(t, v.t) end
			if f.fn and f.fn.kind == "builtin" and f.fn.name ~= "string.gmatch" and f.fn.name ~= "pairs"
				and f.fn.name ~= "ipairs" and f.fn.name ~= "next" then
				self:note("unsupported", "a for loop over " .. f.fn.name, s.line)
			elseif not (f.fn and f.fn.kind == "builtin") then
				self:note("unsupported", "a for loop over an iterator memo analysis could not follow", s.line)
			end
			tops = { top(t, true), top(t) }
		end
	else
		for _, e in ipairs(exprs) do self:eval(e, st, depth) end
		self:note("unsupported", "a for loop over an iterator memo analysis could not follow", s.line)
	end
	return self:run_iterations(names, body, st, depth, iters, tops, s.line)
end

function A:run_iterations(targets, body, st, depth, iters, tops, line)
	local breaks = {}
	table.insert(st.frame.loops, breaks)
	local exits = {}
	if iters then
		local cur = st
		for _, vals in ipairs(iters) do
			local b = copy_state(cur)
			table.insert(b.frame.scopes, {})
			for i, t in ipairs(targets) do b.frame.scopes[#b.frame.scopes][t[1]] = vals[i] or NILV end
			local out = self:exec_block(body, b, depth)
			if out.alive then
				table.remove(out.frame.scopes)
				cur = out
			end
			-- (a body that ended in break/return: its states were collected; later iterations start
			-- from the state before it, which covers the paths that didn't break)
		end
		exits[1] = cur
	else
		exits[1] = copy_state(st)
		self:fixpoint(st, depth, function(entry)
			table.insert(entry.frame.scopes, {})
			for i, t in ipairs(targets) do entry.frame.scopes[#entry.frame.scopes][t[1]] = tops[i] or top() end
			local out = self:exec_block(body, entry, depth)
			if out.alive then
				table.remove(out.frame.scopes)
				exits[#exits + 1] = copy_state(out)
			end
			return out
		end, line)
	end
	table.remove(st.frame.loops)
	for _, b in ipairs(breaks) do exits[#exits + 1] = b end
	for _, e in ipairs(exits) do
		while #e.frame.scopes > #st.frame.scopes do table.remove(e.frame.scopes) end
	end
	return self:join_states(exits)
end

-- Persistent tables
function A:table_has_meta(t, line)
	if t == _G then
		self:note("unsupported", "_G used as a table", line)
		return true
	end
	local mt = getmetatable_raw(t)
	if mt ~= nil then
		self:note("unsupported", "a table with a metatable", line)
		return true
	end
	return false
end
function A:table_read(t, st, line)
	push(self.table_reads, id_of(t), { self.entry, line, st.luaonly })
end

function A:assign_index(target, v, st, depth, line)
	local tv = self:eval(target[1], st, depth)
	local kv = self:eval(target[2], st, depth)
	self:use(kv, "used as a table key", line)
	if tv.lt then
		tv.lt.taints = set_union(tv.lt.taints, v.t)
		return
	end
	if tv.tab then
		if tv.tab == _G then
			local ok, k = single(kv)
			if ok and type(k) == "string" then self:write_name(st, k, v, line); return end
			self:note("unsupported", "_G written with a key that is not a constant", line)
			return
		end
		push(self.mutations, id_of(tv.tab), { self.entry, line, st.luaonly })
		self:use(v, "stored in a table that outlives the object", line)
		return
	end
	self:use(v, "stored in a table memo analysis could not follow", line)
	self:note("unsupported", "a write to a table memo analysis could not follow", line)
end

-- ---- expressions
function A:eval_list(exprs, st, depth, want)
	local out = {}
	local n = #exprs
	for i = 1, n do
		local e = exprs[i]
		if want and i > want and e.tag == "Id" then
			-- an extra value that is discarded: reading a name has no effect
		elseif i == n and (e.tag == "Call" or e.tag == "Invoke" or e.tag == "Dots") then
			for _, v in ipairs(self:eval_multi(e, st, depth)) do out[#out + 1] = v end
		else
			out[#out + 1] = self:eval(e, st, depth)
		end
	end
	return out
end

function A:eval(e, st, depth)
	local tag = e.tag
	if tag == "Call" or tag == "Invoke" then
		return self:eval_multi(e, st, depth)[1] or NILV
	end
	return self:eval1(e, st, depth)
end

function A:index(tv, kv, st, line)
	self:use(kv, "used as a table key", line)
	if tv.tab then
		if tv.tab == _G then
			local ok, k = single(kv)
			if ok and type(k) == "string" then return self:read_name(st, k, line) end
			self:note("unsupported", "_G read with a key that is not a constant", line)
			return top()
		end
		if self:table_has_meta(tv.tab, line) then return top() end
		self:table_read(tv.tab, st, line)
		if next(kv.t) == nil then
			local ok, k = single(kv)
			if ok and k ~= nil and k == k then return self:from_lua(rawget(tv.tab, k)) end
			if is_known(kv) and kv.cn <= MAX_CONSTS then
				local vs = {}
				for c in pairs(kv.c) do
					local k2 = dec(c)
					if k2 == nil or k2 ~= k2 then vs[#vs + 1] = NILV else vs[#vs + 1] = self:from_lua(rawget(tv.tab, k2)) end
				end
				return join_all(vs)
			end
		end
		-- a key that is not known: any value of the table, or nil
		local vs = { NILV }
		local k, v = next(tv.tab)
		while k ~= nil do
			vs[#vs + 1] = self:from_lua(v)
			if #vs > MAX_CONSTS * 4 then return top() end
			k, v = next(tv.tab, k)
		end
		return join_all(vs)
	end
	if tv.lt then return top(tv.lt.taints) end
	if next(tv.t) ~= nil or strings(tv) then self:use(tv, "indexed", line) end
	return top()
end

local ARITH = { add = "+", sub = "-", mul = "*", div = "/", mod = "%", pow = "^", idiv = "//",
	band = "&", bor = "|", bxor = "~", shl = "<<", shr = ">>", concat = ".." }
local ORDER = { lt = true, le = true, gt = true, ge = true }

local folders = {}
local function folder(op, unary)
	local key = (unary and "u" or "b") .. op
	if folders[key] == nil then
		local src = unary and ("return function(a) return " .. op .. " a end")
			or ("return function(a, b) return a " .. op .. " b end")
		local chunk = (loadstring or load)(src)
		folders[key] = chunk and chunk() or false
	end
	return folders[key]
end

function A:fold(op, vals, unary)
	local args = {}
	for i, v in ipairs(vals) do
		local ok, x = single(v)
		if not ok or next(v.t) ~= nil then return top(nil, true) end
		args[i] = x
	end
	local f = folder(op, unary)
	if not f then return top(nil, true) end
	local ok, r = pcall(f, args[1], args[2])
	if ok and (type(r) == "number" or type(r) == "string") then return const(r) end
	return top(nil, true)
end

function A:eval1(e, st, depth)
	local tag, line = e.tag, e.line
	if LITERAL[tag] then return const(literal(e)) end
	if tag == "Id" then return self:read_name(st, e[1], line) end
	if tag == "Paren" then return self:eval(e[1], st, depth) end
	if tag == "Dots" then return st.frame.scopes[1]["..."] or top() end
	if tag == "Index" then
		local tv = self:eval(e[1], st, depth)
		local kv = self:eval(e[2], st, depth)
		return self:index(tv, kv, st, line)
	end
	if tag == "Table" then
		local lt = { taints = EMPTY }
		for _, item in ipairs(e) do
			local vexpr = item
			if item.tag == "Pair" then
				local k = self:eval(item[1], st, depth)
				self:use(k, "used as a table key", line)
				vexpr = item[2]
			end
			local vals
			if vexpr.tag == "Call" or vexpr.tag == "Invoke" or vexpr.tag == "Dots" then
				vals = self:eval_multi(vexpr, st, depth)
			else
				vals = { self:eval(vexpr, st, depth) }
			end
			for _, v in ipairs(vals) do
				lt.taints = set_union(lt.taints, v.t)
				if v.fn or v.tab then
					-- a function or a persistent table kept in a table made now: memo analysis
					-- doesn't follow values out of these tables
					lt.taints = set_union(lt.taints, EMPTY)
					self:note("unsupported", "a function or table stored in a table made while processing an object", line)
				end
			end
		end
		return V { lt = lt }
	end
	if tag == "Function" then
		self:note("unsupported", "a function made while processing an object", line)
		return top(nil, true)
	end
	if tag == "Op" then
		local op = e[1]
		if op == "and" then
			local a = self:eval(e[2], st, depth)
			local ta = truth(a)
			if ta == false then return a end
			local b2 = copy_state(st)
			self:refine(e[2], b2, true, depth)
			local b = self:eval(e[3], b2, depth)
			if ta == true then return b end
			local falsy
			if is_known(a) then
				local c, n = {}, 0
				for k in pairs(a.c) do if not truthy(dec(k)) then c[k] = true; n = n + 1 end end
				falsy = V { c = c, cn = n, t = a.t }
			else
				falsy = V { c = { [NIL] = true, [false] = true }, cn = 2, t = a.t }
			end
			if truth(b) == false then return join(falsy, b) end
			return join(is_known(a) and falsy or a, b)
		elseif op == "or" then
			local a = self:eval(e[2], st, depth)
			local ta = truth(a)
			if ta == true then return a end
			local b2 = copy_state(st)
			self:refine(e[2], b2, false, depth)
			local b = self:eval(e[3], b2, depth)
			if ta == false then return b end
			if is_known(a) then
				local c, n = {}, 0
				for k in pairs(a.c) do if truthy(dec(k)) then c[k] = true; n = n + 1 end end
				if n == 0 then c[NIL] = true; n = 1 end
				a = V { c = c, cn = n, t = a.t }
			end
			return join(a, b)
		elseif op == "not" then
			local a = self:eval(e[2], st, depth)
			-- truthiness says nothing about a tag value (always a string) or a relation id
			for t in pairs(a.t) do if t == "rel" then self:event(t, "test", "", line) end end
			local tt = truth(a)
			if tt == nil then return top() end
			return const(not tt)
		elseif op == "len" then
			return self:length_of(self:eval(e[2], st, depth), line)
		elseif op == "eq" or op == "ne" then
			return self:compare_eq(e, st, depth, line)
		elseif ORDER[op] then
			return self:compare_order(e, st, depth, line)
		elseif op == "unm" or op == "bnot" then
			local a = self:eval(e[2], st, depth)
			self:use(a, "used in arithmetic", line)
			return self:fold(op == "unm" and "-" or "~", { a }, true)
		elseif ARITH[op] then
			local a = self:eval(e[2], st, depth)
			local b = self:eval(e[3], st, depth)
			local what = op == "concat" and "concatenated" or "used in arithmetic"
			self:use(a, what, line)
			self:use(b, what, line)
			return self:fold(ARITH[op], { a, b }, false)
		end
		self:note("unsupported", "operator " .. tostring(op), line)
		return top()
	end
	self:note("unsupported", tostring(tag), line)
	return top()
end

function A:length_of(a, line)
	local t = {}
	for k in pairs(a.t) do
		if k:sub(1, 2) == "v\1" then t["l\1" .. k:sub(3)] = true
		else self:event(k, "other", "length taken", line) end
	end
	if a.tab then
		if self:table_has_meta(a.tab, line) then return top(t, true) end
		return V { c = { [rawlen(a.tab)] = true }, cn = 1, t = t }
	end
	local s = strings(a)
	if s then
		local c, n = {}, 0
		for _, x in ipairs(s) do if not c[#x] then c[#x] = true; n = n + 1 end end
		return V { c = c, cn = n, t = t }
	end
	return top(t, true)
end

local function const_list(v)
	if not is_known(v) or next(v.t) ~= nil then return nil end
	local out = {}
	for k in pairs(v.c) do out[#out + 1] = dec(k) end
	return out, v.c
end

local function show_consts(list)
	local s = {}
	for _, x in ipairs(list) do s[#s + 1] = type(x) == "string" and ("'" .. x .. "'") or tostring(x) end
	table.sort(s)
	return table.concat(s, ", ")
end

function A:area_cmp(t, ys, line)
	if ys then
		local all_numbers = true
		for _, y in ipairs(ys) do if type(y) ~= "number" or y ~= y then all_numbers = false end end
		if all_numbers then
			for _, y in ipairs(ys) do self:event(t, "cmp", y, line) end
			return
		end
	end
	self:event(t, "other", "compared with a value that is not a constant number", line)
end

function A:compare_eq(e, st, depth, line)
	local a = self:eval(e[2], st, depth)
	local b = self:eval(e[3], st, depth)
	for _, pair in ipairs({ { a, b }, { b, a } }) do
		local x, y = pair[1], pair[2]
		local ys, yset = const_list(y)
		for t in pairs(x.t) do
			local kind = t:sub(1, 2)
			if kind == "v\1" then
				if ys and (function() for k in pairs(yset) do if k ~= "" and k ~= NIL then return false end end return true end)() then
					self:event(t, "empty", "", line)
				else
					self:event(t, "other", ys and ("compared with " .. show_consts(ys)) or "compared with a value that is not a constant", line)
				end
			elseif t == "rel" then
				if ys and (function() for k in pairs(yset) do if k ~= NIL and k ~= false then return false end end return true end)() then
					self:event(t, "test", "", line)
				else
					self:event(t, "other", "compared with something other than nil", line)
				end
			elseif t == "area" then
				self:area_cmp(t, ys, line)
			else
				self:event(t, "other", "compared", line)
			end
		end
	end
	if is_known(a) and is_known(b) and next(a.t) == nil and next(b.t) == nil then
		local res_t, res_f = false, false
		for ka in pairs(a.c) do for kb in pairs(b.c) do
			if ka == kb then res_t = true else res_f = true end
		end end
		if e[1] == "ne" then res_t, res_f = res_f, res_t end
		if res_t and not res_f then return const(true) end
		if res_f and not res_t then return const(false) end
	end
	return top()
end

function A:compare_order(e, st, depth, line)
	local a = self:eval(e[2], st, depth)
	local b = self:eval(e[3], st, depth)
	for _, pair in ipairs({ { a, b }, { b, a } }) do
		local x, y = pair[1], pair[2]
		local ys = const_list(y)
		for t in pairs(x.t) do
			if t == "area" then self:area_cmp(t, ys, line)
			else self:event(t, "other", "compared with <, >", line) end
		end
	end
	local oka, x = single(a)
	local okb, y = single(b)
	if oka and okb and next(a.t) == nil and next(b.t) == nil and type(x) == type(y)
		and (type(x) == "number" or type(x) == "string") then
		local op = e[1]
		local r
		if op == "lt" then r = x < y elseif op == "le" then r = x <= y elseif op == "gt" then r = x > y else r = x >= y end
		return const(r)
	end
	return top()
end

-- ---- calls
function A:eval_multi(e, st, depth)
	local line = e.line
	if e.tag == "Dots" then return { st.frame.scopes[1]["..."] or top() } end
	if e.tag == "Invoke" then
		local obj = self:eval(e[1], st, depth)
		local meth = e[2][1]
		local args = {}
		for i = 3, #e do
			if i == #e and (e[i].tag == "Call" or e[i].tag == "Invoke" or e[i].tag == "Dots") then
				for _, v in ipairs(self:eval_multi(e[i], st, depth)) do args[#args + 1] = v end
			else
				args[#args + 1] = self:eval(e[i], st, depth)
			end
		end
		if obj.tab or obj.lt then
			local f = self:index(obj, const(meth), st, line)
			table.insert(args, 1, obj)
			return self:call(f, args, st, depth, line)
		end
		-- a string method
		table.insert(args, 1, obj)
		return self:call_builtin("string." .. meth, args, st, line)
	end
	local f = self:eval(e[1], st, depth)
	local argx = {}
	for i = 2, #e do argx[#argx + 1] = e[i] end
	local args = self:eval_list(argx, st, depth, nil)
	return self:call(f, args, st, depth, line)
end

function A:call(f, args, st, depth, line)
	if not f.fn then
		for _, a in ipairs(args) do self:use(a, "passed to a value memo analysis could not resolve as a function", line) end
		self:note("unanalysed-call", "a call of a value that is not a known function", line)
		return { top() }
	end
	local kind = f.fn.kind
	if kind == "api" then return self:call_api(f.fn.name, args, st, line) end
	if kind == "builtin" then return self:call_builtin(f.fn.name, args, st, line) end
	if kind == "lua" then return self:call_lua(f, args, st, line, depth + 1) end
	for _, a in ipairs(args) do self:use(a, "passed to a C function", line) end
	self:note("unanalysed-call", "a C function that is neither tilemaker's nor the standard library's", line)
	return { top() }
end

function A:call_api(name, args, st, line)
	local function a(i) return args[i] or NILV end
	if API_LUA_ONLY[name] then
		self:note("lua-only", name, line)
		for _, x in ipairs(args) do self:use(x, "passed to " .. name .. "()", line) end
		st.luaonly = true
		return { top(), top() }
	end
	if name == "Find" then
		local k = a(1)
		self:use(k, "used as a tag key in Find()", line)
		local keys = strings(k)
		if not keys then
			self:note("dynamic-key", "Find", line)
			return { top({ ["v\1*"] = true }, true) }
		end
		local t = {}
		for _, key in ipairs(keys) do t["v\1" .. key] = true end
		return { top(t, true) }
	elseif name == "Holds" then
		self:use(a(1), "used as a tag key in Holds()", line)
		return { top() }
	elseif name == "IsClosed" then
		return { top() }
	elseif name == "MercatorArea" then
		return { top({ area = true }, true) }
	elseif name == "NextRelation" then
		self:note("next-relation", "", line)
		return { top({ rel = true }), top() }
	elseif name == "FindInRelation" then
		self:use(a(1), "used as a tag key in FindInRelation()", line)
		local ks = strings(a(1)) or { "*" }
		for _, k in ipairs(ks) do self.relreads[k] = true end
		return { top(nil, true) }
	elseif name == "Attribute" or name == "AttributeNumeric" or name == "AttributeInteger" or name == "AttributeBoolean" then
		local key, val = a(1), a(2)
		self:use(key, "used as an attribute name in " .. name .. "()", line)
		for i = 3, #args do self:use(args[i], "passed as minzoom to " .. name .. "()", line) end
		local attrs = strings(key)
		-- what feeds each attribute (a value tag must be the only source of the attributes it is copied to)
		for _, at in ipairs(attrs or { "*" }) do
			local src
			local n = 0
			for t in pairs(val.t) do
				local k2 = t:sub(1, 2)
				if (k2 == "v\1" and name == "Attribute") or (k2 == "l\1" and (name == "AttributeNumeric" or name == "AttributeInteger")) then
					src = t; n = n + 1
				end
			end
			if n == 1 and not is_known(val) then self.attr_sources[at] = self.attr_sources[at] or {}; self.attr_sources[at][src] = true
			else self.attr_sources[at] = self.attr_sources[at] or {}; self.attr_sources[at]["other\1" .. tostring(line)] = true end
		end
		for t in pairs(val.t) do
			local k2 = t:sub(1, 2)
			if k2 == "v\1" and name == "Attribute" and attrs then
				for _, at in ipairs(attrs) do self:event(t, "copy", at, line) end
			elseif k2 == "l\1" and (name == "AttributeNumeric" or name == "AttributeInteger") and attrs then
				for _, at in ipairs(attrs) do self:event(t, "copylen", at, line) end
			else
				self:event(t, "other", "passed to " .. name .. "()" .. (attrs and "" or " with an attribute name that is not a constant"), line)
			end
		end
		return {}
	elseif API_REPLAYED[name] then
		for _, x in ipairs(args) do self:use(x, "passed to " .. name .. "()", line) end
		return {}
	end
	-- an API function memo analysis doesn't know: like a function it can't follow
	self:note("unanalysed-call", name .. "()", line)
	for _, x in ipairs(args) do self:use(x, "passed to " .. name .. "()", line) end
	return { top() }
end

function A:call_builtin(name, args, st, line)
	if name == "string.len" and args[1] then return { self:length_of(args[1], line) } end
	for _, x in ipairs(args) do
		if x.tab and not TABLE_MUTATORS[name] then
			if x.tab == _G then self:note("unsupported", "_G passed to " .. name, line)
			elseif not self:table_has_meta(x.tab, line) then self:table_read(x.tab, st, line) end
		end
	end
	if TABLE_MUTATORS[name] and args[1] then
		local t = args[1]
		if t.tab then
			push(self.mutations, id_of(t.tab), { self.entry, line, st.luaonly })
			for i = 2, #args do self:use(args[i], "stored in a table that outlives the object", line) end
			return { top() }
		elseif t.lt then
			for i = 2, #args do t.lt.taints = set_union(t.lt.taints, args[i].t) end
			return { top() }
		end
	end
	if IMPURE_BUILTINS[name] or has_prefix(name, "os.") or has_prefix(name, "io.") or has_prefix(name, "debug.")
		or has_prefix(name, "coroutine.") or has_prefix(name, "package.") then
		self:note("side-effect", name, line)
	end
	for _, x in ipairs(args) do self:use(x, "passed to " .. name .. "()", line) end
	-- fold constants by running the (pure, C) builtin on them
	if is_pure_builtin(name) and name ~= "ipairs" and name ~= "pairs" and name ~= "next" and name ~= "error" and name ~= "assert" then
		local cargs, all = {}, true
		for i, x in ipairs(args) do
			local ok, v = single(x)
			if not ok or next(x.t) ~= nil then all = false; break end
			cargs[i] = v
		end
		local f = self.builtin_by_name[name]
		if all and f then
			local r = { pcall(f, unpack(cargs, 1, #args)) }
			if r[1] then
				local out = {}
				for i = 2, #r do
					local ty = type(r[i])
					if ty == "string" or ty == "number" or ty == "boolean" or ty == "nil" then out[#out + 1] = const(r[i])
					else out[#out + 1] = top() end
				end
				if #out == 0 then out[1] = NILV end
				return out
			end
		end
	end
	if name == "tonumber" or has_prefix(name, "math.") then return { top() } end
	local nn = { tostring = true, type = true, ["string.format"] = true, ["string.lower"] = true,
		["string.upper"] = true, ["string.sub"] = true, ["string.rep"] = true, ["string.gsub"] = true }
	return { top(nil, nn[name] or false) }
end

-- ---------------------------------------------------------------- conclusions

local function sorted_keys(t)
	local ks = {}
	for k in pairs(t) do ks[#ks + 1] = k end
	table.sort(ks, function(a, b) return tostring(a) < tostring(b) end)
	return ks
end
local function events_of(ev, taint, kind)
	local out = {}
	for _, e in pairs(ev[taint] or EMPTY) do if kind == nil or e[1] == kind then out[#out + 1] = e end end
	table.sort(out, function(x, y)
		local lx, ly = tonumber(x[3]) or 0, tonumber(y[3]) or 0
		if lx ~= ly then return lx < ly end
		return tostring(x[2]) < tostring(y[2])
	end)
	return out
end

function A:conclude(per_kind)
	local report = { state = {}, kinds = {} }
	-- State carried from one object to the next
	local function unprotected_in_memo(list)
		for _, r in ipairs(list or EMPTY) do
			if r[1] == "way_function" or r[1] == "node_function" then if not r[3] then return true end end
		end
		return false
	end
	for pn, reads in pairs(self.unsafe_reads) do
		local writes = self.writes[pn]
		if writes and (unprotected_in_memo(reads) or unprotected_in_memo(writes)) then
			report.state[#report.state + 1] = { what = self.pname_desc[pn] or pn,
				read = reads[1][1] .. " line " .. tostring(reads[1][2]),
				written = writes[1][1] .. " line " .. tostring(writes[1][2]) }
		end
	end
	for tid, muts in pairs(self.mutations) do
		local reads = self.table_reads[tid]
		if unprotected_in_memo(reads) or unprotected_in_memo(muts) then
			report.state[#report.state + 1] = { what = "a table", written = muts[1][1] .. " line " .. tostring(muts[1][2]),
				read = reads and (reads[1][1] .. " line " .. tostring(reads[1][2])) or "-" }
		end
	end
	table.sort(report.state, function(a, b) return a.what < b.what end)

	for _, entry in ipairs(MEMO_ENTRIES) do
		local kind = entry[2]
		local data = per_kind[kind]
		if data then
			local ev, notes = data.ev, data.notes
			local r = { reasons = {} }
			report.kinds[kind] = r
			local function reason(s) r.reasons[#r.reasons + 1] = s end

			if #report.state > 0 then
				local s = report.state[1]
				reason("state is carried from one object to the next: " .. s.what .. " (written at " .. s.written .. ", read at " .. s.read .. ")")
			end
			for _, n in pairs(notes["side-effect"] or EMPTY) do reason("it calls " .. n[1] .. " (line " .. tostring(n[2]) .. "), which a replay would skip or can't repeat") break end
			for _, k in ipairs({ "unsupported", "unanalysed-call" }) do
				for _, n in pairs(notes[k] or EMPTY) do
					reason("memo analysis can't follow it: " .. tostring(n[1]) .. (n[2] and (" (line " .. tostring(n[2]) .. ")") or ""))
					break
				end
			end
			r.enabled = #r.reasons == 0

			-- value tags
			local values, lengths, rejected, unhelpful = {}, {}, {}, {}
			local dyn_other = events_of(ev, "v\1*", "other")
			local keys = {}
			for t in pairs(ev) do if t:sub(1, 2) == "v\1" and t ~= "v\1*" then keys[#keys + 1] = t:sub(3) end end
			table.sort(keys)
			for _, k in ipairs(keys) do
				local other = events_of(ev, "v\1" .. k, "other")
				for _, x in ipairs(events_of(ev, "l\1" .. k, "other")) do other[#other + 1] = x end
				local copies, lcopies = {}, {}
				for _, x in ipairs(events_of(ev, "v\1" .. k, "copy")) do copies[x[2]] = true end
				for _, x in ipairs(events_of(ev, "l\1" .. k, "copylen")) do lcopies[x[2]] = true end
				if #other > 0 then
					if next(copies) then rejected[k] = other[1] end
				elseif #dyn_other > 0 then
					if next(copies) then rejected[k] = { "other", "maybe the value of a Find() whose key memo analysis could not work out", dyn_other[1][3] } end
				elseif next(copies) or next(lcopies) then
					-- an attribute that is also set from something else: tilemaker would send those objects
					-- through Lua (osm_lua_processing.cpp, memoStringAttribute)
					local shared
					for at in pairs(copies) do
						for src in pairs(self.attr_sources_of[kind][at] or EMPTY) do
							if src ~= "v\1" .. k then shared = at end
						end
					end
					for at in pairs(lcopies) do
						for src in pairs(self.attr_sources_of[kind][at] or EMPTY) do
							if src ~= "l\1" .. k then shared = at end
						end
					end
					if shared then
						unhelpful[k] = shared
					else
						values[k] = sorted_keys(copies)
						for at in pairs(lcopies) do lengths[at] = k end
					end
				end
			end
			r.values, r.lengths, r.rejected, r.unhelpful = values, lengths, rejected, unhelpful

			-- area
			local area_other = events_of(ev, "area", "other")
			local thr = {}
			for _, x in ipairs(events_of(ev, "area", "cmp")) do thr[x[2]] = true end
			r.area = {}
			if #area_other == 0 then
				for v in pairs(thr) do r.area[#r.area + 1] = v end
				table.sort(r.area)
			else
				r.area_rejected = area_other[1]
			end
			-- relations
			local rel_other = events_of(ev, "rel", "other")
			r.relations = #rel_other == 0 and (next(notes["next-relation"] or EMPTY) ~= nil or next(data.relreads) ~= nil)
			if #rel_other > 0 then r.relations_rejected = rel_other[1] end
			local lua_only = {}
			for _, n in pairs(notes["lua-only"] or EMPTY) do lua_only[n[1]] = true end
			r.lua_only = sorted_keys(lua_only)
		end
	end
	return report
end

-- ---------------------------------------------------------------- entry point

local function analyze(opts)
	local self = new_analyzer(opts)
	self.builtin_by_name = {}
	for f, name in pairs(self.builtins) do self.builtin_by_name[name] = f end
	self.attr_sources_of = {}
	local per_kind = {}
	for _, entry in ipairs(MEMO_ENTRIES) do
		self:reset_events()
		self.attr_sources = {}
		if self:run_entry(entry[1]) then
			per_kind[entry[2]] = { ev = self.ev, notes = self.notes, relreads = self.relreads }
		end
		self.attr_sources_of[entry[2]] = self.attr_sources
	end
	for _, name in ipairs(OTHER_ENTRIES) do
		self:reset_events()
		self.attr_sources = {}
		self:run_entry(name)
	end
	return self:conclude(per_kind)
end

return analyze
