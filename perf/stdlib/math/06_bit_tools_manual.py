MASK=(1<<64)-1
def popcount(x):
    n=0
    for i in range(64): n+=x&1; x>>=1
    return n
def leading_zeros(x):
    for i in range(64):
        if (x>>(63-i))&1: return i
    return 64
def trailing_zeros(x):
    for i in range(64):
        if (x>>i)&1: return i
    return 64
def rotate_left(x,n):
    result=0
    for i in range(64):
        if (x>>i)&1: result|=1<<((i+n)%64)
    return result-(1<<64) if result>>63 else result
def rotate_right(x,n): return rotate_left(x,(64-n)%64)
total=0
for i in range(20000):
    x=i*7919
    total+=popcount(x)+leading_zeros(x)+trailing_zeros(x)
    total+=(rotate_left(x,13)&65535)+(rotate_right(x,13)&65535)
print(total)
