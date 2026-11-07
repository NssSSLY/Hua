-- 按行读取
local path = os.tmpname()
local out = assert(io.open(path, "wb"))
out:write(string.rep("the quick brown fox jumps over\n", 20000))
out:close()
local total = 0
for r = 1, 3 do
  for line in io.lines(path) do total = total + #line + 1 end
end
os.remove(path)
print(total)
