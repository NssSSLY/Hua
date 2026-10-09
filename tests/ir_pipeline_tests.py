"""E1 independent HIR/MIR -> VM and source-free HUAB differential gate."""
import pathlib, re, struct, subprocess, sys, tempfile
hua, probe, repo, build = map(pathlib.Path, sys.argv[1:5])
checks = cases = 0

def run(exe, *args):
    global checks
    checks += 1
    return subprocess.run([str(exe), *map(str, args)], capture_output=True,
                          encoding="utf-8", timeout=35)

def outcome(p):
    code = re.search(r"error\[(E\d+)\]", p.stderr)
    return p.returncode, p.stdout, code.group(1) if code else None

def differential(folder, text, expected=None, diagnostic=None):
    global checks, cases
    cases += 1
    folder.mkdir()
    source = folder / "main.hua"
    source.write_text(text, encoding="utf-8", newline="\n")
    interpreted = run(hua, "interpret", source)
    old_vm = run(hua, "run", source)
    ir_vm = run(probe, "run", source)
    assert outcome(old_vm) == outcome(ir_vm), (folder.name, "IR/VM", old_vm, ir_vm)
    assert outcome(interpreted) == outcome(old_vm), (folder.name, "AST/VM", interpreted, old_vm)
    checks += 2
    if expected is not None:
        assert old_vm.stdout == expected, (folder.name, old_vm.stdout, expected)
        checks += 1
    if diagnostic:
        assert outcome(old_vm)[2] == diagnostic, (folder.name, old_vm.stderr, diagnostic)
        checks += 1
    else:
        assert old_vm.returncode == 0, (folder.name, old_vm.stderr)
        checks += 1
    if old_vm.returncode:
        # Exact runtime diagnostics across IR VM and the existing VM, including
        # error source and output before the failure (AST budget uses other units).
        assert old_vm.stderr == ir_vm.stderr, (folder.name, old_vm.stderr, ir_vm.stderr)
        checks += 1
    old_listing = run(hua, "bytecode", source)
    ir_listing = run(probe, "bytecode", source)
    assert old_listing.returncode == ir_listing.returncode == 0, (folder.name, old_listing.stderr, ir_listing.stderr)
    assert old_listing.stdout == ir_listing.stdout, (folder.name, "instruction/charge/span drift",
                                                     old_listing.stdout, ir_listing.stdout)
    checks += 2
    artifact = folder / "direct.huab"
    compiled = run(probe, "build", source, "-o", artifact)
    assert compiled.returncode == 0, (folder.name, compiled.stderr)
    assert struct.unpack_from("<II", artifact.read_bytes(), 8) == (6, 1)
    checks += 2
    source.unlink()
    loaded = run(hua, "run", artifact)
    assert outcome(loaded) == outcome(ir_vm), (folder.name, "source-free HUAB", loaded, ir_vm)
    # Archive keeps source text and diagnostics after deleting the source.
    if ir_vm.returncode:
        assert loaded.stderr == ir_vm.stderr, (folder.name, loaded.stderr, ir_vm.stderr)
        checks += 1
    checks += 1
    return artifact

