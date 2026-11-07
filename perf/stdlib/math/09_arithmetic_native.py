MIN=-(1<<63);MAX=(1<<63)-1;MASK=(1<<64)-1
def checked_add(a,b):
    n=a+b;return n if MIN<=n<=MAX else None
def wrapping_add(a,b):
    n=(a+b)&MASK;return n-(1<<64) if n>>63 else n
def saturating_add(a,b):return min(MAX,max(MIN,a+b))
total=errors=0
for i in range(60000):
    x=MAX-i%8;c=checked_add(x,3)
    if c is None:errors+=1
    total+=((c or 0)&65535)+(wrapping_add(x,3)&65535)+(saturating_add(x,3)&65535)
print(total,errors)
