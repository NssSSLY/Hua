# HUA-D038 slice 1 (std.strings P0, parse/format, Buffer text path) across three execution paths.
from pathlib import Path
import difflib,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0

def run(command,path,*extra,cwd=None,expected=None,code=0,error=None):
    global checks
    p=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=cwd,capture_output=True,timeout=60)
    out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8')
    assert p.returncode==code,(command,path,p.returncode,out,err)
    if expected is not None:assert out==expected,(command,''.join(difflib.unified_diff(expected.splitlines(True),out.splitlines(True),'expected','actual')),err)
    if error:assert error in err,(command,out,err,error)
    checks+=1;return out

def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]

with tempfile.TemporaryDirectory(prefix='hua-strings-',dir=build) as temp:
    base=Path(temp);source=base/'source';source.mkdir();work=base/'work';work.mkdir();n=0
    def fixture(text):
        global n
        n+=1;p=source/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def triple(text,expected):
        p=fixture(text);run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.unlink();run('check',artifact,expected='OK\n')
            run(command,target,cwd=work,expected=expected)
        return artifact
    def runtime_failure(body,error):
        p=fixture('import std.strings as strings\nimport std.buffer as buffer\nfn main() {\n'+body+'\n}\n')
        run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:run(command,target,cwd=work,code=1,error=error)

    artifact=triple('''import std.strings as strings
import std.buffer as buffer
fn main() {
    print(strings.last_index("abcabc", "bc"), strings.last_index("abc", "x"), strings.last_index("abc", ""))
    print(strings.count("a,b,,c", ","), strings.count("aaaa", "aa"), strings.count("中文", ""), strings.count("", ""))
    print(strings.repeat("ab", 3), "[" + strings.repeat("ab", 0) + "]", "[" + strings.repeat("", 5) + "]")
    print(strings.join(strings.split_n("a,b,c,d", ",", 2), "|"), len(strings.split_n("a,b", ",", 0)), strings.join(strings.split_n("a,b,c", ",", -1), "|"))
    print(strings.join(strings.split_n("abc", "", 2), "|"), strings.join(strings.split_n("a,b", ",", 9), "|"))
    print(strings.join(strings.fields("  x \\t y\\nz  "), "|"), len(strings.fields(" \\t\\n ")), len(strings.fields("")))
    print(strings.join(strings.lines("one\\r\\ntwo\\n\\nthree\\n"), "|"), len(strings.lines("")), strings.join(strings.lines("a\\n\\n"), "|") + "!")
    print("[" + strings.trim_start("  hi  ") + "][" + strings.trim_end("  hi  ") + "][" + strings.trim_start("   ") + "]")
    print(strings.trim_prefix("v1.2", "v"), strings.trim_prefix("v1.2", "x"), strings.trim_suffix("file.hua", ".hua"), strings.trim_suffix("ab", "ab") + "!")
    print(strings.pad_left("42", 6, "0"), strings.pad_right("中", 3, "*"), strings.pad_left("long", 2, " "), strings.pad_left("x", 3, "文"))
    print(strings.replace_n("aaaa", "a", "b", 2), strings.replace_n("aaaa", "a", "b", -1), strings.replace_n("aaaa", "a", "b", 0), strings.replace_n("ab", "", "x", 3))
    print(strings.is_empty(""), strings.is_empty("x"))
    print(unwrap(strings.parse_int("-ff", 16)), unwrap(strings.parse_int("+101", 2)), unwrap(strings.parse_int("Zz", 36)))
    print(unwrap(strings.parse_int("-9223372036854775808", 10)), unwrap(strings.parse_int("9223372036854775807", 10)))
    print(unwrap_err(strings.parse_int("9223372036854775808", 10)))
    print(unwrap_err(strings.parse_int("", 10)))
    print(unwrap_err(strings.parse_int(" 1", 10)))
    print(unwrap_err(strings.parse_int("0x10", 16)))
    print(unwrap(strings.parse_float("-1.5e3")), unwrap(strings.parse_float("+.5")), unwrap(strings.parse_float("5.")), unwrap(strings.parse_float("1e-400")))
    print(unwrap_err(strings.parse_float("1e999")))
    print(unwrap_err(strings.parse_float("inf")))
    print(unwrap_err(strings.parse_float("+-5")))
    print(unwrap_err(strings.parse_float("1e")))
    print(strings.format_int(255, 16), strings.format_int(-5, 2), strings.format_int(-9223372036854775807 - 1, 36), strings.format_int(0, 10))
    print(strings.format_float(2.5, 0), strings.format_float(3.5, 0), strings.format_float(3.14159, 2), strings.format_float(-0.125, 2), strings.format_float(1.0, 3))
    print(unwrap(strings.format("{} + {} = {2}, {{x}} {0}", ["1", "2", "3"])), "[" + unwrap(strings.format("", [])) + "]")
    print(unwrap_err(strings.format("{3}", ["a"])))
    print(unwrap_err(strings.format("{", ["a"])))
    print(unwrap_err(strings.format("}", ["a"])))
    print(unwrap_err(strings.format("{x}", ["a"])))
    print(unwrap_err(strings.format("{}{}", ["a"])))
    var b = buffer.new()
    buffer.reserve(b, 64)
    buffer.write_str(b, "n=")
    buffer.write_int(b, -42)
    buffer.write_str(b, " 中")
    print(unwrap(buffer.take_text(b)), buffer.len(b), unwrap(buffer.take_text(b)) + "!")
    unwrap(buffer.write_u8(b, 255))
    print(strings.starts_with(unwrap_err(buffer.take_text(b)), "TEXT_UTF8: "))
}
''','''4 -1 3
3 2 3 1
ababab [] []
a|b,c,d 0 a|b|c
a|bc a|b
x|y|z 0 0
one|two||three 0 a|!
[hi  ][  hi][]
1.2 v1.2 file !
000042 中** long 文文x
bbaa bbbb aaaa ab
true false
-255 5 1295
-9223372036854775808 9223372036854775807
PARSE_RANGE: integer is outside int64
PARSE_SYNTAX: no digits in integer text
PARSE_SYNTAX: invalid digit for base 10
PARSE_SYNTAX: invalid digit for base 16
-1500 0.5 5 0
PARSE_RANGE: float is outside the finite range
PARSE_SYNTAX: invalid float text
PARSE_SYNTAX: invalid float text
PARSE_SYNTAX: invalid float exponent
ff -101 -1y2p0ij32e8e8 0
2 4 3.14 -0.12 1.000
1 + 2 = 3, {x} 1 []
FORMAT_ARGS: placeholder index out of range
FORMAT_ARGS: unclosed '{'
FORMAT_ARGS: unmatched '}'
FORMAT_ARGS: placeholder must be {} or {index}
FORMAT_ARGS: placeholder index out of range
n=-42 中 0 !
true
''')
    assert header_format(artifact)==9,header_format(artifact)
    # Programs that use none of the new functions keep their previous format.
    old=triple('import std.strings as strings\nfn main() { print(strings.trim("  x  ")) }\n','x\n')
    assert header_format(old)<9,header_format(old)
    # A format-9 program whose header claims format 8 is rejected before execution.
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,8);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    run('run',bad,code=1,error='E6002')
    # Programmer errors and limits are runtime diagnostics on every path.
    runtime_failure('    print(strings.repeat("a", -1))','E4003')
    runtime_failure('    print(strings.repeat("ab", 9000000))','E4099')
    runtime_failure('    print(strings.pad_left("a", 3, "ab"))','E4003')
    runtime_failure('    print(strings.pad_left("a", 3, ""))','E4003')
    runtime_failure('    print(strings.pad_right("a", -1, " "))','E4003')
    runtime_failure('    print(strings.parse_int("1", 1))','E4003')
    runtime_failure('    print(strings.format_int(1, 37))','E4003')
    runtime_failure('    print(strings.format_float(1.0, 18))','E4003')
    runtime_failure('    var b = buffer.new()\n    buffer.reserve(b, -1)','E4003')
    # The new Buffer writers need a writable Buffer, as the existing ones.
    for writer in ['buffer.write_str(b, "x")','buffer.write_int(b, 1)','buffer.reserve(b, 8)','unwrap(buffer.take_text(b))','unwrap(buffer.write_text(b, "x"))']:
        p=fixture('import std.buffer as buffer\nfn take(b Buffer) { '+writer+' }\nfn main() { take(buffer.new()) }\n')
        run('check',p,code=1,error='E3003')
print(f'strings P0 three-path checks passed: {checks}')
