-- 参数解析（手写循环）
local argv = {"input.txt", "--verbose", "--name=hua", "-n", "second", "--name", "third", "output.txt", "--", "--literal"}
local total = 0
for r = 1, 4000 do
  local positional, names, verbose, rest, i = 0, 0, false, false, 1
  while i <= #argv do
    local t = argv[i]
    if rest then positional = positional + 1
    elseif t == "--" then rest = true
    elseif t == "--verbose" then verbose = true
    elseif t:sub(1, 7) == "--name=" then names = names + 1
    elseif t == "--name" or t == "-n" then names = names + 1; i = i + 1
    else positional = positional + 1 end
    i = i + 1
  end
  total = total + positional + names + (verbose and 1 or 0)
end
print(total)
