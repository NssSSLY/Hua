-- Unicode大小写（Lua没有Unicode表，用同一小映射表）
local m = {[201]=233,[214]=246,[913]=945,[914]=946,[915]=947,[1055]=1087,[1056]=1088,[1048]=1080,[1042]=1074,[1045]=1077,[1058]=1090}
local text = string.rep("HÉLLO WÖRLD ΑΒΓ ПРИВЕТ ", 200)
local function lower(s)
  local out = {}
  for _, c in utf8.codes(s) do
    if c >= 65 and c <= 90 then c = c + 32 elseif m[c] then c = m[c] end
    out[#out + 1] = utf8.char(c)
  end
  return table.concat(out)
end
local total = 0
for r = 1, 10 do
  local a = lower(text)
  total = total + #a
  if a == lower(text) then total = total + 1 end
end
print(total)
