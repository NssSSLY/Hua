"""Independent Python references for 25 APIs: interpreter, VM and source-free HUAB."""
from pathlib import Path
import bisect, json, math, os, random, struct, subprocess, sys, tempfile
exe, root, build = (Path(x).resolve() for x in sys.argv[1:])
checks = 0
fixtures = 0
covered = set()
environment = dict(os.environ)
environment.update({"HUA_TEST_TEXT": "中文 value", "HUA_TEST_EMPTY": "", "HUA_TEST_变量": "配置"})
environment.pop("HUA_TEST_MISSING_23B941", None)

def run(command, path, *extra, code=0, error=None):
    global checks
    p = subprocess.run([str(exe), command, str(path), *map(str, extra)],
                       capture_output=True, env=environment, timeout=30)
    stdout = p.stdout.decode("utf-8").replace("\r\n", "\n")
    stderr = p.stderr.decode("utf-8")
    assert p.returncode == code, (command, path, p.returncode, stdout, stderr)
    if error: assert error in stderr and not stdout, (command, stdout, stderr)
    checks += 1
    return stdout

with tempfile.TemporaryDirectory(prefix="hua-alg-config-", dir=build) as temp:
    base = Path(temp)
    def triple(text, validate, identical=True):
        global fixtures
        fixtures += 1
        source = base / f"case{fixtures}.hua"
        source.write_text(text, encoding="utf-8", newline="\n")
        assert run("check", source) == "OK\n"
        run("build", source)
        artifact = source.with_suffix(".huab")
        assert struct.unpack_from("<II", artifact.read_bytes(), 8) == (6, 1)
        outputs = [run(command, source) for command in ("interpret", "run")]
        source.unlink()
        assert run("check", artifact) == "OK\n"
        outputs.append(run("run", artifact))
        if identical: assert len(set(outputs)) == 1, outputs
        for out in outputs: validate(out)
        return artifact

    def exact(want):
        def validate(out): assert out == want, (out, want)
        return validate

    def failure(text, code="E3004"):
        global fixtures
        fixtures += 1
        source = base / f"error{fixtures}.hua"
        source.write_text(text, encoding="utf-8")
        for command in ("check", "run", "interpret", "build"):
            run(command, source, code=1, error=code)
        assert not source.with_suffix(".huab").exists()

    math_cases = {
        "floor": [-2.9, -0.0, 0.9, 1e100], "ceil": [-2.9, 0.0, 0.9],
        "trunc": [-2.9, -0.0, 2.9], "round": [-3.5, -2.5, -1.5, -0.5, 0.5, 1.5, 2.5, 3.5, 1e100],
        "sin": [-1.0, 0.0, 0.7], "cos": [-1.0, 0.0, 0.7],
        "tan": [-1.0, 0.0, 0.7], "exp": [-1000.0, 0.0, 0.7],
        "log": [0.5, 1.0, 8.0], "log2": [0.5, 1.0, 8.0],
        "log10": [0.1, 1.0, 1000.0], "hypot": [(3.0, 4.0), (-3.0, 4.0), (1e200, 1e200)]
    }
    text = "import std.math as m\nimport std.json as j\n"
    expected = []
    for name, values in math_cases.items():
        covered.add("math." + name)
        reference = round if name == "round" else getattr(math, name)
        for value in values:
            args = value if isinstance(value, tuple) else (value,)
            text += "print(unwrap(j.encode(unwrap(m." + name + "(" + ",".join(repr(x) for x in args) + ")))))\n"
            expected.append(float(reference(*args)))
    def math_validate(out):
        values = [json.loads(line) for line in out.splitlines()]
        assert len(values) == len(expected)
        for actual, want in zip(values, expected):
            assert math.isclose(actual, want, rel_tol=2e-14, abs_tol=2e-14), (actual, want)
    triple(text, math_validate)
    triple("""import std.math as m
print(is_err(m.log(0.0)),is_err(m.log2(-1.0)),is_err(m.log10(-2.0)))
print(unwrap_err(m.exp(1000.0)))
print(unwrap_err(m.hypot(1.7e308,1.7e308)))
""", exact("true true true\nMATH_OVERFLOW: non-finite result\nMATH_OVERFLOW: non-finite result\n"))
    triple("""import std.math as m
fn value() Result<float,string> { return ok(m.log(8.0)?) }
print(is_ok(value()), unwrap(m.round(2.5)), unwrap(m.round(-2.5)))
""", exact("true 2 -2\n"))

    rng = random.Random(20261006)
    cases = {
        "int": [[], [1], [2, 1, 2, -7, 0], [-(2**63), 2**63-1]] +
               [[rng.randrange(-999,1000) for _ in range(n)] for n in (2, 17, 100, 513)],
        "float": [[], [0.0], [-2.5, 0.0, 2.5, -0.0, 2.5]] +
                 [[rng.randrange(-999,1000)/8 for _ in range(n)] for n in (2, 17, 100, 513)],
        "string": [[], [""], ["中文", "a", "", "文", "a", "z", "中", "\0"]] +
                  [[rng.choice(["", "a", "中文", "中", "😀", "z", "á"]) for _ in range(n)] for n in (2, 17, 100, 513)]
    }
    for kind, arrays in cases.items():
        for values in arrays:
            key = (lambda x: x.encode("utf-8")) if kind == "string" else (lambda x: x)
            ordered = sorted(values, key=key)
            target = (values[len(values)//2] if values else {"int":3,"float":3.0,"string":"missing"}[kind])
            literal = json.dumps(values, ensure_ascii=False, separators=(",",":")).replace("\\u0000","\\0")
            target_literal = json.dumps(target, ensure_ascii=False).replace("\\u0000","\\0")
            program = f"""import std.alg as a
import std.json as j
let xs []{kind} = {literal}
let ys=unwrap(a.sorted(xs))
print(unwrap(j.encode(ys)))
print(unwrap(j.encode(unwrap(a.reverse(xs)))))
print(a.contains(xs,{target_literal}),a.index(xs,{target_literal}))
print(a.lower_bound(ys,{target_literal}),a.upper_bound(ys,{target_literal}),unwrap_or(a.binary_search(ys,{target_literal}),-1))
print(is_err(a.min(xs)),is_err(a.max(xs)))
"""
            expected_sum = None
            if kind != "string":
                program += "print(unwrap(j.encode(unwrap(a.sum(xs)))))\n"
                expected_sum = sum(values) if kind == "int" else sum(values, 0.0)
            if values:
                program += "print(unwrap(j.encode(unwrap(a.min(xs)))))\nprint(unwrap(j.encode(unwrap(a.max(xs)))))\n"
            def validate(out, values=values, ordered=ordered, target=target, kind=kind, key=key, expected_sum=expected_sum):
                lines = out.splitlines()
                assert json.loads(lines[0]) == ordered
                assert json.loads(lines[1]) == values[::-1]
                assert lines[2] == f"{str(target in values).lower()} {values.index(target) if target in values else -1}"
                keys = list(map(key, ordered)); t = key(target)
                lower, upper = bisect.bisect_left(keys,t), bisect.bisect_right(keys,t)
                assert lines[3] == f"{lower} {upper} {lower if lower < len(keys) and keys[lower] == t else -1}"
                assert lines[4] == ("false false" if values else "true true")
                at = 5
                if kind != "string":
                    assert json.loads(lines[at]) == expected_sum
                    at += 1
                if values:
                    assert json.loads(lines[at]) == ordered[0] and json.loads(lines[at+1]) == ordered[-1]
            artifact = triple(program, validate)
    covered.update("alg." + name for name in ("sorted","reverse","contains","index","lower_bound","upper_bound","binary_search","sum","min","max"))
    triple("""import std.alg as a
let xs=[1,2,2,4]
print(a.index(xs,3),a.contains(xs,3),a.lower_bound(xs,3),a.upper_bound(xs,3),is_none(a.binary_search(xs,3)))
print(a.lower_bound(xs,0),a.upper_bound(xs,5),a.binary_search(xs,2))
print(is_err(a.sum([9223372036854775807,1])),is_err(a.sum([-9223372036854775808,-1])))
print(is_err(a.sum([1.7e308,1.7e308])))
""", exact("-1 false 3 3 true\n0 4 1\ntrue true\ntrue\n"))
    triple("""import std.alg as a
var xs [4]int=[7,4,8,1]
let view=xs[1:3]
let ordered=unwrap(a.sorted(view))
let reversed=unwrap(a.reverse(view))
xs[1]=99
print(ordered,reversed,xs)
var editable=clone(ordered)
editable[0]=42
print(editable,ordered)
""", exact("[4, 8] [8, 4] [7, 99, 8, 1]\n[42, 8] [4, 8]\n"))
    triple("""import std.alg as a
fn inner(){return a.sum([1,2,3])}
print(unwrap(inner()))
""", exact("6\n"))
    triple("""import std.time as t
import std.task as task
import std.os as os
let begin=t.monotonic_ns()
task.sleep(12)
let end=t.monotonic_ns()
print(begin>=0,end>=begin,(end-begin)>=1000000,(end-begin)<30000000000)
print(t.unix_ms())
print(unwrap(os.getenv("HUA_TEST_TEXT")),unwrap(os.getenv("HUA_TEST_变量")))
print(is_some(unwrap(os.getenv("HUA_TEST_EMPTY"))),len(unwrap(unwrap(os.getenv("HUA_TEST_EMPTY")))))
print(is_none(unwrap(os.getenv("HUA_TEST_MISSING_23B941"))))
print(is_err(os.getenv("")),is_err(os.getenv("x=y")),is_err(os.getenv("x\\0y")))
""", lambda out: (
        (lambda lines: (
            lines[0] == "true true true true" and
            1000000000000 < int(lines[1]) < 5000000000000 and
            lines[2:] == ["中文 value 配置","true 0","true","true true true"]
        ))(out.splitlines()) or (_ for _ in ()).throw(AssertionError(out))
    ), identical=False)
    covered.update(("time.monotonic_ns","time.unix_ms","os.getenv"))
    triple("""import std.alg as a
import std.math as m
import std.time as t
import std.os as os
async fn work() int {
    let before=t.monotonic_ns()
    print(unwrap(os.getenv("HUA_TEST_TEXT")))
    print(unwrap(a.sorted([3,1,2])),unwrap(m.hypot(3.0,4.0)))
    return t.monotonic_ns()-before
}
fn main(){print((await work())>=0)}
""", exact("中文 value\n[1, 2, 3] 5\ntrue\n"))
    invalid = [
        "m.floor(1)", "m.hypot(1.0,2)", "m.floor(f32(1.0))", "m.exp(\"x\")",
        "a.sorted([true])", "a.sorted([i32(1)])", "a.sorted([])", "a.sorted(1)",
        "a.sum([\"x\"])", "a.contains([1.0],1)", "a.index([1],1.0)",
        "a.min(map[string]int{\"x\":1})",
        "a.sorted(list.from_slice([1]))"
    ]
    imports = "import std.alg as a\nimport std.math as m\nimport std.list as list\n"
    for expr in invalid: failure(imports+expr+"\n")
    failure(imports+"a.sorted([1],[2])\n","E3005")
    failure("import std.time as t\nt.unix_ms(1)\n","E3005")
    failure("import std.os as os\nos.getenv(1)\n")
    failure(imports+"var xs=unwrap(a.sorted([2,1]))\nxs[0]=3\n","E3003")
    version_artifact = triple("import std.alg as a\nprint(unwrap(a.sorted([3,1,2])))\n",exact("[1, 2, 3]\n"))
    data=version_artifact.read_bytes()
    old = base/"old-format.huab"
    changed=bytearray(data);struct.pack_into("<I",changed,8,5);old.write_bytes(changed)
    run("check",old,code=1,error="E6002")
    old_runtime = build/"phase12-old-runtime/hua.exe"
    if old_runtime.exists():
        p = subprocess.run([str(old_runtime),"check",str(version_artifact)],capture_output=True,timeout=15)
        assert p.returncode == 1 and b"E6002" in p.stderr, (p.returncode,p.stdout,p.stderr)
        checks += 1
    assert len(covered)==25,covered

print(f"25 APIs, {fixtures} fixtures, {checks} process checks; independent algorithm/math references and three execution paths passed")
(build/"algorithm_stdlib_verification.json").write_text(json.dumps(
    {"apis":sorted(covered),"fixtures":fixtures,"checks":checks,"paths":["interpreter","VM","source-free HUAB"],"format":6},
    ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
