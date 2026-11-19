-- Test stand-in for hikeratlas' keydb module (a set of ids shared by tilemaker's
-- threads through files in /tmp). Here a Lua table per thread: it still carries state
-- from one object to the next, which is what makes places.lua unsafe to memoize.
local keydb = {}
local seen = {}
function keydb.init() end
function keydb.set(id) seen[id] = true end
function keydb.test(id) return seen[id] == true end
return keydb
