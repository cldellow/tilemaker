-- Test stand-in for hikeratlas' qrank module (a SQLite lookup of Wikidata's QRank).
-- Pure Lua and deterministic, so the slices run without luasql or qrank.db:
-- lower Q-numbers (older, usually better-known items) get higher ranks.
local qrank = {}
function qrank.init() end
function qrank.close() end
-- id: a Wikidata ID, eg Q1261. Returns its rank, or 1 if it has none.
function qrank.get(id)
	if id == '' then return 1 end
	local n = tonumber(string.sub(id, 2, 100))
	if n == nil then return 1 end
	return math.floor(1e9 / (n + 100))
end
return qrank
