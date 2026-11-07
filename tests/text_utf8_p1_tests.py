# HUA-D038 P1: strings center/codepoints/chars/from_codepoints/slice_runes and encoding
# utf8_valid/utf8_encode/utf8_decode (since 23). Every case runs on the AST interpreter,
# the source VM and a source-free HUAB; UTF-8 rules are checked against Python's strict codec.
from pathlib import Path
import struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]

def strict(data):
    try:return data.decode('utf-8')
    except UnicodeDecodeError:return None
def first_scalar(data):
    for n in range(1,5):
        if n<=len(data) and strict(data[:n]) is not None:return ord(strict(data[:n])),n
    return -1,0

with tempfile.TemporaryDirectory(prefix='hua-text-utf8-',dir=build) as temp:
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
        p=source(text);artifact=base/f'case{n}.huab';run(['build',p,'-o',artifact])
        results=[run(['interpret',p],code),run(['run',p],code)]
        hidden=p.with_suffix('.moved');p.rename(hidden);results.append(run(['run',artifact],code));hidden.rename(p)
        return results,artifact
    def same(text,want):
        results,artifact=three(text)
        assert all(o==want for o,_ in results),(want,results);return artifact

    # center: the odd unit goes right; already wide enough; multi-byte fill and text.
    artifact=same('''import std.strings as strings
fn main() {
    print(strings.center("ab", 5, "*"), strings.center("abc", 6, "*"), strings.center("abc", 7, "-"))
    print(strings.center("abc", 3, "*"), strings.center("abc", 2, "*"), strings.center("abc", 0, "*"))
    print(strings.center("", 3, "中"), strings.center("中", 4, "é"), strings.center("😀", 2, "😀"))
}
''','*ab** *abc** --abc--\nabc abc abc\n中中中 é中éé 😀😀\n')
    assert header_format(artifact)==23,header_format(artifact)
    # codepoints/chars/from_codepoints against ord/chr, including every encoded-length boundary.
    boundaries=[1,127,128,2047,2048,55295,57344,65535,65536,1114111]
    text=''.join(map(chr,boundaries))
    literal=', '.join(map(str,boundaries))
    same(f'''import std.strings as strings
fn main() {{
    let cs [{len(boundaries)}]int = [{literal}]
    let t = unwrap(strings.from_codepoints(cs))
    let back = strings.codepoints(t)
    print(back, len(t), len(strings.chars(t)))
    print(strings.codepoints(""), strings.chars(""), len(unwrap(strings.from_codepoints(strings.codepoints("")))))
    print(strings.chars("aé中😀"), strings.codepoints("aé中😀"))
    var w = clone(strings.codepoints("ab"))
    w[0] = 99
    print(unwrap(strings.from_codepoints(w)))
    for c in strings.chars("é中") {{ print(c, len(c)) }}
}}
''',f'[{literal}] {len(text.encode())} {len(boundaries)}\n[] [] 0\n[a, é, 中, 😀] [97, 233, 20013, 128512]\ncb\né 2\n中 3\n')
    # from_codepoints errors name the first invalid index and never replace.
    same('''import std.strings as strings
fn main() {
    print(strings.from_codepoints([65, 55296, -1]))
    print(strings.from_codepoints([57343]))
    print(strings.from_codepoints([1114112]))
    print(strings.from_codepoints([65, 66, -1]))
}
''','err(TEXT_CODEPOINT: invalid Unicode scalar at index 1)\nerr(TEXT_CODEPOINT: invalid Unicode scalar at index 0)\nerr(TEXT_CODEPOINT: invalid Unicode scalar at index 0)\nerr(TEXT_CODEPOINT: invalid Unicode scalar at index 2)\n')
    # slice_runes against Python slicing, and every bounds error.
    sample='aé中😀b'
    rows=[(a,b) for a in range(len(sample)+1) for b in range(a,len(sample)+1)]
    body=''.join(f'    print("[" + unwrap(strings.slice_runes(s, {a}, {b})) + "]")\n' for a,b in rows)
    same('import std.strings as strings\nfn main() {\n    let s = "aé中😀b"\n'+body+'}\n',''.join(f'[{sample[a:b]}]\n' for a,b in rows))
    bad='err(string rune slice bounds are invalid)\n'
    same('''import std.strings as strings
fn main() {
    print(strings.slice_runes("abc", -1, 2))
    print(strings.slice_runes("abc", 2, 1))
    print(strings.slice_runes("abc", 0, 4))
    print(strings.slice_runes("abc", 4, 4))
    print(strings.slice_runes("", 0, 1))
    print("[" + unwrap(strings.slice_runes("", 0, 0)) + "]")
}
''',bad*5+'[]\n')
    # utf8_encode against Python, at every boundary and for invalid values.
    good=boundaries+[0,233,20013,128512]
    body=''.join(f'    print(encoding.hex_encode(unwrap(encoding.utf8_encode({c}))))\n' for c in good)
    same('import std.encoding as encoding\nfn main() {\n'+body+'}\n',''.join(chr(c).encode().hex()+'\n' for c in good))
    same('''import std.encoding as encoding
fn main() {
    print(encoding.utf8_encode(-1))
    print(encoding.utf8_encode(55296))
    print(encoding.utf8_encode(57343))
    print(encoding.utf8_encode(1114112))
}
''','err(ENC_RANGE: invalid Unicode scalar)\n'*4)
    # utf8_valid and utf8_decode against Python's strict decoder and bytes.to_text.
    cases=[b'',b'a',b'\x00',b'\xc2\x80',b'\xdf\xbf',b'\xe0\xa0\x80',b'\xed\x9f\xbf',b'\xee\x80\x80',b'\xef\xbf\xbf',b'\xf0\x90\x80\x80',b'\xf4\x8f\xbf\xbf',
           b'\xc0\x80',b'\xc1\xbf',b'\xe0\x80\x80',b'\xe0\x9f\xbf',b'\xf0\x80\x80\x80',b'\xf0\x8f\xbf\xbf',b'\xed\xa0\x80',b'\xed\xbf\xbf',b'\xf4\x90\x80\x80',b'\xf5\x80\x80\x80',b'\xff',b'\xfe',
           b'\x80',b'\xbf',b'\xc2',b'\xe0\xa0',b'\xf0\x90\x80',b'\xc2a',b'\xe0\xa0a',b'\xf0\x90\x80a',b'a\x80',b'\xc2\x80\x80','aé中😀'.encode()]
    leads=[0x7f,0x80,0xc1,0xc2,0xdf,0xe0,0xed,0xef,0xf0,0xf4,0xf5,0xff]
    seconds=[0x00,0x7f,0x80,0x8f,0x90,0x9f,0xa0,0xbf,0xc0,0xff]
    cases+=[bytes([a,b,0x80,0x80]) for a in leads for b in seconds]
    body=''
    want=''
    for data in cases:
        ints=', '.join(map(str,data))
        body+=f'    let v{len(want.splitlines())} [{len(data)}]int = [{ints}]\n' if data else f'    let v{len(want.splitlines())} []int = []\n'
        k=len(want.splitlines())
        body+=f'    let b{k} = unwrap(bytes.from_ints(v{k}))\n    let c{k}, s{k} = encoding.utf8_decode(b{k}, 0)\n    print(encoding.utf8_valid(b{k}), is_ok(bytes.to_text(b{k})), c{k}, s{k})\n'
        ok=strict(data) is not None;c,s=first_scalar(data)
        want+=f'{"true" if ok else "false"} {"true" if ok else "false"} {c} {s}\n'
    source_text='import std.bytes as bytes\nimport std.encoding as encoding\nfn main() {\n'+body+'}\n'
    same(source_text,want)
    # utf8_decode offsets: scanning, out of range and mid-character positions.
    same('''import std.bytes as bytes
import std.encoding as encoding
fn main() {
    let data = bytes.from_text("aé中😀")
    var i = 0
    while i < bytes.len(data) {
        let c, n = encoding.utf8_decode(data, i)
        print(i, c, n)
        i += n
    }
    let a, b = encoding.utf8_decode(data, 2)
    let d, e = encoding.utf8_decode(data, -1)
    let f, g = encoding.utf8_decode(data, bytes.len(data))
    print(a, b, d, e, f, g)
}
''','0 97 1\n1 233 2\n3 20013 3\n6 128512 4\n-1 0 -1 0 -1 0\n')
    # Runtime errors on three paths.
    for body,error in [('print(strings.center("a", -1, "*"))','E4003'),('print(strings.center("a", 3, ""))','E4003'),('print(strings.center("a", 3, "ab"))','E4003'),
                       ('print(len(strings.center("a", 20000000, "x")))','E4099'),
                       ('print(len(strings.codepoints(strings.repeat("x", 1000001))))','E4099'),
                       ('print(len(strings.chars(strings.repeat("x", 1000001))))','E4099')]:
        results,_=three('import std.strings as strings\nfn main() {\n    '+body+'\n}\n',1)
        assert all(error in e for _,e in results),(body,results)
    # Exactly at the item limit is allowed.
    same('import std.strings as strings\nfn main() {\n    print(len(strings.codepoints(strings.repeat("x", 1000000))), len(strings.chars(strings.repeat("x", 1000000))))\n}\n','1000000 1000000\n')
    # Static errors.
    for body,error in [('fn main() { print(strings.center(1, 2, "*")) }','E3004'),('fn main() { print(strings.center("a", "2", "*")) }','E3004'),('fn main() { print(strings.center("a", 2)) }','E3005'),
                       ('fn main() { print(strings.codepoints(1)) }','E3004'),('fn main() { print(strings.chars([1])) }','E3004'),
                       ('fn main() { print(strings.from_codepoints(["a"])) }','E3004'),('fn main() { print(strings.from_codepoints(1)) }','E3004'),
                       ('fn main() { print(strings.slice_runes("a", "0", 1)) }','E3004'),
                       ('fn main() {\n    let c = strings.codepoints("a")\n    c[0] = 1\n}','E3003'),('fn main() {\n    let c = strings.chars("a")\n    c[0] = "b"\n}','E3003'),
                       ('fn main() { print(encoding.utf8_valid("a")) }','E3004'),('fn main() { print(encoding.utf8_encode("a")) }','E3004'),
                       ('fn main() { print(encoding.utf8_decode("a", 0)) }','E3004'),
                       ('fn main() { let a = encoding.utf8_decode(bytes.from_text("a"), 0) }','E3006'),('fn main() { let a, b, c = encoding.utf8_decode(bytes.from_text("a"), 0) }','E3006')]:
        p=source('import std.strings as strings\nimport std.encoding as encoding\nimport std.bytes as bytes\n'+body+'\n');_,e=run(['check',p],1);assert error in e,(body,e)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,22);bad_file=base/'downgraded.huab';bad_file.write_bytes(bytes(data))
    _,e=run(['run',bad_file],1);assert 'E6002' in e,e
print(f'text/UTF-8 P1 checks passed: {checks}')
