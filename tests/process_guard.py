"""Fail-fast supervision for self-host compiler test subprocesses."""
import json, os, signal, subprocess, sys, time
from unittest.mock import patch
_native_run = subprocess.run
_suite_started = time.monotonic()

class TestProcessTimeout(RuntimeError):
    pass

def run(*args, **kwargs):
    cap = float(os.environ.get('HUA_TEST_TIMEOUT_SECONDS', '30'))
    if not 0 < cap <= 60:
        raise ValueError('HUA_TEST_TIMEOUT_SECONDS must be in (0, 60]')
    suite_cap=float(os.environ.get('HUA_TEST_SUITE_TIMEOUT_SECONDS','900'))
    if not 0 < suite_cap <= 900:raise ValueError('HUA_TEST_SUITE_TIMEOUT_SECONDS must be in (0, 900]')
    remaining=suite_cap-(time.monotonic()-_suite_started)-min(10,suite_cap/10) # reserve cleanup time
    if remaining<=0:raise TestProcessTimeout('HUA_TEST_STOP suite deadline; no further subprocesses')
    cap=min(cap,remaining)
    requested=kwargs.pop('timeout',None)
    limit=min(cap,requested) if requested is not None else cap
    capture=kwargs.pop('capture_output',False);check=kwargs.pop('check',False)
    data=kwargs.pop('input',None)
    if capture:
        if 'stdout' in kwargs or 'stderr' in kwargs:raise ValueError('capture_output with explicit output')
        kwargs['stdout']=subprocess.PIPE;kwargs['stderr']=subprocess.PIPE
    if data is not None:
        if 'stdin' in kwargs:raise ValueError('input with explicit stdin')
        kwargs['stdin']=subprocess.PIPE
    if os.name!='nt':kwargs.setdefault('start_new_session',True)
    started=time.monotonic()
    with subprocess.Popen(*args,**kwargs) as process:
        try: stdout,stderr=process.communicate(data,timeout=limit)
        except subprocess.TimeoutExpired:
            if os.name=='nt':
                try:_native_run(['taskkill','/F','/T','/PID',str(process.pid)],capture_output=True,timeout=5)
                except (OSError,subprocess.TimeoutExpired):pass
            else:os.killpg(process.pid,signal.SIGKILL)
            if process.poll() is None:process.kill()
            stdout,stderr=process.communicate()
            report={'status':'timeout','seconds':round(time.monotonic()-started,3),
                    'limit_seconds':limit,'command':list(map(str,process.args)),
                    'retry':False,'stdout_bytes':len(stdout or ''),'stderr_bytes':len(stderr or '')}
            message='HUA_TEST_STOP '+json.dumps(report,ensure_ascii=False)
            print(message,file=sys.stderr,flush=True)
            # Do not let the frontend convert a test timeout into an expected source error.
            raise TestProcessTimeout(message)
        result=subprocess.CompletedProcess(process.args,process.returncode,stdout,stderr)
        if check:result.check_returncode()
        return result

def guarded_main(module, args):
    # Preserve deliberately mocked protocol/fault tests; supervise real in-process CLI calls.
    if module.subprocess.run is not _native_run:return module.main(args)
    with patch.object(module.subprocess,'run',side_effect=run):return module.main(args)
