-- 06_map_merge：逐项写法的集合相等与合并（与 Hua 版本相同算法）
local function size(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end
local function same_flags(a, b)
  if size(a) ~= size(b) then return false end
  for name, flag in pairs(a) do if b[name] == nil or flag ~= b[name] then return false end end
  return true
end
local function merged(a, b)
  local out = {}
  for k, v in pairs(a) do out[k] = v end
  for origin, readonly in pairs(b) do local old = out[origin]; out[origin] = readonly or (old ~= nil and old or false) end
  return out
end
local a, b = {}, {}
for i = 0, 399 do a["m|#@f" .. tostring(i)] = false; b["m|#@f" .. tostring(i)] = (i % 3 == 0) end
local same, total = 0, 0
for round = 0, 299 do
  if same_flags(a, a) then same = same + 1 end
  if same_flags(a, b) then same = same + 1 end
  total = total + size(merged(a, b))
end
print(same .. " " .. total)
