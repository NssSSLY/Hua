"""A05: Hua SB1 representations, C++ Source/Parser and source-free HUAB."""
import json, shutil, struct, subprocess, sys, time
from pathlib import Path
hua, probe, oracle, repo, build = [Path(p).resolve() for p in sys.argv[1:]]
out = build/'a05';work = out/'cases';work.mkdir(parents=True,exist_ok=True)
checks=0;rows=[]
def run(args,expected=None,error=None):
 global checks
 start=time.monotonic();p=subprocess.run(list(map(str,args)),capture_output=True,encoding='utf-8',timeout=45)
 assert p.returncode==(1 if error else 0),(args,p.returncode,p.stdout,p.stderr)
 if expected is not None:assert p.stdout==expected,(args,p.stdout,expected,p.stderr)
 if error:assert '['+error+']' in p.stderr,(args,p.stderr)
 checks+=1;return p.stdout,round(time.monotonic()-start,6)
HEAD='''import ast_pool as ast
import ast_direct as direct
import std.list as list
fn main(){
 var pool=ast.create()
 let source=unwrap(ast.register_source(pool,"sample.hua","1 + 2\\n"))
 let s=unwrap(ast.span(pool,source,0,1))
 var empty []ast.NodeId=[]
 let a=unwrap(ast.add(pool,ast.Payload.Integer(1),s,empty))
 let b=unwrap(ast.add(pool,ast.Payload.Integer(2),s,empty))
'''
def case(name,text,expected='',error=None,static=False,full=False):
 global checks
 d=work/name;d.mkdir(exist_ok=True);entry=d/'main.hua'
 for n in ['ast_pool.hua','ast_direct.hua']:shutil.copy2(repo/'selfhost'/n,d/n)
 entry.write_text(text if full else HEAD+text+'\n}\n',encoding='utf-8')
 if static:
  for exe in (hua,probe):
   for cmd in ('check','interpret','run','build'):run([exe,cmd,entry],'',error)
  assert not entry.with_suffix('.huab').exists();checks+=1
  rows.append({'case':name,'phase':'static','error':error,'artifact':False});return
 run([hua,'check',entry],'OK\n');run([probe,'check',entry],'SB1 OK\n');durations={}
 for exe in (hua,probe):
  for engine in ('interpret','run'):durations[exe.stem+':'+engine]=run([exe,engine,entry],expected,error)[1]
 run([probe,'build',entry]);archive=entry.with_suffix('.huab')
 version,abi=struct.unpack_from('<II',archive.read_bytes(),8)
 assert (version,abi)==(7,1),(name,version,abi);checks+=1
 for p in d.rglob('*.hua'):p.unlink()
 assert not list(d.rglob('*.hua'));checks+=1
 run([hua,'check',archive],'OK\n');durations['HUAB']=run([hua,'run',archive],expected,error)[1]
 rows.append({'case':name,'phase':'runtime' if error else 'execute','error':error,'AST_VM_HUAB':'passed','SB1':'passed','version':version,'output':expected,'elapsed_seconds_not_benchmark':durations})
