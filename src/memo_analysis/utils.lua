-- The parts of luacheck's utils module that its lexer and parser use (MIT, see LICENSE.luacheck).
local utils = {}

local unpack = table.unpack or unpack
local function pack(...) return { n = select("#", ...), ... } end

function utils.array_to_set(array)
   local set = {}
   for index, value in ipairs(array) do
      set[value] = index
   end
   return set
end

local class_metatable = {}

function class_metatable.__call(class, ...)
   local obj = setmetatable({}, class)
   if class.__init then
      local init_returns = pack(class.__init(obj, ...))
      if init_returns.n > 0 then
         return unpack(init_returns, 1, init_returns.n)
      end
   end
   return obj
end

function utils.class()
   local class = setmetatable({}, class_metatable)
   class.__index = class
   return class
end

utils.Stack = utils.class()

function utils.Stack:__init()
   self.size = 0
end

function utils.Stack:push(value)
   self.size = self.size + 1
   self[self.size] = value
   self.top = value
end

function utils.Stack:pop()
   local value = self[self.size]
   self[self.size] = nil
   self.size = self.size - 1
   self.top = self[self.size]
   return value
end

return utils
