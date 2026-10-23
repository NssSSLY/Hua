-- 字符串键热查找
local m, keys = {}, {}
for i = 0, 1999 do local k = "key" .. i; m[k] = i; keys[#keys + 1] = k end
local total, misses = 0, 0
for r = 0, 249 do
  for _, k in ipairs(keys) do local v = m[k]; if v ~= nil then total = total + v end end
  if m["missing" .. r] == nil then misses = misses + 1 end
end
print(total .. " " .. misses)
