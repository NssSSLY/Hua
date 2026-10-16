"""BO-01: Windows x64 native stack sampling of fixed small compiler inputs.

No full own-source run. CPU-active samples are estimates, not exact function timers.
All child commands stop on first timeout/failure; completed evidence is immutable.
"""
from pathlib import Path
import argparse
from collections import Counter
import hashlib
import json
import os
import statistics
import struct
import subprocess
import sys

from audit_bootstrap_phases import closure, command, digest, machine, save


def pe_text(path):
    """Fingerprint executable code separately from debug sections and PE timestamp."""
    data=Path(path).read_bytes()
    offset=struct.unpack_from('<I',data,0x3c)[0]
    assert data[offset:offset+4]==b'PE\0\0'
    count=struct.unpack_from('<H',data,offset+6)[0]
    size=struct.unpack_from('<H',data,offset+20)[0]
    assert struct.unpack_from('<H',data,offset+4)[0]==0x8664, 'Windows x64 required'
    for i in range(count):
        section=offset+24+size+i*40
        if data[section:section+8].rstrip(b'\0')==b'.text':
            length,raw=struct.unpack_from('<II',data,section+16)
            return hashlib.sha256(data[raw:raw+length]).hexdigest()
    raise AssertionError('missing .text')


def initialize(root,out,native,sampler,calibration,symbolizer):
    assert os.name=='nt' and struct.calcsize('P')==8, 'Windows x64 only'
    assert not (out/'manifest.json').exists(), 'use a fresh output directory'
    out.mkdir(parents=True,exist_ok=True)
    files={}
    names=subprocess.check_output(['git','ls-files','-z','--cached','--others','--exclude-standard'],cwd=root).split(b'\0')
    for raw in names:
        if raw:
            name=raw.decode('utf-8');path=root/name
            if path.is_file():files[name]=digest(path)
    sources=closure(root/'selfhost/archive_main.hua')
    frozen=out/'frozen/selfhost';frozen.mkdir(parents=True)
    for name,path in sources.items():(frozen/name).write_bytes(path.read_bytes())
    inputs=out/'inputs';inputs.mkdir()
    for name in ('catalog','type_rules'):(inputs/(name+'.hua')).write_bytes((frozen/(name+'.hua')).read_bytes())
    cache=native.parent/'CMakeCache.txt'
    options=[line for line in cache.read_text(encoding='utf-8').splitlines() if line.startswith(('CMAKE_BUILD_TYPE:','CMAKE_CXX_COMPILER:','CMAKE_CXX_FLAGS_RELEASE:','HUA_INTERNAL_PROFILE:','HUA_ENABLE_PYTHON_BRIDGE:'))]
    assert 'HUA_INTERNAL_PROFILE:BOOL=OFF' in options
    assert 'HUA_ENABLE_PYTHON_BRIDGE:BOOL=OFF' in options
    manifest={'schema':'hua.native-cpu.audit.v1','host':machine(),'git_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
              'git_status':subprocess.check_output(['git','-c','core.quotepath=false','status','--short'],cwd=root,text=True,encoding='utf-8').splitlines(),
              'files':files,'compiler_sources':{name:digest(path) for name,path in sources.items()},
              'input_sources':{p.name:digest(p) for p in inputs.glob('*.hua')},'build_options':options,
              'tools':{key:{'path':str(path),'sha256':digest(path)} for key,path in [('native',native),('sampler',sampler),('calibration',calibration),('symbolizer',symbolizer)]},
              'native_text_sha256':pe_text(native),'full_own_source_run':False,
              'method':'external main-thread periodic stacks with PE unwind tables; offline LLVM inline symbolization; no counters or production code changes',
              'limitations':['periodic active-stack samples estimate CPU attribution; they are not ETW CPU events or precise function timers',
                             'cycle delta is only used to filter near-idle samples, never converted to time',
                             'startup before first sample and other threads are uncovered and reported separately']}
    save(out/'manifest.json',manifest)


