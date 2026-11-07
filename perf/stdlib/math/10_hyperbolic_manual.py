import math
def sinh(x):return (math.exp(x)-math.exp(-x))/2.0
def cosh(x):return (math.exp(x)+math.exp(-x))/2.0
def tanh(x):
 e=math.exp(2*x);return (e-1)/(e+1)
def asinh(x):return math.log(x+math.sqrt(x*x+1))
def acosh(x):return math.log(x+math.sqrt(x-1)*math.sqrt(x+1))
def atanh(x):return 0.5*math.log((1+x)/(1-x))
total=0.0
for i in range(10000):
 x=(i%100+1)*0.0073
 total+=sinh(x)+cosh(x)+tanh(x)+asinh(x)+acosh(x+1)+atanh(x/2)
print(int(total))