ref=work/'expression.hua';ref.write_text('1 + 2 * 3\n',encoding='utf-8')
expr,_=run([oracle,'ast',ref]);assert expr.count('node\t')==7;checks+=1
case('expression-demo',(repo/'selfhost/ast_demo.hua').read_text(encoding='utf-8'),expr+'rewrite 6 11 5\nold 2\nnew 20\n',full=True);ref.unlink()
case('recursive-values','''
 let tree=direct.binary("+",s,direct.leaf(1,s),direct.leaf(2,s))
 let rewritten=direct.replace_integer(tree,1,7)
 print(direct.visits(tree),ast.payload_text(tree.children[0].payload),ast.payload_text(rewritten.children[0].payload))
 var detached=clone(tree)
 detached.children[0].payload=ast.Payload.Integer(9)
 print(ast.payload_text(tree.children[0].payload),ast.payload_text(detached.children[0].payload))
''','3 1 7\n1 9\n')
case('shared-subtree-cost','''
 let pair=direct.binary("+",s,direct.leaf(1,s),direct.leaf(2,s))
 let tree=direct.binary("*",s,pair,pair)
 print(direct.visits(tree),direct.visits(clone(tree)))
 let shared=unwrap(ast.add(pool,ast.Payload.Binary("+"),s,[a,b]))
 let root=unwrap(ast.add(pool,ast.Payload.Binary("*"),s,[shared,shared]))
 print(len(unwrap(ast.preorder(pool,root,7))),len(unwrap(ast.reachable(pool,root,4))))
''','7 7\n7 4\n')
case('stable-id-and-writeback','''
 let root=unwrap(ast.add(pool,ast.Payload.Binary("+"),s,[a,b]))
 for i in 0..128{unwrap(ast.add(pool,ast.Payload.Integer(i),s,empty))}
 print(a.index,b.index,root.index,ast.count(pool))
 var detached=clone(unwrap(ast.get(pool,root)))
 detached.children[0]=b
 print(unwrap(ast.get(pool,root)).children[0].index)
 unwrap(ast.replace(pool,root,detached.payload,detached.span,detached.children))
 print(unwrap(ast.get(pool,root)).children[0].index,ast.same(root,unwrap(ast.get(pool,root)).id))
 unwrap(ast.verify(pool))
''','0 1 2 131\n0\n1 true\n')
case('input-children-detach','''
 var children=[a,b]
 let root=unwrap(ast.add(pool,ast.Payload.Binary("+"),s,children))
 children[0]=b
 print(unwrap(ast.get(pool,root)).children[0].index)
 unwrap(ast.replace(pool,root,ast.Payload.Binary("*"),s,children))
 children[0]=a
 print(unwrap(ast.get(pool,root)).children[0].index)
''','0\n1\n')
case('cross-pool-and-bad-ids','''
 var other=ast.create()
 let otherSource=unwrap(ast.register_source(other,"other","1"))
 let otherSpan=unwrap(ast.span(other,otherSource,0,1))
 let foreign=unwrap(ast.add(other,ast.Payload.Integer(3),otherSpan,empty))
 print(ast.same(a,foreign),unwrap_err(ast.get(pool,foreign)))
 print(unwrap_err(ast.get(pool,ast.NodeId{pool:pool.identity,index:-1})))
 print(unwrap_err(ast.get(pool,ast.NodeId{pool:pool.identity,index:99})))
 print(unwrap_err(ast.span(pool,otherSource,0,1)))
 print(unwrap_err(ast.add(pool,ast.Payload.Unary("-"),s,[foreign])),ast.count(pool))
''','false AST_BAD_NODE_ID\nAST_BAD_NODE_ID\nAST_BAD_NODE_ID\nAST_BAD_SOURCE_ID\nAST_BAD_NODE_ID 2\n')
case('source-and-span-errors','''
 let again=unwrap(ast.register_source(pool,"sample.hua","1 + 2\\r\\n"))
 print(source.index==again.index,unwrap_err(ast.register_source(pool,"sample.hua","other")))
 print(unwrap_err(ast.register_source(pool,"","x")))
 print(unwrap_err(ast.span(pool,source,-1,1)),unwrap_err(ast.span(pool,source,2,1)),unwrap_err(ast.span(pool,source,0,8)))
 print(unwrap_err(ast.source_info(pool,ast.SourceId{pool:pool.identity,index:-1})))
 print(unwrap_err(ast.source_info(pool,ast.SourceId{pool:pool.identity,index:99})))
 let fake=ast.SourceSpan{source:source,start:0,end:1,line:99,column:1}
 print(unwrap_err(ast.add(pool,ast.Payload.Integer(0),fake,empty)),ast.count(pool))
''','true AST_SOURCE_CHANGED\nAST_SOURCE_NAME\nAST_BAD_SPAN AST_BAD_SPAN AST_BAD_SPAN\nAST_BAD_SOURCE_ID\nAST_BAD_SOURCE_ID\nAST_BAD_SPAN 2\n')
case('line-interval-validation','''
 let next=unwrap(ast.register_source(pool,"lines.hua","a\\n\\n中\\n"))
 for offset in [0,1,2,3,4,5,6,7] { let correct=unwrap(ast.span(pool,next,offset,offset))
  let accepted=unwrap(ast.add(pool,ast.Payload.Integer(0),correct,empty))
  print(unwrap(ast.get(pool,accepted)).span.line) }
 let wrongline=ast.SourceSpan{source:next,start:2,end:2,line:1,column:3}
 let wrongcolumn=ast.SourceSpan{source:next,start:3,end:3,line:3,column:2}
 let zeroline=ast.SourceSpan{source:next,start:0,end:0,line:0,column:1}
 print(unwrap_err(ast.add(pool,ast.Payload.Integer(0),wrongline,empty)))
 print(unwrap_err(ast.add(pool,ast.Payload.Integer(0),wrongcolumn,empty)))
 print(unwrap_err(ast.add(pool,ast.Payload.Integer(0),zeroline,empty)))
''','1\n1\n2\n3\n3\n3\n3\n4\nAST_BAD_SPAN\nAST_BAD_SPAN\nAST_BAD_SPAN\n')
case('multiple-source-spans','''
 let second=unwrap(ast.register_source(pool,"second.hua","f()\\n"))
 let position=unwrap(ast.span(pool,second,0,1))
 let name=unwrap(ast.add(pool,ast.Payload.Name("f"),position,empty))
 let root=unwrap(ast.add(pool,ast.Payload.Block(),s,[a,name]))
 let node=unwrap(ast.get(pool,root))
 print(unwrap(ast.source_info(pool,unwrap(ast.get(pool,node.children[0])).span.source)).name)
 print(unwrap(ast.source_info(pool,unwrap(ast.get(pool,node.children[1])).span.source)).name)
 print(source.index,second.index,len(unwrap(ast.preorder(pool,root,3))))
 unwrap(ast.verify(pool))
''','sample.hua\nsecond.hua\n0 1 3\n')
case('invalid-edits-atomic','''
 let root=unwrap(ast.add(pool,ast.Payload.Binary("+"),s,[a,b]))
 print(unwrap_err(ast.add(pool,ast.Payload.Binary("unknown"),s,[a,b])))
 print(unwrap_err(ast.add(pool,ast.Payload.Binary("+"),s,[a])))
 print(unwrap_err(ast.add(pool,ast.Payload.Name(""),s,empty)))
 print(unwrap_err(ast.replace(pool,root,ast.Payload.Unary("-"),s,[root])))
 print(unwrap_err(ast.replace(pool,a,ast.Payload.Unary("-"),s,[b])))
 print(unwrap_err(ast.replace(pool,root,ast.Payload.Integer(8),s,[a])))
 print(ast.count(pool),ast.kind(unwrap(ast.get(pool,root)).payload),len(unwrap(ast.get(pool,a)).children))
 unwrap(ast.verify(pool))
''','AST_BAD_SHAPE\nAST_BAD_SHAPE\nAST_BAD_SHAPE\nAST_FORWARD_EDGE\nAST_FORWARD_EDGE\nAST_BAD_SHAPE\n3 Binary 0\n')
case('ordered-dag-rewrite-and-budget','''
 let shared=unwrap(ast.add(pool,ast.Payload.Binary("+"),s,[a,b]))
 let root=unwrap(ast.add(pool,ast.Payload.Binary("*"),s,[shared,shared]))
 for id in unwrap(ast.preorder(pool,root,7)){print(id.index)}
 print(unwrap(ast.reachable(pool,root,4)))
 print(ast.same(root,unwrap(ast.rewrite_integer(pool,root,99,100,4))),ast.count(pool))
 let changed=unwrap(ast.rewrite_integer(pool,root,1,10,4))
 let node=unwrap(ast.get(pool,changed))
 print(changed.index,ast.count(pool),ast.same(node.children[0],node.children[1]))
 print(unwrap(ast.get(pool,root)).children[0].index)
 print(unwrap_err(ast.preorder(pool,root,6)),unwrap_err(ast.reachable(pool,root,3)))
 let before=ast.count(pool)
 print(unwrap_err(ast.rewrite_integer(pool,root,1,8,3)),before==ast.count(pool))
 print(unwrap_err(ast.preorder(pool,root,-1)),unwrap_err(ast.preorder(pool,root,0)))
''','3\n2\n0\n1\n2\n0\n1\n[0, 1, 2, 3]\ntrue 4\n6 7 true\n2\nAST_WALK_LIMIT AST_WALK_LIMIT\nAST_WALK_LIMIT true\nAST_WALK_LIMIT AST_WALK_LIMIT\n')
case('all-18-variants','''
 let zero=[ast.Payload.Integer(5),ast.Payload.Float(2.5),ast.Payload.Boolean(true),ast.Payload.Name("x"),ast.Payload.Parameter("x")]
 for p in zero{print(ast.kind(unwrap(ast.get(pool,unwrap(ast.add(pool,p,s,empty)))).payload))}
 let unary=[ast.Payload.Unary("-"),ast.Payload.Binding("x"),ast.Payload.Return(),ast.Payload.ExpressionStatement()]
 for p in unary{print(ast.kind(unwrap(ast.get(pool,unwrap(ast.add(pool,p,s,[a])))).payload))}
 let binary=[ast.Payload.Binary("+"),ast.Payload.Assignment("="),ast.Payload.Function("f"),ast.Payload.If(),ast.Payload.While()]
 for p in binary{print(ast.kind(unwrap(ast.get(pool,unwrap(ast.add(pool,p,s,[a,b])))).payload))}
 let many=[ast.Payload.Parameters(),ast.Payload.Call(),ast.Payload.Block(),ast.Payload.Program()]
 for p in many{print(ast.kind(unwrap(ast.get(pool,unwrap(ast.add(pool,p,s,[a,b])))).payload))}
 unwrap(ast.add(pool,ast.Payload.If(),s,[a,b,a]))
 unwrap(ast.add(pool,ast.Payload.Return(),s,empty))
 unwrap(ast.verify(pool))
''','Integer\nFloat\nBoolean\nName\nParameter\nUnary\nBinding\nReturn\nExpressionStatement\nBinary\nAssignment\nFunction\nIf\nWhile\nParameters\nCall\nBlock\nProgram\n')
case('validator-malformed-record','''
 let root=unwrap(ast.add(pool,ast.Payload.Unary("-"),s,[a]))
 var node=clone(unwrap(ast.get(pool,root)))
 node.children[0]=root
 unwrap(list.set(pool.nodes,root.index,node))
 print(unwrap_err(ast.verify(pool)))
 node.id=a
 unwrap(list.set(pool.nodes,root.index,node))
 print(unwrap_err(ast.get(pool,root)))
''','AST_FORWARD_EDGE\nAST_BAD_NODE_ID\n')
case('readonly-child-static','''
 let root=unwrap(ast.add(pool,ast.Payload.Unary("-"),s,[a]))
 var node=unwrap(ast.get(pool,root))
 node.children[0]=b
''',error='E3003',static=True)
case('readonly-pool-static','''import ast_pool as ast
fn change(pool ast.Pool){ast.register_source(pool,"x","1")}
fn main(){var pool=ast.create()
change(pool)}
''',error='E3003',static=True,full=True)
case('wrong-payload-static','''import ast_pool as ast
let bad=ast.Payload.Integer("1")
''',error='E3004',static=True,full=True)
for size in (1,64,256):
 case(f'pool-chain-{size}',f'''
 var root=a
 for i in 0..{size-1}{{root=unwrap(ast.add(pool,ast.Payload.Unary("-"),s,[root]))}}
 let order=unwrap(ast.preorder(pool,root,{size}))
 print(len(order),order[0].index,order[len(order)-1].index)
 let before=ast.count(pool)
 let next=unwrap(ast.rewrite_integer(pool,root,1,7,{size}))
 print(ast.count(pool)-before,len(unwrap(ast.preorder(pool,next,{size}))))
 print(ast.payload_text(unwrap(ast.get(pool,a)).payload))
 unwrap(ast.verify(pool))
''',f'{size} {0 if size==1 else size} 0\n{size} {size}\n1\n')
case('wide-256','''
 var children=list.from_slice(empty)
 for i in 0..256{unwrap(list.append(children,unwrap(ast.add(pool,ast.Payload.Integer(i),s,empty))))}
 let root=unwrap(ast.add(pool,ast.Payload.Block(),s,list.snapshot(children)))
 let order=unwrap(ast.preorder(pool,root,257))
 print(len(order),order[1].index,order[256].index)
 print(len(unwrap(ast.reachable(pool,root,257))))
''','257 2 257\n257\n')
case('direct-deep-clone-boundary','''
 var node=direct.leaf(1,s)
 for i in 0..80{node=direct.DirectNode{payload:ast.Payload.Unary("-"),span:s,children:[node]}}
 print("constructed",81)
 let detached=clone(node)
 print("unexpected-clone")
''','constructed 81\n',error='E4007')
positions=[]
for index,text in enumerate(('', 'a', 'a\n', '\ufeffa\r\n中\ré\n', '\t🙂e\u0301\r\nlast', '\ufeff\ufeffx\r')):
 p=work/f'位置-{index}.txt';p.write_text(text,encoding='utf-8',newline='')
 expected,_=run([oracle,'positions',p])
 normalized=text.removeprefix('\ufeff').replace('\r\n','\n').replace('\r','\n');n=len(normalized.encode('utf-8'))
 assert expected.startswith(f'size\t{n}\n');checks+=1
 literal=json.dumps(text,ensure_ascii=False)
 case(f'positions-{index}',f'''import ast_pool as ast
import std.bytes as bytes
fn main(){{
 var pool=ast.create()
 let source=unwrap(ast.register_source(pool,"中文文件.hua",{literal}))
 let info=unwrap(ast.source_info(pool,source))
 print("size\\t"+str(info.size))
 print("data\\t"+str(unwrap(bytes.to_ints(info.data))))
 for i in 0..info.size+1{{let s=unwrap(ast.span(pool,source,i,i))
 print("position\\t"+str(i)+"\\t"+str(s.line)+"\\t"+str(s.column))}}
}}
''',expected,full=True)
 positions.append({'case':index,'bytes':n,'positions':n+1});p.unlink()
report={'result':'passed','task':'A05','cases':len(rows),'checks':checks,'platform':sys.platform,'executable':str(hua),'subset':'SB1','expression_oracle_nodes':7,'source_fixtures':positions,'chain_nodes':[1,64,256],'wide_children':256,'direct_clone_boundary':'81 nodes: E4007 depth rejection currently misreports File','production_changes':False,'rows':rows}
(out/'ast-pool-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(f'A05: {len(rows)} cases, {checks} checks; SB1 AST/VM/source-free HUAB and C++ references passed')
