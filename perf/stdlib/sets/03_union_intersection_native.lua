-- 集合并/交/差/子集（table键集合）
local u, n, d, subset = 0, 0, 0, 0
local function size(t) local c = 0; for _ in pairs(t) do c = c + 1 end; return c end
for r = 1, 20 do
  local a, b = {}, {}
  for i = 0, 4999 do a[i] = true; b[i + 2500] = true end
  local un, inter, diff = {}, {}, {}
  for k in pairs(a) do un[k] = true end
  for k in pairs(b) do un[k] = true end
  for k in pairs(a) do if b[k] then inter[k] = true else diff[k] = true end end
  local sub = true
  for k in pairs(inter) do if not a[k] then sub = false; break end end
  u = u + size(un); n = n + size(inter); d = d + size(diff)
  if sub then subset = subset + 1 end
end
print(u .. " " .. n .. " " .. d .. " " .. subset)