def verify(out,require_component=True):
    manifest=json.loads((out/'manifest.json').read_text(encoding='utf-8'))
    if require_component:
        assert 'component_sha256' in manifest, 'STOP: component preparation did not finish; use a new measured preparation plan'
    for row in manifest['tools'].values(): assert digest(row['path'])==row['sha256'],row
    if require_component:assert digest(out/'compiler.huab')==manifest['component_sha256']
    for name,sha in manifest['input_sources'].items():assert digest(out/'inputs'/name)==sha
    for name,sha in manifest['compiler_sources'].items():assert digest(out/'frozen/selfhost'/name)==sha
    return manifest


def symbolize(root,out,tracefile,executable,symbolizer,label):
    raw=json.loads(tracefile.read_text(encoding='utf-8'));assert not raw['timeout'] and raw['exit_code']==0
    module=next(m for m in raw['modules'] if Path(m['path']).resolve()==executable.resolve())
    base,size=module['base'],module['size']
    addresses=sorted({pc-base-(index>0) for s in raw['samples'] for index,pc in enumerate(s['frames']) if base<=pc<base+size})
    text=''.join(hex(a)+'\n' for a in addresses)
    # Reuse the bounded process supervisor, with input in a dedicated file redirected by LLVM.
    inputfile=tracefile.with_suffix('.addresses');inputfile.write_text(text,encoding='ascii')
    args=[symbolizer,'--obj='+str(executable),'--relative-address','--inlining','--output-style=JSON']
    from process_guard import run
    result=run(list(map(str,args)),input=text,capture_output=True,text=True,encoding='utf-8',timeout=30)
    tracefile.with_suffix('.symbols.jsonl').write_text(result.stdout,encoding='utf-8')
    assert result.returncode==0,result.stderr
    symbols={int(row['Address'],16):row['Symbol'] for row in map(json.loads,result.stdout.splitlines())}
    assert set(symbols)==set(addresses),label
    samples=raw['samples'];assert samples
    # Suspend/resume itself can produce a tiny cycle delta during Sleep. Drop these
    # using a declared relative threshold; keep the entire unfiltered trace as evidence.
    threshold=statistics.median(s['cycle_delta'] for s in samples)*.1
    active=[s for s in samples if s['cycle_delta']>=threshold]
    leaf=Counter();nearest=Counter();inclusive=Counter();locations=Counter()
    quarter=parent=busy=unknown_leaf=unresolved=0
    for sample in active:
        chain=[]
        for index,pc in enumerate(sample['frames']):
            if base<=pc<base+size:
                frames=symbols[pc-base-(index>0)]
                chain.extend(frames)
                if index==0 and frames:
                    frame=frames[0];leaf[frame['FunctionName']]+=1
                    locations[(frame['FunctionName'],frame['FileName'],frame['Line'])]+=1
            elif index==0:
                m=next((m for m in raw['modules'] if m['base']<=pc<m['base']+m['size']),None)
                leaf['[system] '+Path(m['path']).name if m else '[unknown module]']+=1
                unknown_leaf+=1
        names={f['FunctionName'] for f in chain if f['FunctionName'] not in ('','??')}
        inclusive.update(names)
        owned=next((f for f in chain if '/src/' in f['FileName'].replace('\\','/') and 'hua' in f['FunctionName']),None)
        if owned:nearest[owned['FunctionName']]+=1
        else:nearest['[no Hua frame]']+=1
        if not names:unresolved+=1
        if any('calibration_parent' in n for n in names):parent+=1
        if any('calibration_quarter()'==n for n in names):quarter+=1;busy+=1
        elif any('calibration_three_quarters()'==n for n in names):busy+=1
    def rank(counts):return [{'name':n,'samples':c,'percent':100*c/len(active)} for n,c in counts.most_common()]
    result={key:raw[key] for key in ('interval_ms','wall_seconds','cpu_seconds','main_thread_cpu_seconds','peak_working_bytes','inactive_polls','context_failures')}
    pauses=[s['pause_ns'] for s in samples];stamps=[s['ns'] for s in samples]
    result.update({'samples':len(samples),'active_samples':len(active),'filtered_low_cycle_samples':len(samples)-len(active),
                   'low_cycle_threshold':threshold,'first_sample_seconds':stamps[0]/1e9,
                   'cadence_median_ms':statistics.median(b-a for a,b in zip(stamps,stamps[1:]))/1e6 if len(samples)>1 else None,
                   'suspended_seconds':sum(pauses)/1e9,'pause_median_ms':statistics.median(pauses)/1e6,'pause_max_ms':max(pauses)/1e6,
                   'main_thread_cpu_fraction':raw['main_thread_cpu_seconds']/raw['cpu_seconds'] if raw['cpu_seconds'] else None,
                   'median_depth':statistics.median(len(s['frames']) for s in samples),'unknown_system_leaf_samples':unknown_leaf,
                   'unresolved_stack_samples':unresolved,'leaf':rank(leaf),'nearest_hua_frame':rank(nearest),'inclusive':rank(inclusive),
                   'locations':[{'name':name,'file':file,'line':line,'samples':count} for (name,file,line),count in locations.most_common()],
                   'calibration_busy_samples':busy,'calibration_quarter_fraction':quarter/busy if busy else None,
                   'calibration_parent_fraction':parent/busy if busy else None})
    save(tracefile.with_suffix('.summary.json'),result)
    return result


