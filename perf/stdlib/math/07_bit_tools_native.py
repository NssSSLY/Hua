MASK=(1<<64)-1
def popcount(x): return (x&MASK).bit_count()
def leading_zeros(x): return 64-(x&MASK).bit_length()
def trailing_zeros(x):
    bits=x&MASK
    return (bits&-bits).bit_length()-1 if bits else 64
def rotate_left(x,n): return ((x<<n)|(x>>(64-n)))&MASK
def rotate_right(x,n): return ((x>>n)|(x<<(64-n)))&MASK
total=0
for i in range(20000):
    x=i*7919
    total+=popcount(x)+leading_zeros(x)+trailing_zeros(x)
    total+=(rotate_left(x,13)&65535)+(rotate_right(x,13)&65535)
print(total)
