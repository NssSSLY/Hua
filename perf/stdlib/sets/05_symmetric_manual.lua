local a,b = {},{}
for i=0,4999 do a[i]=false; b[i+2500]=true end
local total=0
for r=1,20 do
  local result={}
  for k in pairs(a) do if b[k]==nil then result[k]=true end end
  for k in pairs(b) do if a[k]==nil then result[k]=true end end
  for _ in pairs(result) do total=total+1 end
end
print(total)
