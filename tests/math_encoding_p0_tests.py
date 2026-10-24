# HUA-D038 step 4 (std.math P0, std.encoding) across three execution paths.
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

with tempfile.TemporaryDirectory(prefix='hua-math-encoding-',dir=build) as temp:
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
        p=fixture('import std.math as math\nimport std.bytes as bytes\nimport std.encoding as encoding\nfn main() {\n'+body+'\n}\n')
        run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:run(command,target,cwd=work,code=1,error=error)

    artifact=triple('''import std.math as math
import std.bytes as bytes
import std.encoding as encoding
fn show(r Result<Bytes,string>) string {
    if is_err(r) { return unwrap_err(r) }
    return unwrap(bytes.to_text(unwrap(r)))
}
fn main() {
    print(math.gcd(12, 18), math.gcd(-12, 18), math.gcd(0, 0), math.gcd(0, -7), math.lcm(4, 6), math.lcm(-4, 6), math.lcm(0, 5))
    print(math.gcd(-9223372036854775807 - 1, 6), math.lcm(2147483647, 2147483646))
    print(math.isqrt(0), math.isqrt(15), math.isqrt(16), math.isqrt(9223372036854775807), math.isqrt(4611686014132420609))
    print(unwrap(math.asin(1.0)), unwrap(math.acos(-1.0)), unwrap(math.atan(1.0)), unwrap(math.atan2(-1.0, -1.0)), unwrap(math.atan2(0.0, -1.0)))
    print(unwrap(math.cbrt(-27.0)), unwrap(math.fmod(7.5, 2.0)), unwrap(math.fmod(-7.5, 2.0)), unwrap_err(math.fmod(1.0, 0.0)), unwrap_err(math.asin(1.5)))
    print(math.pi(), math.e(), math.tau())
    for t in ["", "f", "fo", "foo", "foob", "fooba", "foobar"] {
        let b = bytes.from_text(t)
        print("[" + encoding.base64_encode(b) + "]", "[" + encoding.base64url_encode(b) + "]", "[" + encoding.hex_encode(b) + "]")
        print("[" + show(encoding.base64_decode(encoding.base64_encode(b))) + "]", "[" + show(encoding.base64url_decode(encoding.base64url_encode(b))) + "]", "[" + show(encoding.hex_decode(encoding.hex_encode(b))) + "]")
    }
    print(show(encoding.hex_decode("666F6f")), show(encoding.hex_decode("abc")), show(encoding.hex_decode("zz")), show(encoding.hex_decode("0g")))
    print(show(encoding.base64_decode("Zg")), show(encoding.base64_decode("Zg=")), show(encoding.base64_decode("Zh==")), show(encoding.base64_decode("Z g==")), show(encoding.base64_decode("====")))
    print(show(encoding.base64url_decode("Zg==")), show(encoding.base64url_decode("Zg")), show(encoding.base64url_decode("Zg=")), show(encoding.base64url_decode("Z")), show(encoding.base64url_decode("Zm9v+A")))
    let raw = unwrap(bytes.from_ints([251, 255, 191]))
    print(encoding.base64_encode(raw), encoding.base64url_encode(raw), encoding.hex_encode(raw))
}
''','''6 6 0 7 12 12 0
2 4611686011984936962
0 3 4 3037000499 2147483647
1.5707963267949 3.14159265358979 0.785398163397448 -2.35619449019234 3.14159265358979
-3 1.5 -1.5 MATH_DOMAIN: fmod requires y != 0 MATH_DOMAIN: asin requires -1 <= x <= 1
3.14159265358979 2.71828182845905 6.28318530717959
[] [] []
[] [] []
[Zg==] [Zg] [66]
[f] [f] [f]
[Zm8=] [Zm8] [666f]
[fo] [fo] [fo]
[Zm9v] [Zm9v] [666f6f]
[foo] [foo] [foo]
[Zm9vYg==] [Zm9vYg] [666f6f62]
[foob] [foob] [foob]
[Zm9vYmE=] [Zm9vYmE] [666f6f6261]
[fooba] [fooba] [fooba]
[Zm9vYmFy] [Zm9vYmFy] [666f6f626172]
[foobar] [foobar] [foobar]
foo ENC_SYNTAX: odd length ENC_SYNTAX: invalid character at byte 0 ENC_SYNTAX: invalid character at byte 1
ENC_SYNTAX: invalid length ENC_SYNTAX: invalid length ENC_SYNTAX: non-zero trailing bits ENC_SYNTAX: invalid length ENC_SYNTAX: invalid padding
f f ENC_SYNTAX: invalid length ENC_SYNTAX: invalid length ENC_SYNTAX: invalid character at byte 4
+/+/ -_-_ fbffbf
''')
    assert header_format(artifact)==12,header_format(artifact)
    old=triple('import std.math as math\nfn main() { print(unwrap(math.floor(2.5))) }\n','2\n')
    assert header_format(old)<12,header_format(old)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,11);bad=base/'downgraded.huab';bad.write_bytes(bytes(data))
    run('run',bad,code=1,error='E6002')
    # Integer overflow and domain errors are diagnostics on every path (G3).
    runtime_failure('    print(math.gcd(-9223372036854775807 - 1, 0))','E4002')
    runtime_failure('    print(math.lcm(9223372036854775807, 2))','E4002')
    runtime_failure('    print(math.isqrt(-1))','E4003')
    # Static: integer functions take exactly int, float functions exactly default float.
    for body in ['print(math.gcd(1.0, 2))','print(math.isqrt(2.0))','print(math.atan2(1, 2.0))','print(math.cbrt(8))',
                 'print(encoding.hex_encode("ab"))','print(encoding.base64_decode(bytes.from_text("x")))','print(math.pi(1.0))']:
        p=fixture('import std.math as math\nimport std.bytes as bytes\nimport std.encoding as encoding\nfn main() {\n    '+body+'\n}\n')
        run('check',p,code=1,error='E300')
print(f'math/encoding P0 three-path checks passed: {checks}')
