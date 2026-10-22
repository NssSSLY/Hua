"""C04 test-only phase supervision; native prepare and execution stay bounded."""
import os,subprocess,time
from pathlib import Path

PHASES=['prepare-begin','prepare-end','AST-begin','AST-end','VM-begin','VM-end']

def execute(command,trace,output,error,limit,suite_deadline,prepare_limit=None):
    assert 0<limit<=60
    if prepare_limit is None:prepare_limit=limit
    assert 0<prepare_limit<=120
    trace,output,error=map(Path,(trace,output,error))
    assert not any(p.exists() for p in (trace,output,error))
    started=time.monotonic();phase='prepare';phase_start=started;seen=0;last_stamp=0.;failure=None
    with output.open('wb') as out,error.open('wb') as err:
        process=subprocess.Popen(list(map(str,command)),stdout=out,stderr=err,**({'start_new_session':True} if os.name!='nt' else {}))
        try:
            while process.poll() is None:
                if trace.exists():
                    raw=trace.read_text(encoding='utf-8');lines=raw.splitlines()
                    if raw and not raw.endswith('\n'):lines=lines[:-1]
                    for line in lines[seen:]:
                        try:name,stamp=line.split();elapsed=float(stamp)
                        except ValueError:failure='invalid phase record';break
                        if seen>=len(PHASES) or name!=PHASES[seen]:failure='invalid phase order';break
                        if not last_stamp<=elapsed<=time.monotonic()-started+1:failure='invalid phase timestamp';break
                        last_stamp=elapsed;seen+=1
                        if name.endswith('-begin') or name=='VM-end':phase=name;phase_start=started+elapsed
                    if failure:break
                now=time.monotonic()
                if now>=suite_deadline:failure='suite deadline';break
                if now-phase_start>=(prepare_limit if phase in ("prepare","prepare-begin") else limit):failure=phase+' phase deadline';break
                if output.stat().st_size>16777216:failure='report size';break
                time.sleep(.05)
        finally:
            if process.poll() is None:
                if os.name=='nt':subprocess.run(['taskkill','/F','/T','/PID',str(process.pid)],capture_output=True,timeout=5)
                else:
                    import signal
                    os.killpg(process.pid,signal.SIGKILL)
                if process.poll() is None:process.kill()
            process.wait(timeout=5)
        if failure:raise RuntimeError('HUA_TEST_STOP '+failure+'; no retry; trace='+str(trace))
        code=process.returncode
    result=subprocess.CompletedProcess(list(map(str,command)),code,output.read_text(encoding='utf-8'),error.read_text(encoding='utf-8'))
    if code==0:
        final=trace.read_text().splitlines() if trace.exists() else []
        if [line.split()[0] for line in final]!=PHASES:raise RuntimeError('incomplete prepared phase trace')
        stamps=[float(line.split()[1]) for line in final]
        if any(b<a for a,b in zip(stamps,stamps[1:])) or any(stamps[end]-stamps[begin]>allowed for begin,end,allowed in [(0,1,prepare_limit),(2,3,limit),(4,5,limit)]):raise RuntimeError('HUA_TEST_STOP completed phase exceeded deadline; no retry')
    return result,{'seconds':round(time.monotonic()-started,3),'phase_seconds_limit':limit,'prepare_seconds_limit':prepare_limit,'trace':str(trace),'returncode':code,'retry':False}