def calibrate(root,out):
    m=verify(out,False);t={k:Path(v['path']) for k,v in m['tools'].items()}
    normal,stdout,_=command(root,out,'calibration-normal',[t['calibration']])
    trace=out/'calibration-8ms.json'
    command(root,out,'calibration-8ms',[t['sampler'],trace,'8','10000',t['calibration']],15)
    result=symbolize(root,out,trace,t['calibration'],t['symbolizer'],'calibration')
    assert .15<=result['calibration_quarter_fraction']<=.35,result
    assert result['calibration_parent_fraction']>=.95,result
    assert result['median_depth']>=3 and result['active_samples']>=100,result
    assert result['filtered_low_cycle_samples']>=5,result
    assert trace.with_name(trace.name+'.stdout').read_bytes()==stdout
    result['normal']=normal
    result['pause_wall_fraction']=result['suspended_seconds']/result['wall_seconds']
    assert result['pause_wall_fraction']<.15,result
    save(out/'calibration.json',{'status':'passed','result':result})
    print(json.dumps({'calibration':'passed','active_samples':result['active_samples'],'quarter_fraction':result['calibration_quarter_fraction'],'pause_fraction':result['pause_wall_fraction']}),flush=True)


def preparation_summary(out,probe):
    m=verify(out,False)
    for label in ('host-full-sampled','host-component-check','host-full-cli-control'):
        row=json.loads((out/'commands'/(label+'.json')).read_text(encoding='utf-8'))
        assert row['returncode']==0 and not row['timeout'],label
    assert (out/'compiler.huab').read_bytes()==(out/'compiler-cli.huab').read_bytes()
    events=[json.loads(line) for line in (out/'host-full-stages.jsonl').read_text(encoding='utf-8').splitlines()]
    stages={};opened={}
    for event in events:
        key=event['phase']
        if event['event']=='begin':assert key not in opened;opened[key]=event
        else:
            assert event['event']=='end' and key in opened,event
            start=opened.pop(key)
            stages[key]={name:event[name]-start[name] for name in ('seconds','cpu_seconds')}
    assert not opened and set(stages)=={'execution','load','sema','bytecode','archive','destroy'}
    m['component_sha256']=digest(out/'compiler.huab')
    m['preparation']={'phase_probe':digest(probe),'native_cli_bytes_identical':True,'reader_checked':True,
                      'sampled_limit_seconds':json.loads((out/'host-full-samples.json').read_text(encoding='utf-8'))['limit_ms']/1000,
                      'normal_limit_seconds':json.loads((out/'commands/host-full-cli-control.json').read_text(encoding='utf-8'))['limit_seconds']}
    save(out/'manifest.json',m)
    save(out/'preparation.json',{'status':'passed','stages':stages,'component_sha256':m['component_sha256'],'full_own_source_run':False})


