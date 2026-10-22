-- 04_struct_pass：记录按值传入/返回；显式复制外层与两个内层记录（切片/map 共享，与 Hua 相同）
local function copy_inner(x) return {name = x.name, tags = x.tags, count = x.count} end
local function copy_outer(o) return {label = o.label, left = copy_inner(o.left), right = copy_inner(o.right), flags = o.flags, total = o.total} end
local function bump(o, k) local out = copy_outer(o); out.total = o.total + k; out.left.count = o.left.count + 1; return out end
local function read(o) return o.total + o.left.count + o.right.count end
local o = {label = "outer", left = {name = "l", tags = {"a", "b", "c"}, count = 0}, right = {name = "r", tags = {"x"}, count = 5}, flags = {p = true}, total = 0}
local acc = 0
for i = 0, 199999 do
  o = bump(copy_outer(o), i % 7)
  acc = (acc + read(copy_outer(o))) % 1000003
end
print(acc .. " " .. o.left.count)
