from pathlib import Path
import hashlib,json,subprocess,sys,time
import argparse
parser=argparse.ArgumentParser(description="Pure record value, alias, Task and source-free HUAB contracts")
parser.add_argument('hua',type=Path);parser.add_argument('root',type=Path);parser.add_argument('output',type=Path)
parser.add_argument('--legacy',type=Path);parser.add_argument('--case')
args=parser.parse_args();R=args.root.resolve();parent=args.output.resolve();parent.mkdir(parents=True,exist_ok=True)
import tempfile
W=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
new=args.hua.resolve();old=args.legacy.resolve() if args.legacy else None;rows=[];checks=0
cases=[
('nested-mutation', '''struct Id{pool int
index int}
struct Span{source Id
start int
end int
line int
column int}
fn read(id Id) int{return id.index}
fn change(id mut Id){id.index=9}
fn main(){var id=Id{pool:1,index:7}
let saved=id
change(id)
print(saved.index,id.index,read(saved))
var span=Span{source:id,start:1,end:8,line:2,column:3}
var copy=span
span.source.index=12
print(copy.source.index,span.source.index)}
''','7 9 7\n9 12\n'),
('shared-readonly-view', '''struct Point{x int}
var xs=[Point{x:1}]
fn change() int{xs[0].x=9
return 0}
fn main(){let ro=xs
print(ro[0],change())}
''','Point{x: 9} 0\n'),
('target-before-rebind', '''struct Point{x int}
var p=Point{x:1}
fn replace() int{p=Point{x:4}
return 9}
fn main(){p.x=replace()
print(p.x)}
''','4\n'),
('result-optional', '''struct Id{pool int
index int}
fn value() Result<Id>{return ok(Id{pool:1,index:3})}
fn main(){let result=value()
var a=unwrap(result)
a.index=4
print(unwrap(result).index,a.index)
let absent Id?=nil
print(is_none(absent))}
''','3 4\ntrue\n'),
('json', '''import std.json as json
struct Id{pool int
index int}
fn main(){let id=Id{pool:1,index:7}
print(unwrap(json.encode(id)))}
''','{"index":7,"pool":1}\n'),
('defer-snapshot', '''struct Point{x int}
fn set(p mut Point){p.x=9}
fn result() Point{var p=Point{x:1}
defer set(p)
return p}
fn main(){print(result().x)}
''','1\n'),
('task-pure', '''import std.task as t
struct Id{pool int
index int}
async fn work(id Id) Id{t.yield()
return id}
fn main(){let id=Id{pool:1,index:7}
let task=work(id)
print((await task).index,(await task).index)}
''','7 7\n'),
]
if args.case and args.case not in {name for name,_,_ in cases}:parser.error('unknown case: '+args.case)

def call(label,args,want=None):
 global checks
 start=time.monotonic();p=subprocess.run(list(map(str,args)),cwd=R,capture_output=True,timeout=15)
 row={'label':label,'command':list(map(str,args)),'seconds':time.monotonic()-start,'returncode':p.returncode,'stdout':p.stdout.decode('utf-8',errors='replace'),'stderr':p.stderr.decode('utf-8',errors='replace')};rows.append(row)
 (W/'commands.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 assert p.returncode==0,row
 if want is not None:assert row['stdout'].replace('\r\n','\n')==want,row
 checks+=1;return row
for name,text,want in cases:
 if args.case and name!=args.case:continue
 d=W/name;d.mkdir();src=d/'main.hua';src.write_text(text,encoding='utf-8',newline='\n');a=d/'old.huab';b=d/'new.huab'
 call(name+'-new-build',[new,'build',src,'-o',b])
 if old:
  call(name+'-old-build',[old,'build',src,'-o',a]);assert a.read_bytes()==b.read_bytes(),name+' HUAB differs';checks+=1
 for engine in ['interpret','run']:call(name+'-'+engine,[new,engine,src],want)
 src.rename(src.with_suffix('.hidden'))
 call(name+'-source-free-reader',[new,'check',b],'OK\n');call(name+'-source-free',[new,'run',b],want)
 if old:call(name+'-old-reader-execution',[old,'run',b],want)
print(json.dumps({'status':'passed','cases':1 if args.case else len(cases),'checks':checks,'commands':len(rows),'artifacts_identical':bool(old),'old_reader_execution':bool(old)}),flush=True)
(W/'result.json').write_text(json.dumps({'status':'passed','cases':1 if args.case else len(cases),'checks':checks,'commands':len(rows)},indent=2)+'\n',encoding='utf-8')
