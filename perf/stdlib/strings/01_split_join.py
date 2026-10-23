# split + join
line = "alpha,beta,gamma,delta,epsilon,zeta,eta,theta,iota,kappa"
total = 0
for i in range(100000):
    parts = line.split(",")
    total += len(parts)
    joined = ";".join(parts)
    total += len(joined)
print(total)