def prepare(root,out,probe,limit):
    m=verify(out,False);assert 30<=limit<=180,'explicit host-only limit must be 30..180 seconds'
    assert json.loads((out/'calibration.json').read_text(encoding='utf-8'))['status']=='passed'
    assert not (out/'compiler.huab').exists(), 'never overwrite preparation evidence'
    native=Path(m['tools']['native']['path']);sampler=Path(m['tools']['sampler']['path'])
    command(root,out,'host-full-sampled',[sampler,out/'host-full-samples.json','8',str(limit*1000),probe,
                                         out/'host-full-stages.jsonl',out/'frozen/selfhost/archive_main.hua',out/'compiler.huab'],limit+5)
    symbolize(root,out,out/'host-full-samples.json',probe,Path(m['tools']['symbolizer']['path']),'host-full')
    command(root,out,'host-component-check',[native,'check',out/'compiler.huab'])
    command(root,out,'host-full-cli-control',[native,'build',out/'frozen/selfhost/archive_main.hua','-o',out/'compiler-cli.huab'],limit)
    preparation_summary(out,probe)


def measure(root,out):
    m=verify(out);assert json.loads((out/'calibration.json').read_text(encoding='utf-8'))['status']=='passed'
    assert not (out/'results.json').exists(),'completed experiments are not rerun'
    t={k:Path(v['path']) for k,v in m['tools'].items()};rows=[]
    save(out/'results.json',{'status':'running','rows':rows,'full_own_source_run':False})
    for name in ('catalog','type_rules'):
        source=out/'inputs'/(name+'.hua');artifacts=[];reports=[];normals=[]
        for label in ('before','sampled','after'):
            artifact=out/(name+'-'+label+'.huab');artifacts.append(artifact)
            args=[t['native'],'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact]
            if label=='sampled':
                trace=out/(name+'-samples.json')
                command(root,out,name+'-'+label,[t['sampler'],trace,'8','30000',*args],35)
                report=trace.with_name(trace.name+'.stdout').read_bytes()
                stderr=trace.with_name(trace.name+'.stderr').read_bytes()
                profile=symbolize(root,out,trace,t['native'],t['symbolizer'],name)
                assert profile['main_thread_cpu_fraction']>=.95 and profile['active_samples']>=80,profile
            else:
                row,report,stderr=command(root,out,name+'-'+label,args)
                normals.append(row)
            receipt=json.loads(report);assert receipt['status']=='ok',receipt
            reports.append(report)
            budget=json.loads(stderr);assert budget['steps']>0,budget
            if label=='before':expected_budget=budget
            else:assert budget==expected_budget,(name,budget,expected_budget)
            if len(reports)>1:assert reports[-1]==reports[0] and artifacts[-1].read_bytes()==artifacts[0].read_bytes(),name
        wall=statistics.mean(r['seconds'] for r in normals);cpu=statistics.mean(r['cpu_seconds'] for r in normals)
        profile['normal_bracket']=normals;profile['wall_overhead_fraction']=profile['wall_seconds']/wall-1
        profile['cpu_overhead_fraction']=profile['cpu_seconds']/cpu-1
        profile['normal_bracket_wall_drift_fraction']=normals[-1]['seconds']/normals[0]['seconds']-1
        result={'case':name,'input_bytes':source.stat().st_size,'input_sha256':digest(source),'report_sha256':hashlib.sha256(reports[0]).hexdigest(),
                'artifact_sha256':digest(artifacts[0]),'budget':expected_budget,'profile':profile}
        rows.append(result);save(out/'results.json',{'status':'running','rows':rows,'full_own_source_run':False})
        print(json.dumps({'case':name,'active_samples':profile['active_samples'],'wall_overhead_fraction':profile['wall_overhead_fraction'],'top':profile['nearest_hua_frame'][:8]}),flush=True)
        assert abs(profile['wall_overhead_fraction'])<.2,'STOP: sample perturbation exceeds 20%; no extrapolation'
    save(out/'results.json',{'status':'passed','rows':rows,'full_own_source_run':False})


def compare(root,out,candidate):
    m=verify(out);prior=json.loads((out/'results.json').read_text(encoding='utf-8'));assert prior['status']=='passed'
    assert digest(candidate)!=m['tools']['native']['sha256'], 'candidate is the unchanged baseline'
    target=out/'lookup-candidate';target.mkdir();rows=[]
    data={'status':'running','runtime_sha256':digest(candidate),'native_text_sha256':pe_text(candidate),'rows':rows,
          'changed_source_sha256':digest(root/'src/stdlib.cpp'),'full_own_source_run':False}
    save(target/'results.json',data)
    for old in prior['rows']:
        name=old['case'];source=out/'inputs'/(name+'.hua');expected_report=(out/'commands'/(name+'-before.stdout')).read_bytes()
        normal=[]
        for label in ('control','normal','sampled','after'):
            native=Path(m['tools']['native']['path']) if label=='control' else candidate
            artifact=target/(name+'-'+label+'.huab')
            args=[native,'run',out/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,artifact]
            if label=='sampled':
                trace=target/(name+'-samples.json')
                command(root,out,'lookup-'+name+'-'+label,[Path(m['tools']['sampler']['path']),trace,'8','30000',*args],35)
                report=Path(str(trace)+'.stdout').read_bytes();stderr=Path(str(trace)+'.stderr').read_bytes()
                profile=symbolize(root,out,trace,native,Path(m['tools']['symbolizer']['path']),name)
            else:
                row,report,stderr=command(root,out,'lookup-'+name+'-'+label,args)
                if label=='control':control=row
                else:normal.append(row)
            assert report==expected_report and json.loads(stderr)==old['budget'],(name,label)
            assert artifact.read_bytes()==(out/(name+'-before.huab')).read_bytes(),(name,label)
        wall=statistics.mean(r['seconds'] for r in normal);cpu=statistics.mean(r['cpu_seconds'] for r in normal)
        row={'case':name,'control':control,'normal':normal,'profile':profile,'wall_seconds':wall,'cpu_seconds':cpu,
             'wall_change_fraction':wall/control['seconds']-1,'cpu_change_fraction':cpu/control['cpu_seconds']-1,
             'sample_wall_overhead_fraction':profile['wall_seconds']/wall-1,'report_sha256':old['report_sha256'],'artifact_sha256':old['artifact_sha256']}
        rows.append(row);save(target/'results.json',data)
        print(json.dumps({'case':name,'wall_before':control['seconds'],'wall_after':wall,'cpu_before':control['cpu_seconds'],'cpu_after':cpu,
                          'active_samples':profile['active_samples'],'top':profile['nearest_hua_frame'][:6]}),flush=True)
    data['status']='passed';save(target/'results.json',data)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('init','calibrate','prepare','measure','compare'))
    parser.add_argument('--output',type=Path,required=True);parser.add_argument('--native',type=Path)
    parser.add_argument('--sampler',type=Path);parser.add_argument('--calibration',type=Path);parser.add_argument('--symbolizer',type=Path)
    parser.add_argument('--phase-probe',type=Path);parser.add_argument('--host-timeout',type=int,default=90)
    parser.add_argument('--candidate',type=Path)
    a=parser.parse_args();root=Path(__file__).resolve().parents[1];out=a.output.resolve()
    assert out.is_relative_to(root/'build') and out!=root/'build'
    sys.path.insert(0,str(root/'tests'))
    try:
        if a.mode=='init':
            assert all((a.native,a.sampler,a.calibration,a.symbolizer))
            initialize(root,out,*[p.resolve() for p in (a.native,a.sampler,a.calibration,a.symbolizer)])
        elif a.mode=='prepare':
            assert a.phase_probe
            prepare(root,out,a.phase_probe.resolve(),a.host_timeout)
        elif a.mode=='compare':
            assert a.candidate
            compare(root,out,a.candidate.resolve())
        else:(calibrate if a.mode=='calibrate' else measure)(root,out)
    except Exception as error:
        # Keep successful/failed commands and partial output; never start another child.
        if out.exists():
            failure=out/(a.mode+'-failure.json')
            if not failure.exists():save(failure,{'status':'stopped','mode':a.mode,'error':str(error),'retry':False,'full_own_source_run':False})
        raise


if __name__=='__main__':main()
