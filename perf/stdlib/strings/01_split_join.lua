-- split + join
local line = "alpha,beta,gamma,delta,epsilon,zeta,eta,theta,iota,kappa"
local total = 0
for i = 0, 99999 do
  local parts, n = {}, 0
  for item in string.gmatch(line, "([^,]*)") do n = n + 1; parts[n] = item end
  total = total + n
  local joined = table.concat(parts, ";")
  total = total + #joined
end
print(total)
