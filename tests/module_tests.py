"""Module + VM contracts exercised through the actual CLI and both execution engines."""
from pathlib import Path
import subprocess
import sys
import tempfile

exe, root = Path(sys.argv[1]), Path(sys.argv[2])
checks = 0

def run(command, path, code=0, output=None, error=None, silent=False):
    global checks
    result = subprocess.run([str(exe), command, str(path)], capture_output=True,
                            encoding="utf-8", timeout=12)
    assert result.returncode == code, (command, path, result.returncode, result.stdout, result.stderr)
    if output is not None:
        assert result.stdout == output, (command, result.stdout, output, result.stderr)
    if error is not None:
        assert error in result.stderr, (command, result.stderr, error)
        assert "^" in result.stderr, result.stderr
    if silent:
        assert not result.stdout, result.stdout
    checks += 1
    return result

with tempfile.TemporaryDirectory(prefix="hua-modules-", dir=root / "build") as temp:
    base = Path(temp)
    entry = base / "鍏ュ彛 with spaces.hua"

    def fixture(files):
        # Every test gets an isolated subtree to avoid resolver tests depending on older files.
        directory = base / str(checks)
        directory.mkdir()
        for name, text in files.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
        return directory / "main.hua"

    def success(files, output):
        path = fixture(files)
        run("check", path, output="OK\n")
        for command in ("run", "interpret"):
            run(command, path, output=output)
        return path

    def failure(files, code, fragment, origin="main.hua"):
        path = fixture(files)
        for command in ("check", "run", "interpret"):
            result = run(command, path, 1, error=f"error[{code}]", silent=True)
            assert fragment in result.stderr, result.stderr
            assert origin in result.stderr, result.stderr

    path = success({
        "main.hua": "import arithmetic as m\nfn main(){print(m.add(3,4),m.quotient(7,2))}\n",
        "arithmetic.hua": "pub fn add(a int,b int) int{return a+b}\npub fn quotient(a int,b int) int{return a//b}\n",
    }, "7 3\n")
    dump = run("bytecode", path)
    assert "CALL" in dump.stdout and "BINARY" in dump.stdout and "arithmetic.hua" in dump.stdout
    assert "->" not in dump.stdout  # Straight-line fixture.

    success({
        "main.hua": "import pkg.values\nimport pkg.ops as op\nprint(pkg.values.answer(),op.double(3))\n",
        "pkg/values.hua": "pub fn answer() int{return 42}\n",
        "pkg/ops/package.hua": "pub fn double(x int) int{return x*2}\n",
    }, "42 6\n")

    success({
        "main.hua": "import state as a\nimport state as b\nprint(a.next(),b.next())\n",
        "state.hua": 'print("init")\nvar counter=0\npub fn next() int{counter+=1\nreturn counter}\nfn main(){print("must not run")}\n',
    }, "init\n1 2\n")

    success({
        "main.hua": "import left\nimport right\nprint(left.read(),right.read())\n",
        "left.hua": "import shared\npub fn read() int{return shared.next()}\n",
        "right.hua": "import shared as s\npub fn read() int{return s.next()}\n",
        "shared.hua": 'print("shared")\nvar x=0\npub fn next() int{x+=1\nreturn x}\n',
    }, "shared\n1 2\n")

    success({
        "main.hua": "import data as d\nfn pass(x d.Point) d.Point{return x}\nvar p=d.Point{x:2,y:3}\np.shift(4)\nprint(p.sum(),pass(p).sum(),type(p))\n",
        "data.hua": "pub struct Point{x int\ny int}\npub fn Point.sum() int{return self.x+self.y}\npub fn Point.shift(x int){self.x+=x}\n",
    }, "9 9 Point\n")

    success({
        "main.hua": "import a\nimport b\nprint(a.value(),b.value())\nfn print(x int,y int){str(x)\nstr(y)}\nfn main(){}\n",
        "a.hua": 'var count=10\npub fn value() int{print("a")\nreturn count}\n',
        "b.hua": 'var count=20\npub fn value() int{print("b")\nreturn count}\n',
    }, "a\nb\n")

    success({
        "main.hua": "import funcs as m\nfn local(m int) int{return m+1}\nlet f=m.inc\nprint(f(5),local(9))\n",
        "funcs.hua": "pub fn inc(x int) int{return x+1}\n",
    }, "6 10\n")

    success({
        "main.hua": "import pkg\nimport pkg.package as alias\nprint(pkg.value(),alias.value())\n",
        "pkg/package.hua": 'print("once")\npub fn value() int{return 7}\n',
    }, "once\n7 7\n")

    success({
        "main.hua": "import m\nprint(m.read())\n",
        "m.hua": "pub fn read() int{return 1}\n",
        "m/package.hua": "pub fn read() int{return 2}\n",
    }, "1\n")

    success({
        "main.hua": "import box\nvar b=box.Box{values:[1,2]}\nbox.set(b.values)\nprint(b.values)\n",
        "box.hua": "pub struct Box{values []int}\npub fn set(mut values []int){values[0]=9}\n",
    }, "[9, 2]\n")

    failure({"main.hua": 'import missing\nprint("must not run")\n'}, "E5001", "cannot resolve module")
    failure({"main.hua": 'import a\nprint("must not run")\n', "a.hua": "import b\n", "b.hua": "import a\n"}, "E5002", "cyclic module import", "b.hua")
    failure({"main.hua": 'import m\nprint("must not run")\nprint(m.hidden())\n', "m.hua": "fn hidden() int{return 1}\n"}, "E5003", "no public export")
    failure({"main.hua": "import m\nprint(m.nope())\n", "m.hua": "pub fn yes() int{return 1}\n"}, "E5003", "no public export")
    failure({"main.hua": "import m\nlet p=m.Point{x:1}\nprint(p.secret())\n", "m.hua": "pub struct Point{x int}\nfn Point.secret() int{return self.x}\n"}, "E5003", "method is private")
    failure({"main.hua": "import m\nlet p=m.Point{x:1}\np.change()\n", "m.hua": "pub struct Point{x int}\npub fn Point.change(){self.x=2}\n"}, "E3003", "mutable receiver")
    failure({"main.hua": "import m as x\nimport m as x\n", "m.hua": ""}, "E5004", "duplicate import namespace")
    failure({"main.hua": "import m\nlet m=1\n", "m.hua": ""}, "E5004", "conflicts with declaration")
    failure({"main.hua": 'print("must not run")\nimport m\n', "m.hua": ""}, "E5004", "imports must precede")
    failure({"main.hua": "fn main(){import m}\n", "m.hua": ""}, "E5004", "only at the start")
    failure({"main.hua": 'import m\nprint("must not run")\n', "m.hua": "let x=unknown\n"}, "E3001", "undefined name", "m.hua")
    failure({"main.hua": 'import m\nprint("must not run")\n', "m.hua": "let =1\n"}, "E2001", "expected binding name", "m.hua")
    failure({"main.hua": "import m\nlet rootOnly=7\nprint(m.read())\n", "m.hua": "pub fn read() int{return rootOnly}\n"}, "E3001", "undefined name", "m.hua")
    failure({"main.hua": "import m\nprint(m.f(1))\n", "m.hua": "pub fn f(a int,b int) int{return a+b}\n"}, "E3005", "argument count mismatch")
    failure({"main.hua": "import m\nprint(m.f(true))\n", "m.hua": "pub fn f(a int) int{return a}\n"}, "E3004", "expected int, got bool")
    failure({"main.hua": "import m\nlet x m.f=1\n", "m.hua": "pub fn f() int{return 1}\n"}, "E5003", "not a struct type")
    failure({"main.hua": "import m\nlet alias=m\n", "m.hua": ""}, "E5003", "qualified export")
    failure({"main.hua": "import m\nlet a=m.A{x:1}\nlet b=m.B{x:2}\nprint(a==b)\n", "m.hua": "pub struct A{x int}\npub struct B{x int}\n"}, "E3004", "expected")

    path=fixture({"main.hua": 'import bad\nprint("root")\n', "bad.hua": 'print("before")\nprint(1/0)\n'})
    for command in ("run","interpret"):
        result=run(command,path,1,"before\n","error[E4004]")
        assert "bad.hua:2:" in result.stderr and "print(1/0)" in result.stderr

    path=fixture({"main.hua": "import dynamic\nlet value=dynamic.make()\nvalue.secret()\n", "dynamic.hua": "struct Hidden{x int}\nfn Hidden.secret(){print(1)}\npub fn make(){return Hidden{x:1}}\n"})
    run("check",path,output="OK\n")
    for command in ("run","interpret"):
        run(command,path,1,error="error[E5003]",silent=True)

    success({
        "main.hua": "import data as d\nfn keep(d d.Point) d.Point{return d}\nlet p=d.Point{x:3}\nprint(keep(p).x)\n",
        "data.hua": "pub struct Point{x int}\n",
    }, "3\n")
    success({
        "main.hua": "import m\nfn call() int {let m=7\nreturn m+1}\nprint(call(),m.value())\n",
        "m.hua": "pub fn value() int{return 9}\n",
    }, "8 9\n")
    failure({
        "main.hua": "import a\nimport b\nlet x=a.P{x:1}\nb.accept(x)\n",
        "a.hua": "pub struct P{x int}\n",
        "b.hua": "pub struct P{x int}\npub fn accept(p P){}\n",
    }, "E3004", "expected")
    # Module-count and single-file bounds independently fail before any output.
    files={"main.hua":"".join(f"import count{i}\n" for i in range(128))}
    files.update({f"count{i}.hua":"" for i in range(128)})
    failure(files,"E5005","module graph limit")
    path=fixture({"main.hua":"import tooLarge\n","tooLarge.hua":""})
    (path.parent/"tooLarge.hua").write_bytes(b" "*(16*1024*1024+1))
    run("check",path,1,error="exceeds 16 MiB",silent=True)
    # Windows case aliases map to the same canonical cache entry.
    if sys.platform=="win32":
        success({
            "main.hua":"import shared as a\nimport SHARED as b\nprint(a.read(),b.read())\n",
            "shared.hua":'print("once")\npub fn read() int{return 1}\n',
        },"once\n1 1\n")

    # Limits and invalid encodings are rejected before initialization.
    files={"main.hua":"import chain0\n"}
    for i in range(64): files[f"chain{i}.hua"]=f"import chain{i+1}\n"
    files["chain64.hua"]=""
    failure(files,"E5005","module graph limit","chain62.hua")

    path=fixture({"main.hua":"import m\n","m.hua":""})
    (path.parent/"m.hua").write_bytes(b'let x="\xc0\xaf"\n')
    result=run("run",path,1,error="error[E1009]",silent=True)
    assert "m.hua:1:" in result.stderr and "\\xC0" in result.stderr

    # Unicode entry paths and the process working directory do not change resolution.
    entry.write_text('import helper\nprint(helper.answer())\n',encoding="utf-8")
    (base/"helper.hua").write_text('pub fn answer() int{return 42}\n',encoding="utf-8")
    run("run",entry,output="42\n")

    # Branches, nested loops, return and source locations are visible in bytecode.
    path=fixture({"main.hua":"fn f() int{for i in 0..3 {if i==1 {return 7}}\nreturn 0}\nprint(f())\n"})
    for command in ("run","interpret"): run(command,path,output="7\n")
    result=run("bytecode",path)
    assert all(op in result.stdout for op in ("RANGE_INIT","ITER_NEXT","JUMP_FALSE","RETURN"," -> "))

print(f"{checks} VM/module CLI checks passed")
