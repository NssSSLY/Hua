# Optional CPython bridge: interpreter, VM and relocated source-free HUAB.
from pathlib import Path
import json, shutil, subprocess, sys, tempfile
exe, build, bundle, home = (Path(p).resolve() for p in sys.argv[1:5])
checks = 0

def run(command, path, *extra, cwd=None, expected=None, code=0, error=None):
    global checks
    p = subprocess.run([str(exe), command, str(path), *map(str, extra)],
                       cwd=cwd, capture_output=True, timeout=60)
    out = p.stdout.decode('utf-8').replace('\r\n', '\n'); err = p.stderr.decode('utf-8')
    assert p.returncode == code, (command, path, p.returncode, out, err)
    if expected is not None: assert out == expected, (command, out, expected, err)
    if error: assert error in err, (command, out, err)
    checks += 1
    return out

with tempfile.TemporaryDirectory(prefix='hua-python-', dir=build) as temp:
    base = Path(temp); source = base/'source'; shutil.copytree(bundle, source)
    work = base/'work'; work.mkdir(); (work/'cwd_fixture.py').write_text('value=1\n')
    packages = base/'packages中文'; packages.mkdir()
    marker = work/'imported.txt'
    (packages/'bridge_fixture.py').write_text("import pathlib\npathlib.Path(" + repr(str(marker)) + ").write_text('imported')\n" + '''
class Counter:
    def __init__(self, value): self.value = value
    def add(self, step=1): self.value += step; return self.value
    def __str__(self): return 'Counter:' + str(self.value)
    @property
    def broken(self): raise ValueError('property failed')
def echo(value): return value
def combine(a, b=0): return a+b
def explode(): raise ValueError('中文 error')
def stop(): raise SystemExit(5)
def big(): return 2**63
def nan(): return float('nan')
def raw(): return b'abc'
def cycle():
    value=[]; value.append(value); return value
def wrong_keys(): return {1: 'a'}
def deep():
    value=0
    for i in range(66): value=[value]
    return value
def tuple_value(): return (1, '中', None)
''', encoding='utf-8')
    n=0
    prefix='import python as py\nimport std.json as json\nimport python_native as native\n'
    init='unwrap(py.init(""))\nunwrap(py.add_path('+json.dumps(str(packages),ensure_ascii=False)+'))\n'
    def triple(text, expected, with_init=True):
        global n, checks
        n+=1
        directory=base/f'case{n}'; shutil.copytree(source,directory)
        path=directory/'main.hua'; path.write_text(prefix+(init if with_init else '')+text,encoding='utf-8')
        marker.unlink(missing_ok=True)
        run('check',path,expected='OK\n')
        archive=directory/'main.huab'; run('build',path,'-o',archive)
        assert not marker.exists(), 'check/build executed Python'; checks+=1
        for command in ['interpret','run']: run(command,path,cwd=work,expected=expected)
        for p in directory.glob('*.hua'): p.unlink()
        relocated=base/f'relocated{n}'; directory.rename(relocated); artifact=relocated/'main.huab'
        assert int.from_bytes(artifact.read_bytes()[8:12],'little') == 6; checks+=1
        run('check',artifact,expected='OK\n'); run('run',artifact,cwd=work,expected=expected)
        return relocated
    triple(r'''let m=unwrap(py.import_module("bridge_fixture"))
let echo=unwrap(py.getattr(m,"echo"))
let value=unwrap(py.from_value(unwrap(json.decode("{\"a\":[1,null,true],\"text\":\"中文\\u0000\"}"))))
let copied=unwrap(py.call(echo,[value]))
print(unwrap(json.encode(unwrap(py.to_json(copied)))))
let combine=unwrap(py.getattr(m,"combine"))
let one=unwrap(py.from_value(1))
let two=unwrap(py.from_value(2))
print(unwrap(py.to_string(unwrap(py.call_kw(combine,[one],map[string]int{"b":two})))))
let cls=unwrap(py.getattr(m,"Counter"))
let counter=unwrap(py.call(cls,[one]))
let add=unwrap(py.getattr(counter,"add"))
print(unwrap(py.to_string(unwrap(py.call(add,[])))),unwrap(py.to_string(counter)))
print(unwrap(json.as_int(unwrap(py.to_json(unwrap(py.getattr(counter,"value")))))),is_err(py.to_json(counter)))
print(is_err(py.getattr(counter,"broken")),is_err(py.call(one,[])))
print(is_err(py.getattr(m,"missing")),is_err(py.call(combine,[])),is_err(py.call_kw(combine,[one],map[string]int{"bad":two})))
print(unwrap(py.release(one)),unwrap(py.release(one)),is_err(py.to_string(one)))
print(unwrap(py.shutdown()),is_err(py.version()),is_err(py.init("")))
''','{"a":[1,null,true],"text":"中文\\u0000"}\n3\n2 Counter:2\n2 true\ntrue true\ntrue true true\ntrue false true\ntrue true true\n')
    triple('''let m=unwrap(py.import_module("bridge_fixture"))
for name in ["big","nan","raw","cycle","wrong_keys","deep"] {
    let f=unwrap(py.getattr(m,name))
    let value=unwrap(py.call(f,[]))
    print(name,is_err(py.to_json(value)))
    unwrap(py.release(value))
    unwrap(py.release(f))
}
let tuple_fn=unwrap(py.getattr(m,"tuple_value"))
print(unwrap(json.encode(unwrap(py.to_json(unwrap(py.call(tuple_fn,[])))))))
for name in ["explode","stop"] {
    let f=unwrap(py.getattr(m,name))
    print(unwrap_err(py.call(f,[])))
    unwrap(py.release(f))
}
print(is_err(py.import_module("hua_no_such_package")),is_err(py.import_module(".relative")),is_err(py.add_path("missing-directory")))
unwrap(py.shutdown())
''','big true\nnan true\nraw true\ncycle true\nwrong_keys true\ndeep true\n[1,"中",null]\nValueError: 中文 error\nSystemExit: 5\ntrue true true\n')
    triple('''print(is_err(py.version()),is_err(py.init("missing-python-home")))
print(unwrap(py.init(HOME)),unwrap(py.init("")),is_err(py.import_module("cwd_fixture")))
let math=unwrap(py.import_module("math"))
let sqrt=unwrap(py.getattr(math,"sqrt"))
let input=unwrap(py.from_value(81))
let output=unwrap(py.call(sqrt,[input]))
print(unwrap(json.as_float(unwrap(py.to_json(output)))))
print(unwrap(py.active_handles()))
for id in [math,sqrt,input,output] { unwrap(py.release(id)) }
print(unwrap(py.active_handles()),is_err(py.release(0)),is_err(py.release(-1)))
unwrap(py.shutdown())
'''.replace('HOME',json.dumps(str(home))),'true true\ntrue true true\n9\n4\n0 true true\n',with_init=False)
    triple(r'''fn failed(raw string) bool { return !unwrap(json.as_bool(unwrap(json.get(unwrap(json.decode(native.request(raw))),"ok")))) }
for raw in ["not json","[]","{}","{\"op\":\"unknown\"}","{\"op\":\"from_json\",\"value\":NaN}","{\"op\":\"from_json\",\"value\":9223372036854775808}","{\"op\":\"release\",\"target\":true}","{\"op\":\"release\",\"target\":\"1\"}","{\"op\":\"to_json\",\"target\":123456}","{\"op\":\"from_json\"}"] { print(failed(raw)) }
unwrap(py.shutdown())
''','true\n'*10)
    triple(r'''for i in 0..4096 { native.request("{\"op\":\"from_json\",\"value\":null}") }
print(unwrap(py.active_handles()),is_err(py.from_value(0)))
print(unwrap(py.release(1)),unwrap(py.from_value(0)),unwrap(py.active_handles()))
unwrap(py.shutdown())
''','4096 true\ntrue 4097 4096\n')
    oversized_requests=[json.dumps({'op':'call','target':3,'args':[1]*1025,'kwargs':{}}),
                        json.dumps({'op':'call','target':3,'args':[],'kwargs':{f'k{i}':1 for i in range(1025)}})]
    triple('let one=unwrap(py.from_value(1))\nlet builtins=unwrap(py.import_module("builtins"))\nlet sum=unwrap(py.getattr(builtins,"sum"))\n'+
           ''.join('print(unwrap(json.as_string(unwrap(json.get(unwrap(json.decode(native.request('+json.dumps(raw)+'))),"error")))))\n' for raw in oversized_requests)+
           'unwrap(py.shutdown())\n', 'Python call argument limit exceeded (1024)\n'*2)
    # Optional real third-party package proof; never downloads dependencies during CTest.
    if len(sys.argv) > 5:
        package_dir=Path(sys.argv[5]).resolve()
        triple('unwrap(py.add_path('+json.dumps(str(package_dir))+'))\n'+"""let module=unwrap(py.import_module("packaging.version"))
let constructor=unwrap(py.getattr(module,"Version"))
let text=unwrap(py.from_value("1.2.3"))
let version=unwrap(py.call(constructor,[text]))
let major=unwrap(py.getattr(version,"major"))
print(unwrap(py.to_string(version)),unwrap(json.as_int(unwrap(py.to_json(major)))))
for handle in [major,version,text,constructor,module] { unwrap(py.release(handle)) }
print(unwrap(py.active_handles()))
unwrap(py.shutdown())
""", '1.2.3 1\n0\n')
    missing=base/'missing'; missing.mkdir()
    for name in ['python.hua','python_native.huam','python_native.dll']: shutil.copy2(source/name,missing/name)
    path=missing/'main.hua'; path.write_text(prefix+'print(is_err(py.init("")))\n',encoding='utf-8')
    run('check',path,expected='OK\n'); run('build',path,'-o',missing/'main.huab')
    (missing/'python_native.dll').unlink()
    run('run',path,code=1,error='E7001'); run('interpret',path,code=1,error='E7001')
    path.unlink(); (missing/'python.hua').unlink()
    run('run',missing/'main.huab',code=1,error='E7001')
print(f'{checks} optional Python bridge checks passed')
