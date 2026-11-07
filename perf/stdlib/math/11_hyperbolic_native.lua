local function sinh(x)return (math.exp(x)-math.exp(-x))/2.0 end
local function cosh(x)return (math.exp(x)+math.exp(-x))/2.0 end
local function tanh(x)local e=math.exp(2*x);return (e-1)/(e+1) end
local function asinh(x)return math.log(x+math.sqrt(x*x+1)) end
local function acosh(x)return math.log(x+math.sqrt(x-1)*math.sqrt(x+1)) end
local function atanh(x)return 0.5*math.log((1+x)/(1-x)) end
local total=0.0
for i=0,9999 do
 local x=(i%100+1)*0.0073
 total=total+sinh(x)+cosh(x)+tanh(x)+asinh(x)+acosh(x+1)+atanh(x/2)
end
print(math.floor(total))
