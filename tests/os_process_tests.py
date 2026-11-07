# HUA-D038 P1: os.environ, os.parse_args and std.process (since 28). The child processes
# are the test's own Python, so behaviour is checked end to end on real processes.
import os,subprocess,sys,time
from pathlib import Path
from three_path import Cases
args=sys.argv[1:]
py=sys.executable.replace('\\','/')
def q(text):return text.replace('\\','\\\\').replace('"','\\"').replace('\n','\\n')
with Cases(*args,'hua-process-') as c:
    # parse_args
    artifact=c.same('''import std.os as os
fn show(m map[string][]string) {
    for k, v in m { print("[" + k + "]", v) }
}
fn main() {
    let flags = ["verbose", "v"]
    let options = ["name", "n"]
    show(unwrap(os.parse_args(["a", "--verbose", "--name=x", "-n", "y", "--name", "z", "b", "--", "--verbose", "-n"], flags, options)))
    show(unwrap(os.parse_args([], flags, options)))
    show(unwrap(os.parse_args(["-", "--name", "--verbose", "-v", "-v"], flags, options)))
    show(unwrap(os.parse_args(["--name="], flags, options)))
    print(os.parse_args(["--what"], flags, options))
    print(os.parse_args(["-x"], flags, options))
    print(os.parse_args(["-1"], flags, options))
    print(os.parse_args(["-vn"], flags, options))
    print(os.parse_args(["--name"], flags, options))
    print(os.parse_args(["-n"], flags, options))
    print(os.parse_args(["--verbose=1"], flags, options))
    print(os.parse_args(["-n=1"], flags, options))
}
''','''[] [a, b, --verbose, -n]
[n] [y]
[name] [x, z]
[verbose] [true]
[] []
[] [-]
[name] [--verbose]
[v] [true]
[] []
[name] []
err(ARGS_UNKNOWN: unknown argument --what)
err(ARGS_UNKNOWN: unknown argument -x)
err(ARGS_UNKNOWN: unknown argument -1)
err(ARGS_UNKNOWN: unknown argument -vn)
err(ARGS_VALUE: missing value for --name)
err(ARGS_VALUE: missing value for -n)
err(ARGS_VALUE: flag takes no value: --verbose=1)
err(ARGS_UNKNOWN: unknown argument -n=1)
''')
    c.header(artifact,28)
    c.fails('import std.os as os\nfn main() { print(os.parse_args([], ["x"], ["x"])) }\n','E4003')
    # environ
    os.environ['HUA_TEST_ENV_VALUE']='值 = 1'
    c.same('''import std.os as os
fn main() {
    let e = unwrap(os.environ())
    print(unwrap(e["HUA_TEST_ENV_VALUE"]), has(e, "HUA_TEST_ENV_MISSING"), len(e) > 2)
    print(unwrap(os.getenv("HUA_TEST_ENV_VALUE")) == unwrap(e["HUA_TEST_ENV_VALUE"]))
}
''','值 = 1 false true\ntrue\n')
    # run: stdout, stderr, exit code, stdin, binary output and argument fidelity.
    child=c.base/'child.py'
    child.write_text('''import os,sys,time
mode=sys.argv[1]
sys.stdout.reconfigure(encoding="utf-8")
b=sys.stdout.buffer
if mode=="echo":b.write(sys.stdin.buffer.read())
elif mode=="args":print("\\n".join(repr(x) for x in sys.argv[2:]))
elif mode=="code":sys.exit(int(sys.argv[2]))
elif mode=="both":b.write(b"out\\xff\\x00");sys.stderr.buffer.write(b"err\\xfe")
elif mode=="flood":
    chunk=b"x"*65536
    for _ in range(int(sys.argv[2])):b.write(chunk);sys.stderr.buffer.write(chunk)
elif mode=="sleep":b.write(b"before");b.flush();time.sleep(60)
elif mode=="spawn_sleep":
    import subprocess
    p=subprocess.Popen([sys.executable,__file__,"sleep"]);open(sys.argv[2],"w").write(str(p.pid));time.sleep(60)
elif mode=="daemon":
    import subprocess
    subprocess.Popen([sys.executable,__file__,"sleep"]);b.write(b"parent done")
elif mode=="cwd":print(os.getcwd())
elif mode=="env":print(os.environ.get("HUA_CHILD","<none>"),os.environ.get("HUA_TEST_ENV_VALUE","<none>"),"PATH" in os.environ)
''',encoding='utf-8',newline='\n')
    head=f'import std.process as process\nimport std.bytes as bytes\nfn text(b Bytes) string {{ return unwrap(bytes.to_text(b)) }}\nfn main() {{\n    let none = bytes.from_text("")\n    let py = "{py}"\n    let child = "child.py"\n'
    body='''    let c1, o1, e1, p1 = process.run([py, child, "echo"], bytes.from_text("hello 中文"), 0)
    print(c1, text(o1), "[" + text(e1) + "]", "[" + p1 + "]")
    let c2, o2, e2, p2 = process.run([py, child, "code", "7"], none, 0)
    print(c2, bytes.len(o2), bytes.len(e2), "[" + p2 + "]")
    let c3, o3, e3, p3 = process.run([py, child, "both"], none, 0)
    print(c3, unwrap(bytes.to_ints(o3)), unwrap(bytes.to_ints(e3)))
    let c4, o4, e4, p4 = process.run([py, child, "flood", "200"], none, 0)
    print(c4, bytes.len(o4), bytes.len(e4), "[" + p4 + "]")
    let large = bytes.from_text("0123456789abcdef")
    var big = large
    for i in 0..18 { big = unwrap(bytes.concat(big, big)) }
    let c5, o5, e5, p5 = process.run([py, child, "echo"], big, 0)
    print(c5, bytes.len(o5) == bytes.len(big), "[" + p5 + "]")
}
'''
    results,_=c.three(head+body)
    expected='0 hello 中文 [] []\n7 0 0 []\n0 [111, 117, 116, 255, 0] [101, 114, 114, 254]\n0 13107200 13107200 []\n0 true []\n'
    assert all(o==expected for o,_ in results),results
    # Arguments reach the child unchanged: spaces, quotes, backslashes, empty, UTF-8.
    tricky=['plain','with space','','a"b','a\\b','a\\\\','tail\\ ','"','\\"','a b\\','中 文','--flag=1','%PATH%','&|<>^']
    literal=', '.join('"'+q(x)+'"' for x in tricky)
    results,_=c.three(head+f'    let cc, oo, ee, pp = process.run([py, child, "args", {literal}], none, 0)\n    print(cc, "[" + pp + "]")\n    print(text(oo))\n}}\n')
    want='0 []\n'+'\n'.join(repr(x) for x in tricky)+'\n\n'
    assert all(o.replace('\r\n','\n')==want for o,_ in results),(want,results)
    # Start failure, argv errors, timeout with partial output, and killing the tree.
    results,_=c.three(head.replace('import std.bytes as bytes','import std.bytes as bytes\nimport std.strings as strings')+'''    let c1, o1, e1, p1 = process.run(["hua-no-such-program-xyz"], none, 0)
    print(c1, bytes.len(o1), bytes.len(e1), strings.starts_with(p1, "PROCESS_START: "))
    let c2, o2, e2, p2 = process.run([py, child, "sleep"], none, 300)
    print(c2, text(o2), p2)
}
''')
    assert all(o=='-1 0 0 true\n-1 before PROCESS_TIMEOUT: time limit exceeded\n' for o,_ in results),results
    pidfile=c.base/'grandchild.pid'
    start=time.time()
    results=c.same(head+f'    let c1, o1, e1, p1 = process.run([py, child, "spawn_sleep", "grandchild.pid"], none, 2000)\n    print(c1, p1)\n}}\n','-1 PROCESS_TIMEOUT: time limit exceeded\n')
    grandchild=int(pidfile.read_text())
    alive=subprocess.run(['tasklist','/FI',f'PID eq {grandchild}','/NH'],capture_output=True,text=True,errors='replace').stdout if os.name=='nt' else ''
    assert str(grandchild) not in alive,('grandchild survived the timeout',alive)
    # A descendant left behind cannot hold the pipes open past the child's exit.
    start=time.time()
    c.same(head+'    let c1, o1, e1, p1 = process.run([py, child, "daemon"], none, 0)\n    print(c1, text(o1), "[" + p1 + "]")\n}\n','0 parent done []\n')
    assert time.time()-start<30,'run waited for a descendant'
    # Output limit: each stream stops at 128 MiB.
    results,_=c.three(head+'    let c1, o1, e1, p1 = process.run([py, child, "flood", "2100"], none, 0)\n    print(c1, bytes.len(o1) <= 134217728, p1)\n}\n')
    assert all(o=='-1 true PROCESS_OUTPUT_LIMIT: output exceeds 128 MiB\n' for o,_ in results),results
    # run_with: cwd, environment overlay and invalid entries.
    sub=c.base/'sub dir';sub.mkdir()
    results,_=c.three(head+f'''    let c1, o1, e1, p1 = process.run_with([py, "../child.py", "cwd"], none, 0, "sub dir", map[string]string{{}})
    print(c1, text(o1))
    let c2, o2, e2, p2 = process.run_with([py, child, "env"], none, 0, "", map[string]string{{"HUA_CHILD": "中 1"}})
    print(c2, text(o2))
    let c3, o3, e3, p3 = process.run_with([py, child, "env"], none, 0, "", map[string]string{{"HUA_TEST_ENV_VALUE": "over"}})
    print(text(o3))
    let c4, o4, e4, p4 = process.run_with([py, child, "cwd"], none, 0, "no such dir", map[string]string{{}})
    print(c4, bytes.len(o4))
    let c5, o5, e5, p5 = process.run_with([py, child, "cwd"], none, 0, "", map[string]string{{"A=B": "1"}})
    print(c5, p5)
}}
''')
    want=f'0 {sub}\n\n0 中 1 值 = 1 True\n\n<none> over True\n\n-1 0\n-1 PROCESS_START: invalid environment entry\n'
    assert all(o.replace('\r\n','\n')==want for o,_ in results),(want,results)
    # In a Task: the worker stays free for others, and cancellation kills the whole tree.
    pidfile=c.base/'canceled.pid'
    start=time.time()
    results,_=c.three(f'''import std.process as process
import std.bytes as bytes
import std.task as task
fn work() int {{
    let code, o, e, p = process.run(["{py}", "child.py", "spawn_sleep", "canceled.pid"], bytes.from_text(""), 0)
    return code
}}
fn quick() int {{
    let code, o, e, p = process.run(["{py}", "child.py", "code", "9"], bytes.from_text(""), 0)
    return code
}}
async fn main() {{
    print(await spawn quick())
    let t = spawn work()
    print(is_err(task.timeout(t, 3000)))
}}
''')
    assert all(o=='9\ntrue\n' for o,_ in results),results
    assert time.time()-start<120,'cancellation did not stop the child'
    alive=subprocess.run(['tasklist','/FI',f'PID eq {int(pidfile.read_text())}','/NH'],capture_output=True,text=True,errors='replace').stdout if os.name=='nt' else ''
    assert pidfile.read_text() not in alive,('descendant survived cancellation',alive)
    # Programming errors.
    c.fails('import std.process as process\nimport std.bytes as bytes\nfn main() {\n    let e []string = []\n    let a, b, x, y = process.run(e, bytes.from_text(""), 0)\n}\n','E4003')
    c.fails('import std.process as process\nimport std.bytes as bytes\nfn main() {\n    let a, b, x, y = process.run(["x"], bytes.from_text(""), -1)\n}\n','E4003')
    for body,error in [('let a, b, x, y = process.run("x", bytes.from_text(""), 0)','E3004'),('let a, b, x, y = process.run(["x"], "in", 0)','E3004'),('let a, b, x, y = process.run(["x"], bytes.from_text(""), "0")','E3004'),('let a = process.run(["x"], bytes.from_text(""), 0)','E3006'),('let a, b, x, y = process.run_with(["x"], bytes.from_text(""), 0, 1, map[string]string{})','E3004'),('let a, b, x, y = process.run_with(["x"], bytes.from_text(""), 0, "", map[string]int{})','E3004'),('let a, b, x, y = process.run(["x"])','E3005')]:
        c.static('import std.process as process\nimport std.bytes as bytes\nfn main() {\n    '+body+'\n}\n',error)
    for body,error in [('os.parse_args(1, [], [])','E3004'),('os.parse_args([], [1], [])','E3004'),('os.environ(1)','E3005')]:
        c.static('import std.os as os\nfn main() { print('+body+') }\n',error)
print(f'os/process checks passed: {c.checks}')
