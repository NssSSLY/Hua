local function popcount(x)
  local n=0
  for i=0,63 do n=n+(x&1); x=x>>1 end
  return n
end
local function leading_zeros(x)
  for i=0,63 do if ((x>>(63-i))&1)~=0 then return i end end
  return 64
end
local function trailing_zeros(x)
  for i=0,63 do if ((x>>i)&1)~=0 then return i end end
  return 64
end
local function rotate_left(x,n)
  local result=0
  for i=0,63 do if ((x>>i)&1)~=0 then result=result|(1<<((i+n)%64)) end end
  return result
end
local function rotate_right(x,n) return rotate_left(x,(64-n)%64) end
local total=0
for i=0,19999 do
  local x=i*7919
  total=total+popcount(x)+leading_zeros(x)+trailing_zeros(x)
  total=total+(rotate_left(x,13)&65535)+(rotate_right(x,13)&65535)
end
print(total)
