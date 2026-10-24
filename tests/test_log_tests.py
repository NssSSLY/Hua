# HUA-D038 step 5 group 5: @test + hua test, std.test assertions, std.log and
# io.write_err. Library functions run on three paths; hua test is source-only (T5).
from pathlib import Path
import re,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]
LINE=re.compile(r'^ts=\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{3})?Z level=(debug|info|warn|error) msg=(?P<rest>.*)$')

with tempfile.TemporaryDirectory(prefix='hua-test-log-',dir=build) as temp:
    base=Path(temp);n=0
    def source(text):
        global n
        n+=1;p=base/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def run(args,code=0):
        global checks
        r=subprocess.run([str(exe),*map(str,args)],cwd=base,capture_output=True,timeout=120)
        out=r.stdout.decode('utf-8').replace('\r\n','\n');err=r.stderr.decode('utf-8').replace('\r\n','\n')
        assert r.returncode==code,(args,r.returncode,out,err);checks+=1;return out,err
    def three(text,code=0):
        """AST interpreter, source VM and source-free HUAB; returns the (stdout, stderr) pairs."""
        p=source(text);artifact=base/f'case{n}.huab';run(['build',p,'-o',artifact])
        results=[run(['interpret',p],code),run(['run',p],code)]
        hidden=p.with_suffix('.moved');p.rename(hidden);results.append(run(['run',artifact],code));hidden.rename(p)
        return results,artifact

    # hua test: report lines, exit code, isolation between tests, --filter; run still runs main.
    tests=source('''import std.test as test
import std.os as os
var counter = 0
fn add(a int, b int) int { return a + b }
@test
fn adds() {
    counter += 1
    test.equal(add(2, 3), 5)
    test.equal(counter, 1)
}
@test
fn isolated() {
    counter += 1
    test.equal(counter, 1)
}
@test
fn wrong() { test.equal(add(2, 2), 5) }
@test
fn returns_err() Result<void> { return err("bad input") }
@test
fn crashes() { print(1 // 0) }
@test
fn exits() { os.exit(3) }
@test
fn ok_result() Result<void> {
    test.assert(add(1, 1) == 2, "math")
    return ok()
}
fn main() { print("main runs", add(1, 1)) }
''')
    out,_=run(['test',tests],1)
    lines=[re.sub(r' \(.*:\d+:\d+\)$',' (loc)',l) for l in out.splitlines()]
    assert lines==['PASS adds','PASS isolated','FAIL wrong: E4900: assertion failed: expected 5, got 4 (loc)','FAIL returns_err: returned Err: bad input',
                   'FAIL crashes: E4004: division by zero (loc)','FAIL exits: test called os.exit(3)','PASS ok_result','3 passed, 4 failed'],lines
    out,_=run(['test',tests,'--filter','add']);assert out=='PASS adds\n1 passed, 0 failed\n',out
    out,_=run(['test',tests,'--filter','nothing']);assert out=='0 passed, 0 failed\n',out
    out,_=run(['run',tests]);assert out=='main runs 2\n',out
    run(['test',base/'missing.hua'],1)
    # Assertions on three paths; failures are E4900 diagnostics in ordinary programs.
    results,artifact=three('''import std.test as test
struct P { x int }
fn main() {
    test.assert(true, "never shown")
    test.equal([1, 2], [1, 2])
    test.equal(P{x: 1}, P{x: 1})
    test.equal(map[string]int{"a": 1}, map[string]int{"a": 1})
    test.not_equal("a", "b")
    print("ok")
}
''')
    assert all(o=='ok\n' for o,_ in results),results
    assert header_format(artifact)==18,header_format(artifact)
    for body,want in [('test.assert(1 > 2, "one is not bigger")','assertion failed: one is not bigger'),('test.fail("stop here")','assertion failed: stop here'),
                      ('test.not_equal(3, 3)','assertion failed: values are equal: 3'),('test.equal("ab", "ac")','assertion failed: expected ac, got ab')]:
        results,_=three('import std.test as test\nfn main() {\n    '+body+'\n}\n',1)
        assert all('E4900' in e and want in e for _,e in results),(body,results)
    results,_=three('import std.test as test\nimport std.strings as strings\nfn main() { test.equal(strings.repeat("x", 500), "y") }\n',1)
    assert all('got '+'x'*200+'...' in e for _,e in results),results[0][1][:400]
    # Logging and write_err: stderr only, logfmt format, escaping, field order, set_level.
    results,_=three('''import std.io as io
import std.log as log
fn main() {
    print(unwrap(io.write("to stdout\\n")), unwrap(io.write_err("to stderr\\n")))
    log.info("hello world")
    log.debug("hidden by default")
    log.set_level("debug")
    log.debug("plain")
    log.with_fields("warn", "disk \\"low\\"", map[string]string{"path": "C:/a b", "pct": "93", "note": "x=y\\n", "empty": ""})
    log.set_level("error")
    log.warn("filtered")
    log.error("tab\\there")
}
''')
    for out,err in results:
        assert out=='to stdout\n10 10\n',out
        lines=err.splitlines();assert lines[0]=='to stderr',lines
        rest=[LINE.match(l) for l in lines[1:]];assert all(rest),lines
        assert [m.group('rest') for m in rest]==['"hello world"','plain','"disk \\"low\\"" empty="" note="x=y\\n" path="C:/a b" pct=93','"tab\\there"'],[m.group('rest') for m in rest]
        assert [m.group(2) for m in rest]==['info','debug','warn','error']
    # Concurrent Tasks: every stderr line stays whole.
    results,_=three('''import std.log as log
fn worker(id int) int {
    for i in 0..300 { log.info("worker " + str(id) + " line " + str(i)) }
    return id
}
fn main() {
    let a = spawn worker(1)
    let b = spawn worker(2)
    let c = spawn worker(3)
    print(await a + await b + await c)
}
''')
    for out,err in results:
        assert out=='6\n',out
        lines=err.splitlines();assert len(lines)==900 and all(LINE.match(l) for l in lines),(len(lines),[l for l in lines if not LINE.match(l)][:3])
    # Errors.
    for body,error in [('log.with_fields("loud", "x", map[string]string{})','E4003'),('log.set_level("verbose")','E4003'),
                       ('log.with_fields("info", "x", map[string]string{"bad key": "v"})','E4003')]:
        results,_=three('import std.log as log\nfn main() {\n    '+body+'\n}\n',1)
        assert all(error in e for _,e in results),(body,results)
    for body in ['test.equal(1, "a")','log.with_fields("info", "x", map[string]int{})','test.assert(1, "x")']:
        p=source('import std.test as test\nimport std.log as log\nfn main() {\n    '+body+'\n}\n');_,e=run(['check',p],1);assert 'E3004' in e,(body,e)
    for text in ['import std.test as test\n@test\nfn bad(x int) { }\nfn main() { }\n','fn main() {\n    @test\n    fn inner() { }\n}\n','@test\nfn bad() int { return 1 }\nfn main() { }\n','@test\nstruct S { x int }\nfn main() { }\n']:
        p=source(text);_,e=run(['check',p],1);assert 'E3004' in e or 'E2002' in e,(text,e)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,17);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    _,e=run(['run',bad],1);assert 'E6002' in e,e
print(f'test/log checks passed: {checks}')
