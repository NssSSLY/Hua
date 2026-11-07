local MIN,MAX=math.mininteger,math.maxinteger
local function checked_add(a,b)
 if (b>0 and a>MAX-b) or (b<0 and a<MIN-b) then return nil end
 return a+b
end
local function wrapping_add(a,b)
 if b>0 and a>MAX-b then return MIN+(a-(MAX-b)-1) end
 if b<0 and a<MIN-b then return MAX+(a-(MIN-b)+1) end
 return a+b
end
local function saturating_add(a,b)
 if b>0 and a>MAX-b then return MAX end
 if b<0 and a<MIN-b then return MIN end
 return a+b
end
local total,errors=0,0
for i=0,59999 do
 local x=MAX-i%8;local c=checked_add(x,3)
 if c==nil then errors=errors+1 end
 total=total+((c or 0)&65535)+(wrapping_add(x,3)&65535)+(saturating_add(x,3)&65535)
end
print(total.." "..errors)
