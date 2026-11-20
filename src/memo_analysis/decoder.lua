-- A source object for luacheck's lexer: bytes as they are (strings keep their UTF-8 bytes, as in Lua).
local decoder = {}
local Source = {}
Source.__index = Source
function decoder.decode(s) return setmetatable({ s = s }, Source) end
function Source:get_length() return #self.s end
function Source:get_codepoint(i) return self.s:byte(i) end
function Source:get_substring(i, j) return self.s:sub(i, j) end
function Source:get_printable_substring(i, j) return self.s:sub(i, j) end
function Source:find(pattern, init) return self.s:find(pattern, init) end
return decoder
