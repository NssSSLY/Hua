-- 命名模板
local t = "user={name} id={id} city={city} status={status}"
local m = {name = "hua", city = "上海", status = "active", id = ""}
local total = 0
for i = 0, 29999 do
  m.id = tostring(i)
  total = total + #string.gsub(t, "{(%w+)}", m)
end
print(total)
