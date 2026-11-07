# HUA-D038 P1: file.read_line/read_all/read_exact/write_all (since 29). The read-ahead must
# stay invisible: every mix of read*, tell, seek and write sees the same file as without it.
import sys
from three_path import Cases
head='import std.file as f\nimport std.bytes as b\nimport std.error as e\nimport std.strings as s\n'
with Cases(*sys.argv[1:],'hua-file-bulk-') as c:
    # Lines: LF, CRLF, blank, no trailing newline, UTF-8, a line longer than the read-ahead.
    long_line='x'*200000
    (c.base/'lines.txt').write_bytes(f'one\ntwo\r\n\n\r\n中文\n{long_line}\nlast'.encode())
    artifact=c.same(head+'''fn main() {
    let h = unwrap(f.open("lines.txt", "r"))
    print("[" + unwrap(unwrap(f.read_line(h))) + "]")
    print("[" + unwrap(unwrap(f.read_line(h))) + "]")
    print("[" + unwrap(unwrap(f.read_line(h))) + "]")
    print("[" + unwrap(unwrap(f.read_line(h))) + "]")
    print("[" + unwrap(unwrap(f.read_line(h))) + "]")
    print(len(unwrap(unwrap(f.read_line(h)))))
    print("[" + unwrap(unwrap(f.read_line(h))) + "]")
    print(unwrap(f.read_line(h)), unwrap(f.read_line(h)))
    unwrap(f.close(h))
    print(e.code(f.error(unwrap_err(f.read_line(h)))))
}
''','[one]\n[two]\n[]\n[]\n[中文]\n200000\n[last]\nnil nil\nclosed\n')
    c.header(artifact,29)
    (c.base/'empty.txt').write_bytes(b'')
    (c.base/'nl.txt').write_bytes(b'a\n')
    (c.base/'cr.txt').write_bytes(b'a\r')
    c.same(head+'''fn main() {
    let a = unwrap(f.open("empty.txt", "r"))
    print(unwrap(f.read_line(a)), bytes_len(unwrap(f.read_all(a))), unwrap(f.read_exact(a, 3)))
    let n = unwrap(f.open("nl.txt", "r"))
    print(unwrap(unwrap(f.read_line(n))), unwrap(f.read_line(n)))
    let r = unwrap(f.open("cr.txt", "r"))
    print(len(unwrap(unwrap(f.read_line(r)))))
}
fn bytes_len(x Bytes) int { return b.len(x) }
''','nil 0 nil\na nil\n2\n')
    # The read-ahead is invisible to tell, read, seek and the other read functions.
    (c.base/'mix.txt').write_bytes(b'alpha\nbeta\ngamma\ndelta\n')
    c.same(head+'''fn text(x Bytes) string { return unwrap(b.to_text(x)) }
fn main() {
    let h = unwrap(f.open("mix.txt", "r"))
    print(unwrap(unwrap(f.read_line(h))), unwrap(f.tell(h)))
    print(text(unwrap(unwrap(f.read(h, 3)))), unwrap(f.tell(h)))
    print(unwrap(unwrap(f.read_line(h))), unwrap(f.tell(h)))
    print(text(unwrap(unwrap(f.read_exact(h, 2)))), unwrap(f.tell(h)))
    print(unwrap(unwrap(f.read_line(h))), unwrap(f.tell(h)))
    print(unwrap(f.seek(h, 1, "start")), unwrap(unwrap(f.read_line(h))), unwrap(f.tell(h)))
    print(unwrap(f.seek(h, 2, "current")), unwrap(unwrap(f.read_line(h))))
    print(unwrap(f.seek(h, -3, "end")), unwrap(unwrap(f.read_line(h))))
    print(unwrap(f.seek(h, 0, "start")), unwrap(unwrap(f.read_line(h))), text(unwrap(f.read_all(h))), unwrap(f.tell(h)))
    print(b.len(unwrap(f.read_all(h))), unwrap(f.read(h, 4)), unwrap(f.read_line(h)))
}
''','alpha 6\nbet 9\na 11\nga 13\nmma 17\n1 lpha 6\n8 ta\n20 ta\n0 alpha beta\ngamma\ndelta\n 23\n0 nil nil\n')
    # Writes after read_line land at the logical position; append modes always at the end.
    c.same(head+'''fn text(x Bytes) string { return unwrap(b.to_text(x)) }
fn main() {
    let w = unwrap(f.open("rw.txt", "w"))
    unwrap(f.write_all(w, b.from_text("11\\n22\\n33\\n")))
    unwrap(f.close(w))
    let h = unwrap(f.open("rw.txt", "r+"))
    print(unwrap(unwrap(f.read_line(h))), unwrap(f.tell(h)))
    print(unwrap(f.write(h, b.from_text("XX"))), unwrap(f.tell(h)))
    print(unwrap(unwrap(f.read_line(h))))
    unwrap(f.seek(h, 0, "start"))
    print(unwrap(unwrap(f.read_line(h))))
    unwrap(f.write_all(h, b.from_text("YY")))
    unwrap(f.close(h))
    let g = unwrap(f.open("rw.txt", "r"))
    print(text(unwrap(f.read_all(g))))
    let a = unwrap(f.open("rw.txt", "a+"))
    unwrap(f.seek(a, 0, "start"))
    print(unwrap(unwrap(f.read_line(a))))
    unwrap(f.write(a, b.from_text("end\\n")))
    unwrap(f.seek(a, 0, "start"))
    print(text(unwrap(f.read_all(a))))
}
''','11 3\n2 5\n\n11\n11\nYY\n33\n\n11\n11\nYY\n33\nend\n\n')
    # read_exact and read_all limits and partial data.
    (c.base/'ten.bin').write_bytes(b'0123456789')
    c.same(head+'''fn text(x Bytes) string { return unwrap(b.to_text(x)) }
fn main() {
    let h = unwrap(f.open("ten.bin", "r"))
    print(text(unwrap(unwrap(f.read_exact(h, 4)))), b.len(unwrap(unwrap(f.read_exact(h, 0)))))
    let issue = unwrap_err(f.read_exact(h, 10))
    print(e.code(f.error(issue)), f.transferred(issue), text(unwrap(f.partial(issue))))
    print(unwrap(f.read_exact(h, 1)))
    print(e.code(f.error(unwrap_err(f.read_exact(h, -1)))), e.code(f.error(unwrap_err(f.read_exact(h, 134217729)))))
}
''','0123 0\nunexpected_eof 6 456789\nnil\nlimit limit\n')
    # Invalid UTF-8 is consumed with its bytes available, and reading can continue.
    (c.base/'bad.txt').write_bytes(b'ok\n\xff\xfe\nafter\n')
    c.same(head+'''fn main() {
    let h = unwrap(f.open("bad.txt", "r"))
    print(unwrap(unwrap(f.read_line(h))))
    let issue = unwrap_err(f.read_line(h))
    print(e.code(f.error(issue)), f.transferred(issue), unwrap(b.to_ints(unwrap(f.partial(issue)))))
    print(unwrap(unwrap(f.read_line(h))))
}
''','ok\ninvalid_utf8 2 [255, 254]\nafter\n')
    # A line beyond the limit is an Err; the bytes stay readable.
    (c.base/'huge.txt').write_bytes(b'y'*(17*1024*1024)+b'\ntail\n')
    c.same(head+'''fn main() {
    let h = unwrap(f.open("huge.txt", "r"))
    print(e.code(f.error(unwrap_err(f.read_line(h)))))
    print(unwrap(f.tell(h)))
    let rest = unwrap(f.read_all(h))
    print(b.len(rest))
}
''',f'limit\n0\n{17*1024*1024+6}\n')
    # write_all across host chunks, and wrong modes.
    size=3*1024*1024+17
    c.same(head+f'''fn main() {{
    let w = unwrap(f.open("big.bin", "w"))
    let data = b.from_text(s.repeat("z", {size}))
    print(is_ok(f.write_all(w, data)), unwrap(f.tell(w)))
    print(is_ok(f.write_all(w, b.from_text(""))))
    print(e.code(f.error(unwrap_err(f.read_line(w)))), e.code(f.error(unwrap_err(f.read_all(w)))), e.code(f.error(unwrap_err(f.read_exact(w, 1)))))
    unwrap(f.close(w))
    let r = unwrap(f.open("big.bin", "r"))
    print(b.len(unwrap(f.read_all(r))))
    print(e.code(f.error(unwrap_err(f.write_all(r, data)))))
}}
''',f'true {size}\ntrue\npermission_denied permission_denied permission_denied\n{size}\npermission_denied\n')
    assert (c.base/'big.bin').stat().st_size==size
    # read_all accepts exactly 128 MiB and stops beyond it with the data so far.
    limit=128*1024*1024
    with open(c.base/'full.bin','wb') as out:out.truncate(limit)
    with open(c.base/'over.bin','wb') as out:out.truncate(limit+1)
    c.same(head+'''fn main() {
    let full = unwrap(f.open("full.bin", "r"))
    print(b.len(unwrap(f.read_all(full))))
    let h = unwrap(f.open("over.bin", "r"))
    let issue = unwrap_err(f.read_all(h))
    print(e.code(f.error(issue)), f.transferred(issue), b.len(unwrap(f.partial(issue))))
}
''',f'{limit}\nlimit {limit+1} {limit+1}\n')
    # Inside a Task.
    c.same(head+'''fn work() int {
    let h = unwrap(f.open("mix.txt", "r"))
    var n = 0
    while true {
        let line = unwrap(f.read_line(h))
        if is_none(line) { break }
        n += 1
    }
    return n
}
async fn main() {
    print(await spawn work())
}
''','4\n')
    for body,error in [('f.read_line(1)','E3004'),('f.read_all("x")','E3004'),('f.read_exact(1, 1)','E3004'),('f.write_all(1, b.from_text(""))','E3004'),('f.read_line()','E3005')]:
        c.static(head+'fn main() { print('+body+') }\n',error)
print(f'file bulk checks passed: {c.checks}')
