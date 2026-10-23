-- 按行与按空白切分（标准等价：gmatch）
local text = "alpha  beta\tgamma\ndelta   epsilon\n\nzeta eta\ttheta\n"
local total = 0
for i = 0, 19999 do
  for line in string.gmatch(text, "([^\n]*)\n") do
    if #line > 0 then
      total = total + 1
      for word in string.gmatch(line, "%S+") do total = total + #word end
    end
  end
end
print(total)
