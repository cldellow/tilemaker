-- Development runner for tilemaker's memo analysis (src/memo_analysis): runs it on a profile the way
-- tilemaker does, with stand-ins for tilemaker's functions, and prints the report as JSON.
-- Usage: lua run_analysis.lua path/to/src/memo_analysis profile.lua
local dir, profile = arg[1], arg[2]
package.path = dir .. "/?.lua;" .. package.path
package.preload["luacheck.utils"] = function() return dofile(dir .. "/utils.lua") end
package.preload["luacheck.lexer"] = function() return dofile(dir .. "/lexer.lua") end
package.preload["luacheck.parser"] = function() return dofile(dir .. "/parser.lua") end
package.preload["luacheck.decoder"] = function() return dofile(dir .. "/decoder.lua") end
local analyze = dofile(dir .. "/analyze.lua")

-- 1. the standard library, before the profile can change it (C functions only)
local builtins = {}
for _, lib in ipairs({ "string", "math", "table", "os", "io", "utf8", "coroutine", "debug", "package" }) do
	if type(_G[lib]) == "table" then
		for k, f in pairs(_G[lib]) do
			if type(f) == "function" and debug.getinfo(f, "S").what == "C" then builtins[f] = lib .. "." .. k end
		end
	end
end
for k, f in pairs(_G) do
	if type(f) == "function" and debug.getinfo(f, "S").what == "C" and not builtins[f] then builtins[f] = k end
end

-- 2. the profile, from its directory (for require)
local pdir = profile:match("^(.*)/[^/]*$") or "."
package.path = pdir .. "/?.lua;" .. package.path
dofile(profile)

-- 3. tilemaker's functions, registered after the profile is loaded
local API = { "Find", "Holds", "Layer", "LayerAsCentroid", "Attribute", "AttributeNumeric", "AttributeBoolean",
	"AttributeInteger", "MinZoom", "ZOrder", "Score", "ModifyId", "MinZoomByArea", "IsClosed", "MercatorArea",
	"NextRelation", "FindInRelation", "Id", "OsmType", "AllKeys", "AllTags", "HasTags", "SetTag",
	"FindIntersecting", "Intersects", "FindCovering", "CoveredBy", "IsMultiPolygon", "Area", "Length",
	"Centroid", "RestartRelations", "Accept", "AreaIntersecting", "SetData", "GetData" }
local api = {}
for _, name in ipairs(API) do
	local f = function() return nil end
	_G[name] = f
	api[f] = name
end

-- 4. init_function, then the analysis
if type(init_function) == "function" then pcall(init_function, "memo-analysis", true) end
local t0 = os.clock()
local report = analyze({ api = api, builtins = builtins })
report.seconds = os.clock() - t0

-- JSON
local function json(v, ind)
	local ty = type(v)
	if ty == "table" then
		local n = #v
		local isarr = n > 0 or next(v) == nil
		if isarr then for k in pairs(v) do if type(k) ~= "number" then isarr = false end end end
		local parts = {}
		if isarr then
			for i = 1, n do parts[i] = json(v[i]) end
			return "[" .. table.concat(parts, ",") .. "]"
		end
		local ks = {}
		for k in pairs(v) do ks[#ks + 1] = tostring(k) end
		table.sort(ks)
		for _, k in ipairs(ks) do
			local val = v[k]; if val == nil then val = v[tonumber(k)] end
			parts[#parts + 1] = string.format("%q", k) .. ":" .. json(val)
		end
		return "{" .. table.concat(parts, ",") .. "}"
	elseif ty == "string" then
		return '"' .. v:gsub('[%c"\\]', function(c) return string.format("\\u%04x", c:byte()) end) .. '"'
	elseif ty == "number" then
		if v ~= v or v == math.huge or v == -math.huge then return "null" end
		return string.format("%.17g", v)
	elseif ty == "boolean" then return tostring(v)
	end
	return "null"
end
print(json(report))
