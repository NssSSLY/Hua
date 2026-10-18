"""Canonical array-size spelling, rejection before effects, and old HUAB reading."""
from pathlib import Path
import base64, hashlib, json, re, shutil, sys, time
from process_guard import run
hua, oracle, repo, build = map(lambda x: Path(x).resolve(), sys.argv[1:5])
work = build/'performance-audit'/'grammar'; work.mkdir(parents=True, exist_ok=True)
started=time.monotonic(); rows=[]; checks=0
cases=json.loads((repo/'tests/data/a02_cases.json').read_text(encoding='utf-8'))
for case in [c for c in cases if c['name'].startswith('array_syntax_')]:
    source=work/(case['name']+'.hua'); source.write_text(case['source'], encoding='utf-8')
    image=source.with_suffix('.huab'); image.unlink(missing_ok=True)
    for action in ('check','interpret','run','build'):
        args=[hua,action,source]+(['-o',image] if action=='build' else [])
        result=run(list(map(str,args)),capture_output=True,text=True,encoding='utf-8')
        assert result.returncode==1 and 'error[E2006]' in result.stderr and not result.stdout, (case['name'],action,result)
        assert not image.exists()
        checks+=2
    rows.append({'case':case['name'],'status':'rejected'})
positive=work/'valid.hua';positive.write_text('fn main(){let a [2]int=[1,2]\nlet z [0]int=[]\nlet b [2][2]int=[[1,2],[3,4]]\nprint(a,z,b,0x2,0b10,0o2,0_2,02)}\n',encoding='utf-8')
for action in ('check','interpret','run'):
    result=run([str(hua),action,str(positive)],capture_output=True,text=True,encoding='utf-8'); assert result.returncode==0, result.stderr;checks+=1
for fixture in json.loads((repo/'tests/data/a02_legacy_arrays.json').read_text(encoding='utf-8')):
    data=base64.b64decode(fixture['base64']);assert hashlib.sha256(data).hexdigest()==fixture['sha256']
    image=work/(fixture['name']+'.huab');image.write_bytes(data)
    result=run([str(hua),'run',str(image)],capture_output=True,text=True,encoding='utf-8')
    assert result.returncode==0 and result.stdout==fixture['stdout'],result
    checks+=2
# Prepare the checker once; each rejected spelling still gets its own full report.
# Keep its sources in the test worktree, never overwrite the repository compiler.
checker_sources=work/'checker-sources';checker_sources.mkdir(exist_ok=True)
units=sorted((repo/'selfhost').glob('*.hua'))
for unit in units:shutil.copy2(unit,checker_sources/unit.name)
checker=work/'checker.huab'
print('array syntax: preparing one checker',flush=True)
result=run([str(hua),'build',str(checker_sources/'types_main.hua'),'-o',str(checker)],capture_output=True,text=True,encoding='utf-8')
assert result.returncode==0,result.stderr;checks+=1
# The Hua parser rejects the same token and position as the independent host parser.
for case in [c for c in cases if c['name'].startswith('array_syntax_')]:
    source=work/(case['name']+'.hua')
    ref=run([str(oracle),'--types',str(source)],capture_output=True,text=True,encoding='utf-8');expected=json.loads(ref.stdout)
    result=run([str(hua),'run',str(checker),'--execution-profile','compiler','--',str(source)],capture_output=True,text=True,encoding='utf-8')
    report=json.loads(result.stdout)
    assert result.returncode==0 and report['status']=='error',report
    assert report['error'].startswith(f"E2006@{expected['line']}:{expected['column']}"),(report,expected)
    checks+=2
# A single import graph checks every current unit, with all original module bodies
# and imports. Rebuilding the same dependency closure for each entry is redundant.
aggregate=checker_sources/'array_syntax_all_units.hua'
aggregate.write_text(''.join('import '+unit.stem+' as unit'+str(i)+'\n' for i,unit in enumerate(units)),encoding='utf-8',newline='\n')
print('array syntax: checking all '+str(len(units))+' units in one graph',flush=True)
result=run([str(hua),'check',str(aggregate)],capture_output=True,text=True,encoding='utf-8');assert result.returncode==0,result.stderr;checks+=1
report={'status':'passed','rejected_cases':len(rows),'checks':checks,'seconds':round(time.monotonic()-started,3),'rows':rows,'checker_builds':1,'selfhost_checked_units':len(units),'selfhost_sources':{unit.name:hashlib.sha256(unit.read_bytes()).hexdigest() for unit in units}}
(work/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(json.dumps(report,ensure_ascii=False))
