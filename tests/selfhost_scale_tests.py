"""A11 real corpus and complete C04 frontend: bounded, deterministic, four paths."""
from pathlib import Path
import sys,subprocess,json,time,shutil,hashlib,os,ctypes
hua,probe,oracle,repo,build=map(Path,sys.argv[1:])
repo=repo.resolve();hua=hua.resolve();probe=probe.resolve();oracle=oracle.resolve();build=build.resolve()
out=build/'a11-c04';out.mkdir(exist_ok=True);work=out/'scale';work.mkdir(exist_ok=True)
rows=[];checks=0
def command(args,timeout=600):
    started=time.monotonic()
    p=subprocess.Popen(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    peak=None
    if os.name=='nt':
        class Counters(ctypes.Structure):
            _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong),('peak',ctypes.c_size_t),('working',ctypes.c_size_t),('quotaPeakPaged',ctypes.c_size_t),('quotaPaged',ctypes.c_size_t),('quotaPeakNonPaged',ctypes.c_size_t),('quotaNonPaged',ctypes.c_size_t),('pagefile',ctypes.c_size_t),('peakPagefile',ctypes.c_size_t)]
        def memory():
            c=Counters();c.cb=ctypes.sizeof(c)
            if ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(p._handle)),ctypes.byref(c),c.cb):return c.peak
            return None
    else:
        def memory():return None
    try:
        while True:
            observed=memory()
            if observed:peak=max(peak or 0,observed)
            remaining=timeout-(time.monotonic()-started)
            if remaining<=0:raise subprocess.TimeoutExpired(args,timeout)
            try:stdout,stderr=p.communicate(timeout=min(2,remaining));break
            except subprocess.TimeoutExpired:pass
    except BaseException:
        p.kill();p.communicate();raise
    observed=memory()
    if observed:peak=max(peak or 0,observed)
    assert p.returncode==0,(args,p.returncode,stderr.decode('utf-8'))
    return stdout.decode('utf-8'),stderr.decode('utf-8'),time.monotonic()-started,peak
closure=work/'persisted';closure.mkdir(exist_ok=True)
for p in (repo/'selfhost').glob('*.hua'):shutil.copy2(p,closure/p.name)
artifacts={}
for entry in ['parser_compiler_main','modules_compiler_main','types_main']:
    target=closure/(entry+'.huab');command([probe,'build',closure/f'{entry}.hua','-o',target]);artifacts[entry]=target
for p in closure.glob('*.hua'):p.unlink()
def paths(entry,source,parse=False):
    args=[source,source.name] if parse else [source]
    result=None;measurements=[]
    for exe,engine,target,label in [(hua,'interpret',repo/'selfhost'/f'{entry}.hua','AST'),(hua,'run',repo/'selfhost'/f'{entry}.hua','VM'),(hua,'run',artifacts[entry],'HUAB'),(probe,'run',repo/'selfhost'/f'{entry}.hua','SB1')]:
        raw,err,seconds,peak=command([exe,engine,target,'--execution-profile','compiler','--report-budget','--',*args])
        report=json.loads(raw);metrics=json.loads(err)
        assert report['status']=='ok',(source,label,report)
        assert metrics['steps']<=500000000 and metrics['peak_calls']<=128
        if result is None:result=report
        else:assert report==result,(source,label)
        measurements.append({'engine':label,'seconds':round(seconds,3),'peak_working_bytes':peak,**metrics})
    return result,measurements
def projection(d):
    return [{k:n[k] for k in ('kind','text','start','end','line','column','depth')}|{'arity':len(n['children'])} for n in d['nodes']]
for source in sorted((repo/'selfhost').glob('*.hua')):
    result,metrics=paths('parser_compiler_main',source,True)
    reference=json.loads(command([oracle,source])[0])
    assert reference['status']=='ok' and projection(result)==reference['nodes'],source
    checks+=7
    rows.append({'kind':'parser','source':source.name,'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'stats':result['stats'],'measurements':metrics})
    print('A11 parsed',source.name,result['stats'],flush=True)
# Entire name-analysis closure and entire typed frontend closure, not just fragments.
for entry in ['modules_compiler_main','types_main']:
    source=repo/'selfhost'/f'{entry}.hua';result,metrics=paths(entry,source)
    reference=json.loads(command([oracle,'--types' if entry=='types_main' else '--modules',source])[0]);assert reference['status']=='ok';checks+=6
    graph=result.get('graph',result)
    row={'kind':'types' if entry=='types_main' else 'names','source':source.name,'modules':len(graph['modules']),'bytes':graph['stats']['input_bytes'],'symbols':len(graph['symbols']),'references':len(graph['references']),'measurements':metrics,'input_hashes':{u['id']:hashlib.sha256((source.parent/u['id']).read_bytes()).hexdigest() for u in graph['modules']}}
    if entry=='types_main':
        # Full facts compare where both models record an unchanged source expression.
        refs={}
        for f in reference['facts']:refs.setdefault((f['module'],f['start'],f['end']),[]).append(f)
        matched=0;different=[]
        for f in result['facts']:
            options=refs.get((f['module'],f['start'],f['end']),[])
            if options and any(f['type_name']==x['type_name'] and (x['writable'] is None or f['writable']==x['writable']) for x in options):matched+=1
            elif options:different.append({'hua':f,'cpp':options})
        row.update(facts=len(result['facts']),matched_facts=matched,different_facts=len(different),stats=result['stats'])
        # Type facts may refine legacy empty annotations or conservatively reduce capabilities.
        # Never label full-model equivalence on the strength of just a successful reference check.
        (out/'self-fact-differences.json').write_text(json.dumps(different,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        (out/'self-types-final.json').write_text(json.dumps(result,ensure_ascii=False)+'\n',encoding='utf-8')
    rows.append(row);print('A11 complete closure',entry,row['bytes'],'facts',row.get('facts'),flush=True)
(out/'scale-results.json').write_text(json.dumps({'parser_files':sum(x['kind']=='parser' for x in rows),'checks':checks,'rows':rows},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print('A11 complete scale acceptance',len(rows),'rows /',checks,'checks',flush=True)