with tempfile.TemporaryDirectory(prefix="e1-", dir=build) as directory:
    root = pathlib.Path(directory)
    fixtures = [
        ("empty", "", ""),
        ("scalars", "const A=2**5\nlet x float=2\nvar y=3\ny+=A\nprint(x,y,true,7/2,-7//3,-7%3)\n", "2 35 true 3.5 -3 2\n"),
        ("numeric-ops", "print(2**8,7//3,7%3,5&3,5|2,5^2,1<<5,64>>2,~0,-2.5,+3)\n", "256 2 1 1 7 7 32 16 -1 -2.5 3\n"),
        ("helper", "print(int(3.75),float(5),int(-3.75))\n", "3 5 -3\n"),
        ("update", "var x=2\nprint(x++,x--,x)\nx*=4\nx//=3\nx%=2\nprint(x)\n", None),
        ("short", "var n=0\nfn yes() bool {n++\nreturn true}\nprint(false&&yes(),true||yes(),n,true&&yes(),false||yes(),n)\n", "false true 0 true true 2\n"),
        ("nested-short", "print((false||true)&&(false||true),false||(true&&false)||true)\n", "true true\n"),
        ("call-stack", "var n=0\nfn step() int {n++\nreturn n}\nfn pair(a int,b int) int {return a*10+b}\nprint(pair(step(),step())+pair(step(),step()))\n", "46\n"),
        ("target-order", "var n=1\nfn side() int {n=10\nreturn 2}\nn+=side()\nprint(n)\n", "12\n"),
        ("initializer-call", "var n=2\nlet v=bump()\nfn bump() int {n++\nreturn n}\nfn main(){print(n,v)}\n", "3 3\n"),
        ("global-before-init", "let before=read()\nvar later=9\nfn read() int {return later}\n", "", "E4001"),
        ("shadow", "var v=5\nfn f(v int) int {if true {let v=8\nprint(v)}\nreturn v}\nfn main(){let v=20\nprint(f(v),v)}\n", "8\n20 20\n"),
        ("branches", "fn f(n int) int {if n<0 {return -1} else if n==0 {return 0} else {return 1}}\nprint(f(-2),f(0),f(5))\n", "-1 0 1\n"),
        ("while", "var n=0\nvar sum=0\nwhile n<10 {n++\nif n==2 {continue}\nif n==6 {break}\nsum+=n}\nprint(n,sum)\n", "6 13\n"),
        ("ranges", "var n=0\nfor i in 1..=9 by 2 {n+=i}\nfor k,v in 5..0 by -2 {print(k,v)}\nprint(n)\n", "0 5\n1 3\n2 1\n25\n"),
        ("empty-range", "for i in 5..2 {print(i)}\nfor j in 0..5 by -1 {print(j)}\nprint(1)\n", "1\n"),
        ("nested-range", "var sum=0\nfor i in 0..4 {for j in 0..4 {if j==1 {continue}\nif i==2 {break}\nsum+=i+j}}\nprint(sum)\n", None),
        ("return-loop", "fn f(n int) int {for i in 0..n {for j in 0..n {if i+j==3 {return i*10+j}}}\nreturn -1}\nprint(f(4),f(1))\n", "3 -1\n"),
        ("range-max", "for i in 9223372036854775806..=9223372036854775807 {print(i)}\n", "9223372036854775806\n9223372036854775807\n"),
        ("range-min", "for i in -9223372036854775807..=-9223372036854775808 by -1 {print(i)}\n", "-9223372036854775807\n-9223372036854775808\n"),
        ("zero-step", "print(1)\nfor i in 0..10 by 0 {}\n", "1\n", "E4005"),
        ("literal-overflow", "print(1)\nprint(9223372036854775808)\n", "1\n", "E4002"),
        ("checked-add", "var n=9223372036854775807\nprint(1)\nprint(n+1)\n", "1\n", "E4002"),
        ("checked-divide", "print(1)\nprint(7/0)\n", "1\n", "E4004"),
        ("error-order", "fn bad() int {print(2)\nreturn 1//0}\nprint(1)\nprint(bad(),9223372036854775808)\n", "1\n2\n", "E4004"),
        ("finite-float", "var n=1e308\nprint(n*n)\n", "", "E4002"),
        ("factorial", "fn fact(n int) int {if n<=1 {return 1}\nreturn n*fact(n-1)}\nprint(fact(10))\n", "3628800\n"),
        ("void", "fn f() void {print(3)\nreturn}\nf()\nfn main(){print(4)}\n", "3\n4\n"),
        ("empty-fn", "fn f() {}\nf()\n", ""),
        ("main-return", "fn main() int {return 3}\n", ""),
        ("recursive-limit", "fn f(){f()}\nf()\n", "", "E4099"),
        ("step-limit", "while true {}\n", "", "E4099"),
        ("float-context", "fn f(x float) float {var y float=x\ny+=2.0\nreturn y}\nfn g() float{return 2}\nvar z float=1\nz=3\nprint(f(3),g(),z,2.0**3)\n", "5 2 3 8\n"),
        ("float-compound-existing-error", "var y float=3\ny+=2\n", "", "E4003"),
        ("float-constant", "const X float=2\nprint(X)\n", "2\n"),
        ("signed-zero", "print(-0.0,0.0==(-0.0),-9223372036854775808)\n", None),
        ("bool-equality", "print(true==false,true!=false,!false)\n", "false true true\n"),
    ]
    for case in fixtures:
        name, text, expected, *diag = case
        differential(root/name, text, expected, diag[0] if diag else None)

    rejected = [
        ("strings", 'print(1)\nprint("hello")\n'),
        ("precise", "let x i32=3\nprint(x)\n"),
        ("list", "let x=[1,2]\nprint(x)\n"),
        ("generic", "fn id<T>(x T) T {return x}\nprint(id<int>(3))\n"),
        ("nested", "fn main(){fn f(){print(1)}\nf()}\n"),
        ("dynamic", "fn f(x){print(x)}\nf(3)\n"),
        ("inferred-return", "fn f(){return 3}\nprint(f())\n"),
        ("defer", "fn f(){print(1)}\nfn main(){defer f()}\n"),
        ("task", "async fn f() int {return 1}\nfn main(){print(await f())}\n"),
        ("module", "import std.time as t\nprint(t.monotonic_ns())\n"),
    ]
    for name, text in rejected:
        folder=root/("reject-"+name);folder.mkdir()
        source=folder/"main.hua";source.write_text(text,encoding="utf-8")
        artifact=folder/"keep.huab";artifact.write_bytes(b"existing artifact")
        for action in ("hir","mir","explain","run","build"):
            args=[action,source]
            if action=="build":args+=["-o",artifact]
            p=run(probe,*args)
            assert p.returncode==1 and "E8001" in p.stderr and not p.stdout,(name,action,p)
            assert artifact.read_bytes()==b"existing artifact"
            checks+=2
        # The old CLI still supports the valid programs outside E1.
        old=run(hua,"check",source)
        assert old.returncode==0,(name,"old CLI",old.stderr)
        checks+=1
print(f"{cases} E1 four-path fixtures, {len(rejected)} explicit subset rejections, {checks} assertions")
