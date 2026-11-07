# HUA-D038 P1: std.json construction, pretty output and JSONL (since 27); pretty and
# compact output are checked against Python's json module.
import json,sys
from three_path import Cases
with Cases(*sys.argv[1:],'hua-json-') as c:
    artifact=c.same('''import std.json as json
fn main() {
    let n = json.null()
    print(json.kind(n), json.is_null(n), unwrap(json.encode(n)))
    let a = json.array([unwrap(json.from(1)), unwrap(json.from("x")), unwrap(json.from(true)), n])
    print(unwrap(json.encode(a)), unwrap(json.len(a)))
    var fields = map[string]Json{"b": a, "a": unwrap(json.from(1.5))}
    let o = json.object(fields)
    print(unwrap(json.encode(o)))
    let empty_list []Json = []
    print(unwrap(json.encode(json.array(empty_list))), unwrap(json.encode(json.object(map[string]Json{}))))
    print(unwrap(json.encode(unwrap(json.from([1, 2])))), unwrap(json.encode(unwrap(json.from(map[string]int{"k": 7})))))
    print(unwrap(json.encode(unwrap(json.from(a)))))
    print(json.from(map[int]int{1: 2}))
}
''','''null true null
[1,"x",true,null] 4
{"a":1.5,"b":[1,"x",true,null]}
[] {}
[1,2] {"k":7}
[1,"x",true,null]
err(JSON objects require string Map keys)
''')
    c.header(artifact,27)
    # Persistent updates leave the original untouched.
    c.same('''import std.json as json
fn main() {
    let o = unwrap(json.decode("{\\"a\\":1,\\"b\\":[1,2]}"))
    let o2 = unwrap(json.set(o, "c", unwrap(json.from(3))))
    let o3 = unwrap(json.set(o2, "a", json.null()))
    let o4 = unwrap(json.remove(o3, "b"))
    print(unwrap(json.encode(o)), unwrap(json.encode(o2)), unwrap(json.encode(o3)), unwrap(json.encode(o4)))
    let b = unwrap(json.get(o, "b"))
    let b2 = unwrap(json.append(b, unwrap(json.from(3))))
    let b3 = unwrap(json.set_at(b2, 0, unwrap(json.from("z"))))
    print(unwrap(json.encode(b)), unwrap(json.encode(b2)), unwrap(json.encode(b3)))
    print(json.set(b, "k", b), json.remove(b, "k"), json.remove(o, "missing"))
    print(json.append(o, b), json.set_at(o, 0, b), json.set_at(b, 2, b), json.set_at(b, -1, b))
}
''','''{"a":1,"b":[1,2]} {"a":1,"b":[1,2],"c":3} {"a":null,"b":[1,2],"c":3} {"a":null,"c":3}
[1,2] [1,2,3] ["z",2,3]
err(JSON value is not an object) err(JSON value is not an object) err(JSON object key is missing)
err(JSON value is not an array) err(JSON value is not an array) err(JSON array index out of bounds) err(JSON array index out of bounds)
''')
    # encode_pretty against json.dumps with sorted keys.
    value={'z':[1,[2,[]],{}],'a':{'k':'中"\\\n','n':None,'t':True,'f':False},'':[]}
    text=json.dumps(value,ensure_ascii=False)
    literal=text.replace('\\','\\\\').replace('"','\\"')
    body=''.join(f'    print(unwrap(json.encode_pretty(v, {n})))\n' for n in (1,2,4,16))
    c.same(f'import std.json as json\nfn main() {{\n    let v = unwrap(json.decode("{literal}"))\n{body}    print(unwrap(json.encode_pretty(1, 2)), unwrap(json.encode_pretty([1], 1)))\n}}\n',
           ''.join(json.dumps(value,ensure_ascii=False,sort_keys=True,indent=n).replace('\\n','\\u000a')+'\n' for n in (1,2,4,16))+'1 [\n 1\n]\n')
    # JSONL round trip, blank lines, CRLF and the failing line number.
    c.same('''import std.json as json
fn main() {
    let rows = unwrap(json.decode_lines("{\\"a\\":1}\\n\\n  \\r\\n[1,2]\\r\\n7"))
    print(len(rows), unwrap(json.encode(rows[0])), unwrap(json.encode(rows[1])), unwrap(json.encode(rows[2])))
    print("[" + unwrap(json.encode_lines(rows)) + "]")
    print(json.decode_lines("1\\n\\n{bad\\n2"))
    print(len(unwrap(json.decode_lines(""))), len(unwrap(json.decode_lines("\\n\\n"))))
    let none []Json = []
    print("[" + unwrap(json.encode_lines(none)) + "]")
}
''','''3 {"a":1} [1,2] 7
[{"a":1}
[1,2]
7
]
err(line 3: JSON byte 2: expected string)
0 0
[]
''')
    # Nesting stops at 64 levels whichever way the value is built, so one-level-at-a-time
    # loops cannot build a tree whose release would overflow the host stack.
    nest='''import std.json as json
fn main() {
    var j = json.null()
    for i in 0..64 { j = json.array([j]) }
    print(json.kind(j), len(unwrap(json.encode(j))))
'''
    c.same(nest+'}\n','array 132\n')
    c.fails(nest+'    print(json.kind(json.array([j])))\n}\n','E4099')
    c.fails(nest+'    print(json.kind(json.object(map[string]Json{"k": j})))\n}\n','E4099')
    c.same('''import std.json as json
fn main() {
    var j = json.null()
    for i in 0..64 { j = json.array([j]) }
    let shallow = unwrap(json.decode("[]"))
    let holder = unwrap(json.decode("{}"))
    print(json.append(shallow, j))
    print(json.set(holder, "k", j))
    print(json.set_at(unwrap(json.decode("[0]")), 0, j))
    print(json.from([j]))
}
''','err(JSON nesting exceeds 64)\n'*4)
    # Sharing a subtree cannot make output explode: the node limit reports an Err.
    c.same('''import std.json as json
fn main() {
    var j = unwrap(json.from(1))
    for i in 0..40 { j = json.array([j, j]) }
    print(json.kind(j))
    print(json.encode(j))
    print(json.encode_pretty(j, 1))
    print(json.encode_lines([j]))
}
''','array\nerr(JSON nesting/node limit exceeded)\nerr(JSON text exceeds 16 MiB)\nerr(JSON nesting/node limit exceeded)\n')
    c.fails('import std.json as json\nfn main() { print(json.encode_pretty(1, 0)) }\n','E4003')
    c.fails('import std.json as json\nfn main() { print(json.encode_pretty(1, 17)) }\n','E4003')
    for body,error in [('json.array([1])','E3004'),('json.array(1)','E3004'),('json.object(map[string]int{})','E3004'),('json.set(1, "a", json.null())','E3004'),('json.set(json.null(), 1, json.null())','E3004'),('json.append(json.null())','E3005'),('json.set_at(json.null(), "0", json.null())','E3004'),('json.encode_pretty(1)','E3005'),('json.encode_pretty(1, "2")','E3004'),('json.decode_lines(1)','E3004'),('json.encode_lines([1])','E3004'),('json.null(1)','E3005')]:
        c.static('import std.json as json\nfn main() { print('+body+') }\n',error)
print(f'json construct checks passed: {c.checks}')
