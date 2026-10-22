"""Finite graph check for the proposed RC candidate-set collector, not a runtime test."""
from pathlib import Path
import collections,itertools,json,time
def reachable(roots,edges,allowed):
 seen=set(roots);work=list(roots)
 while work:
  u=work.pop()
  for v in edges[u]:
   if v in allowed and v not in seen:seen.add(v);work.append(v)
 return seen
def collect_subset(edges,external,candidates):
 incoming=collections.Counter(v for u in candidates for v in edges[u] if v in candidates)
 actual=collections.Counter(v for row in edges for v in row)
 # The collector itself pins each candidate once. Other candidate references
 # are subtracted; ALL references from outside the candidate set stay roots.
 roots={v for v in candidates if actual[v]+int(v in external)+1>incoming[v]+1}
 survivors=reachable(roots,edges,candidates)
 return candidates-survivors
def main():
 start=time.perf_counter();checks=0;conservative=0
 for n in range(1,4):
  nodes=set(range(n))
  for mask in range(1<<(n*n)):
   edges=[{v for v in range(n) if mask&(1<<(u*n+v))} for u in range(n)]
   for roots_mask in range(1<<n):
    external={v for v in nodes if roots_mask&(1<<v)};live=reachable(external,edges,nodes)
    for sub in range(1<<n):
     candidates={v for v in nodes if sub&(1<<v)};dead=collect_subset(edges,external,candidates)
     assert not dead&live,(edges,external,candidates,dead,live);checks+=1
     if candidates==nodes:assert dead==nodes-live;checks+=1
     conservative+=len((candidates-live)-dead)
 cases=[]
 for name,edges,external,candidates,expected in [
  ('old-held-young-cycle',[{1},{2},{1}],{0},{1,2},set()),
  ('unreachable-cross-age-cycle',[{1},{0}],set(),{1},set()),
  ('host-rooted-young-cycle',[{1},{0}],{1},{0,1},set()),
  ('unreachable-young-cycle',[{1},{0}],set(),{0,1},{0,1}),
  ('old-self-cycle-reclaimed-by-major',[{0}],set(),{0},{0})]:
  dead=collect_subset(edges,external,candidates);assert dead==expected;cases.append({'name':name,'reclaimed':sorted(dead)});checks+=1
 # Duplicated strong edges count independently; weak edges are absent.
 edges=[[1,1],[0]];assert collect_subset(edges,set(),{0,1})=={0,1};checks+=1
 for copies in range(1,8):
  edges=[[1]*copies,[0]*copies];assert not collect_subset(edges,{0},{0,1});checks+=1
 report={'schema':'hua.rc-subset.model.v1','status':'passed','checks':checks,'conservative_retention_total':conservative,'explicit_cases':cases,'seconds':time.perf_counter()-start,'boundary':'Mathematical model only. No C++ collector, barriers, frame storage, allocation failure, resource finalizers or actual speed validated.'}
 print(json.dumps(report,ensure_ascii=False))
if __name__=='__main__':main()
