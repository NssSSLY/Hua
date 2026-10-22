'''Private reverse-edge publication: exact sets, self edges and snapshot isolation.'''
from pathlib import Path
import argparse,json,re,time
from process_guard import run

def main():
 p=argparse.ArgumentParser();p.add_argument('exe',type=Path);p.add_argument('root',type=Path);p.add_argument('output',type=Path);a=p.parse_args();R=a.root.resolve();W=a.output.resolve()/('dependencies-'+str(time.time_ns()));W.mkdir(parents=True,exist_ok=False)
 text=(R/'selfhost/capabilities.hua').read_text(encoding='utf-8');m=re.search(r'(?ms)^fn record_dependencies\(.*?(?=^(?:pub )?fn |\Z)',text);assert m
 code=r'''import std.json as json
struct State{dependents map[string]map[string]bool}
struct Environment{function string
callees map[string]bool}
HELPER
fn main(){
let prior=map[string]bool{"old":true}
let before=unwrap(json.encode(prior))
var s=State{dependents:map[string]map[string]bool{"target":prior}}
let e=Environment{function:"caller",callees:map[string]bool{"target":true,"caller":true}}
let input=unwrap(json.encode(e))
record_dependencies(s,e)
let target=unwrap(s.dependents["target"])
print(len(target)==2,unwrap_or(target["old"],false) && unwrap_or(target["caller"],false),unwrap_or(unwrap(s.dependents["caller"])["caller"],false))
let completed=unwrap(json.encode(s))
record_dependencies(s,e)
print(completed==unwrap(json.encode(s)),before==unwrap(json.encode(prior)),input==unwrap(json.encode(e)))
record_dependencies(s,Environment{function:"second",callees:map[string]bool{"target":true}})
let after=unwrap(s.dependents["target"])
let second=unwrap(json.encode(s))
record_dependencies(s,Environment{function:"empty",callees:map[string]bool{}})
print(len(after)==3 && unwrap_or(after["second"],false),len(unwrap(s.dependents["caller"]))==1,second==unwrap(json.encode(s)) && len(target)==2 && !has(target,"second"))
}
'''.replace('HELPER',m.group())
 src=W/'main.hua';src.write_text(code,encoding='utf-8',newline='\n');artifact=src.with_suffix('.huab');expected=['true true true']*3;rows=[]
 for mode in ['interpret','run','build','huab']:
  if mode=='huab':src.rename(src.with_suffix('.hidden'))
  args=[str(a.exe.resolve()),'run' if mode=='huab' else mode,str(artifact if mode=='huab' else src)]
  if mode=='build':args+=['-o',str(artifact)]
  r=run(args,cwd=R,capture_output=True,timeout=30);(W/(mode+'.stdout')).write_bytes(r.stdout);(W/(mode+'.stderr')).write_bytes(r.stderr)
  assert r.returncode==0,(mode,r.stderr.decode('utf-8'))
  if mode!='build':assert r.stdout.decode('utf-8').splitlines()==expected,(mode,r.stdout.decode('utf-8'))
  rows.append({'mode':mode,'status':'passed'})
 result={'status':'passed','assertions_per_path':9,'paths':3,'rows':rows};(W/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
if __name__=='__main__':main()
