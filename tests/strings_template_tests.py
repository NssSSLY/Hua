# HUA-D038 P1: strings.template, format specs and grouped number formatting (since 25).
import sys
from three_path import Cases
with Cases(*sys.argv[1:],'hua-template-') as c:
    artifact=c.same('''import std.strings as strings
fn main() {
    var m = map[string]string{"name": "Hua", "n": "中文", "unused": "x"}
    print(unwrap(strings.template("hi {name}, {{x}} {n}", m)))
    print("[" + unwrap(strings.template("{name:>6}|{name:<6}|{name:^6}|{name:*^7}|{n:中>4}|{name:2}|{name:6}", m)) + "]")
    print("[" + unwrap(strings.template("", m)) + "]", unwrap(strings.template("plain", m)))
    print(strings.template("{missing}", m))
    print(strings.template("{name", m))
    print(strings.template("}", m))
    print(strings.template("{name:x}", m))
    print(strings.template("{name:>x}", m))
    print(strings.template("{name:*>}", m), strings.template("{}", m))
}
''','''hi Hua, {x} 中文
[   Hua|Hua   | Hua  |**Hua**|中中中文|Hua|Hua   ]
[] plain
err(FORMAT_ARGS: missing template key missing)
err(FORMAT_ARGS: unclosed '{')
err(FORMAT_ARGS: unmatched '}')
err(FORMAT_ARGS: invalid format spec)
err(FORMAT_ARGS: invalid format spec)
ok(Hua) err(FORMAT_ARGS: missing template key )
''')
    c.header(artifact,25)
    # strings.format gains the same specs; existing behaviour is covered by strings_p0.
    c.same('''import std.strings as strings
fn main() {
    print("[" + unwrap(strings.format("{0:>4}|{:<4}|{:^5}|{1:*>3}", ["a", "b"])) + "]")
    print(strings.format("{:x}", ["a"]))
    print(strings.format("{x:>3}", ["a"]))
    print(strings.format("{5:>3}", ["a"]))
}
''','''[   a|a   |  b  |**b]
err(FORMAT_ARGS: invalid format spec)
err(FORMAT_ARGS: placeholder must be {} or {index})
err(FORMAT_ARGS: placeholder index out of range)
''')
    # Grouping against Python's own "," format, including the int bounds.
    ints=[0,5,-5,999,1000,-1000,12345,-999999,1000000,1234567,-1234567,9223372036854775807,-9223372036854775808]
    body=''.join(f'    print(strings.format_int_grouped({v}, ","))\n' for v in ints if v!=-9223372036854775808)
    want=''.join(f'{v:,}\n' for v in ints if v!=-9223372036854775808)
    c.same('import std.strings as strings\nfn main() {\n'+body+'    print(strings.format_int_grouped(-9223372036854775807 - 1, ","))\n    print(strings.format_int_grouped(1234567, ""), strings.format_int_grouped(1234567, "中"), strings.format_int_grouped(1234567, "_"))\n}\n',want+'-9,223,372,036,854,775,808\n1234567 1中234中567 1_234_567\n')
    floats=[(0.0,2),(1234.5,2),(-1234.5,1),(999.999,2),(1000000.0,0),(-0.5,0),(1234567.891,3),(0.125,2),(1e15,1)]
    c.same('import std.strings as strings\nfn main() {\n'+''.join(f'    print(strings.format_float_grouped({x!r}, {p}, ","))\n' for x,p in floats)+'}\n',''.join(f'{x:,.{p}f}\n' for x,p in floats))
    for body,error in [('strings.format_int_grouped(1, "ab")','E4003'),('strings.format_float_grouped(1.0, 2, "ab")','E4003'),('strings.format_float_grouped(1.0, 18, ",")','E4003'),('strings.format_float_grouped(1.0, -1, ",")','E4003')]:
        c.fails('import std.strings as strings\nfn main() { print('+body+') }\n',error)
    c.fails('import std.strings as strings\nfn main() {\n    var m = map[string]string{"a": "b"}\n    print(strings.template("{a:99999999}", m))\n}\n','E4099')
    c.fails('import std.strings as strings\nfn main() {\n    print(strings.format("{:99999999999999999999}", ["a"]))\n}\n','E4099')
    for body,error in [('strings.template(1, map[string]string{})','E3004'),('strings.template("a", map[string]int{})','E3004'),('strings.template("a")','E3005'),('strings.format_int_grouped("1", ",")','E3004'),('strings.format_int_grouped(1, 2)','E3004'),('strings.format_float_grouped("1", 2, ",")','E3004')]:
        c.static('import std.strings as strings\nfn main() { print('+body+') }\n',error)
print(f'strings template checks passed: {c.checks}')
