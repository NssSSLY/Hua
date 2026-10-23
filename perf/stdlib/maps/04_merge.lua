-- 合并两个map
local added, size = 0, 0
for r = 1, 20 do
  local dst, src, n = {}, {}, 0
  for i = 0, 2999 do dst[i * 2] = i; n = n + 1; src[i * 3] = i end
  for k, v in pairs(src) do if dst[k] == nil then added = added + 1; n = n + 1 end; dst[k] = v end
  size = size + n
end
print(added .. " " .. size)
