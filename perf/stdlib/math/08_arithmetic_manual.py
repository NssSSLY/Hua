MIN=-(1<<63);MAX=(1<<63)-1
MASK=(1<<64)-1
def checked_add(a,b):
    if (b>0 and a>MAX-b) or (b<0 and a<MIN-b): return None
    return a+b
def wrapping_add(a,b):
    if b>0 and a>MAX-b:return MIN+(a-(MAX-b)-1)
    if b<0 and a<MIN-b:return MAX+(a-(MIN-b)+1)
    return a+b
def saturating_add(a,b):
    if b>0 and a>MAX-b:return MAX
    if b<0 and a<MIN-b:return MIN
    return a+b
total=errors=0
for i in range(60000):
    x=MAX-i%8;c=checked_add(x,3)
    if c is None:errors+=1
    total+=(c if c is not None else 0)&65535
    total+=wrapping_add(x,3)&65535
    total+=saturating_add(x,3)&65535
print(total,errors)
