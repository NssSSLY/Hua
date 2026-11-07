# HUA-D038 P1: encoding pack/unpack (since 24), checked against Python struct.
import math,struct,sys
from three_path import Cases
codes={1:'B',2:'H',4:'I',8:'Q'}
with Cases(*sys.argv[1:],'hua-pack-') as c:
    # Round trips and exact bytes at every width, order and range boundary.
    prog=['import std.encoding as e','fn main() {'];want=[]
    for width,code in codes.items():
        for order,sign in (('le','<'),('be','>')):
            top=2**(8*width)-1 if width<8 else 2**63-1
            for v in (0,1,top//2,top):
                prog.append(f'    print(e.hex_encode(unwrap(e.pack_uint({v}, {width}, "{order}"))), unwrap(e.unpack_uint(unwrap(e.pack_uint({v}, {width}, "{order}")), 0, {width}, "{order}")))')
                want.append(f'{struct.pack(sign+code,v).hex()} {v}')
            low=-2**(8*width-1);high=2**(8*width-1)-1
            for v in (low,-1,0,1,high):
                prog.append(f'    print(e.hex_encode(unwrap(e.pack_int({v}, {width}, "{order}"))), unwrap(e.unpack_int(unwrap(e.pack_int({v}, {width}, "{order}")), 0, {width}, "{order}")))')
                want.append(f'{struct.pack(sign+code.lower(),v).hex()} {v}')
    artifact=c.same('\n'.join(prog+['}','']),'\n'.join(want)+'\n')
    c.header(artifact,24)
    # Offsets, truncation and ranges are Err, never a diagnostic.
    c.same('''import std.encoding as e
import std.bytes as bytes
fn main() {
    let b = unwrap(bytes.from_ints([1, 2, 3, 4, 255, 255, 255, 255, 255, 255, 255, 255]))
    print(unwrap(e.unpack_uint(b, 1, 2, "be")), unwrap(e.unpack_uint(b, 1, 2, "le")), unwrap(e.unpack_int(b, 4, 8, "le")))
    print(e.unpack_uint(b, 4, 8, "le"))
    print(e.unpack_uint(b, 9, 4, "le"))
    print(e.unpack_uint(b, 12, 1, "le"))
    print(e.unpack_int(b, -1, 1, "le"))
    print(unwrap(e.unpack_uint(b, 11, 1, "le")))
    print(e.pack_uint(256, 1, "le"), e.pack_uint(-1, 8, "le"), e.pack_uint(65536, 2, "be"), e.pack_uint(4294967296, 4, "be"))
    print(e.pack_int(128, 1, "le"), e.pack_int(-129, 1, "le"), e.pack_int(32768, 2, "be"), e.pack_int(2147483648, 4, "be"))
}
''','515 770 -1\nerr(ENC_RANGE: value does not fit in int)\n'+'err(ENC_TRUNCATED: not enough bytes)\n'*3+'255\n'+' '.join(['err(ENC_RANGE: value out of range for width)']*4)+'\n'+' '.join(['err(ENC_RANGE: value out of range for width)']*4)+'\n')
    # Floats against struct, including signed zero and f32 rounding.
    values=[0.0,-0.0,1.5,-2.25,1e-30,1e30,3.141592653589793]
    prog=['import std.encoding as e','fn main() {'];want=[]
    for v in values:
        for order,sign in (('le','<'),('be','>')):
            prog.append(f'    print(e.hex_encode(unwrap(e.pack_f64({v!r}, "{order}"))), e.hex_encode(unwrap(e.pack_f32({v!r}, "{order}"))))')
            want.append(struct.pack(sign+'d',v).hex()+' '+struct.pack(sign+'f',v).hex())
    prog.append('    print(unwrap(e.unpack_f64(unwrap(e.pack_f64(1.5, "be")), 0, "be")), unwrap(e.unpack_f32(unwrap(e.pack_f32(-2.25, "le")), 0, "le")))')
    want.append('1.5 -2.25')
    c.same('\n'.join(prog+['}','']),'\n'.join(want)+'\n')
    c.same('''import std.encoding as e
import std.bytes as bytes
fn main() {
    print(e.pack_f32(1.0e39, "le"), e.pack_f32(-1.0e39, "be"))
    print(e.hex_encode(unwrap(e.pack_f32(1.0e-50, "le"))))
    let nan = unwrap(bytes.from_ints([0, 0, 192, 127]))
    let inf = unwrap(bytes.from_ints([0, 0, 0, 0, 0, 0, 240, 127]))
    print(e.unpack_f32(nan, 0, "le"), e.unpack_f64(inf, 0, "le"))
    print(e.unpack_f64(nan, 0, "le"), e.unpack_f32(nan, 1, "le"), e.unpack_f32(nan, -1, "le"))
}
''','err(ENC_RANGE: value out of range for width) err(ENC_RANGE: value out of range for width)\n00000000\nerr(ENC_RANGE: value is not finite) err(ENC_RANGE: value is not finite)\nerr(ENC_TRUNCATED: not enough bytes) err(ENC_TRUNCATED: not enough bytes) err(ENC_TRUNCATED: not enough bytes)\n')
    # Bad width/order are programming errors on all three paths.
    for body in ('e.pack_uint(1, 3, "le")','e.pack_int(1, 0, "le")','e.pack_uint(1, 16, "le")','e.pack_uint(1, 1, "LE")','e.pack_int(1, 1, "")','e.unpack_uint(b, 0, 5, "le")','e.unpack_int(b, 0, 1, "x")','e.pack_f64(1.0, "x")','e.pack_f32(1.0, "x")','e.unpack_f64(b, 0, "x")','e.unpack_f32(b, 0, "x")','e.pack_f64(1, "le")'):
        c.fails('import std.encoding as e\nimport std.bytes as bytes\nfn main() {\n    let b = bytes.from_text("abcdefgh")\n    print('+body+')\n}\n','E4003')
    for body in ('e.pack_uint("1", 1, "le")','e.pack_uint(1, 1)','e.pack_int(1.5, 1, "le")','e.unpack_uint("a", 0, 1, "le")','e.pack_f64(v, "le")','e.unpack_f64("a", 0, "le")'):
        c.static('import std.encoding as e\nfn main() {\n    let v = 1\n    print('+body+')\n}\n','E3005' if body=='e.pack_uint(1, 1)' else 'E3004')
print(f'encoding pack checks passed: {c.checks}')
