import math
total=0.0
for i in range(10000):
    x=(i%100+1)*0.0073
    total+=math.sinh(x)+math.cosh(x)+math.tanh(x)+math.asinh(x)+math.acosh(x+1.0)+math.atanh(x/2.0)
print(int(total))
