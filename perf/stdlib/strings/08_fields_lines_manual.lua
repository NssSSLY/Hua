-- 手写按空白切分与按行切分
local text = "alpha  beta\tgamma\ndelta   epsilon\n\nzeta eta\ttheta\n"
local total = 0
for i = 0, 19999 do
  for line in string.gmatch(text, "([^\n]*)\n") do
    if #line > 0 then
      total = total + 1
      local spaced = string.gsub(line, "\t", " ")
      for word in string.gmatch(spaced, "([^ ]*)") do
        if #word > 0 then total = total + #word end
      end
    end
  end
end
print(total)
