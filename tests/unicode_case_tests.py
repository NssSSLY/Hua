# HUA-D038 P1: strings upper/lower/casefold/equal_fold (since 26), checked against
# Python's str methods over every Unicode scalar and over final sigma contexts.
import random,sys
from pathlib import Path
from three_path import Cases
scalars=[chr(c) for c in range(0x110000) if not 0xD800<=c<=0xDFFF]
with Cases(*sys.argv[1:],'hua-unicode-') as c:
    # Every scalar on its own line, so no context crosses between them.
    all_lines='\n'.join(scalars)
    (c.base/'all.txt').write_bytes(all_lines.encode())
    artifact=c.same('''import std.fs as fs
import std.strings as strings
fn main() {
    let t = unwrap(fs.read_text("all.txt"))
    unwrap(fs.write_text("upper.txt", strings.upper(t)))
    unwrap(fs.write_text("lower.txt", strings.lower(t)))
    unwrap(fs.write_text("fold.txt", strings.casefold(t)))
    print(strings.equal_fold(t, strings.casefold(t)), strings.equal_fold(t, strings.upper(t)))
}
''','true false\n')
    c.header(artifact,26)
    assert (c.base/'upper.txt').read_bytes()==all_lines.upper().encode(),'upper differs from Python'
    assert (c.base/'lower.txt').read_bytes()==all_lines.lower().encode(),'lower differs from Python'
    assert (c.base/'fold.txt').read_bytes()==all_lines.casefold().encode(),'casefold differs from Python'
    # Final sigma against Python: handwritten contexts plus random mixes of cased letters,
    # case-ignorable marks and uncased characters around capital sigma.
    picks=['Σ','A','b','Α','.',"'",'́','­',' ','1','σ','İ','ß','ﬁ','ᾈ','ͅ','\U00010400']
    rng=random.Random(20261107)
    samples=['Σ','AΣ','AΣB','AΣ B','A.Σ','AΣ.','AΣ.B',' Σ','1Σ','ÁΣ','AΣ́','AΣ́B','İ','ß','ﬁ']
    samples+=[''.join(rng.choice(picks) for _ in range(rng.randint(1,8))) for _ in range(400)]
    (c.base/'mix.txt').write_bytes('\n'.join(samples).encode())
    c.same('''import std.fs as fs
import std.strings as strings
fn main() {
    let lines = strings.split(unwrap(fs.read_text("mix.txt")), "\\n")
    var out = ""
    for s in lines { out = out + strings.upper(s) + "|" + strings.lower(s) + "|" + strings.casefold(s) + "\\n" }
    unwrap(fs.write_text("mix_out.txt", out))
    print(len(lines))
}
''',f'{len(samples)}\n')
    want=''.join(f'{s.upper()}|{s.lower()}|{s.casefold()}\n' for s in samples)
    assert (c.base/'mix_out.txt').read_bytes().decode()==want,'mixed case mapping differs from Python'
    # equal_fold equals casefold comparison on pairs, in both directions.
    pairs=[('','',),('a','A'),('ß','SS'),('ß','ss'),('ẞ','ss'),('ﬁ','FI'),('ﬁ','fi'),('Σ','σ'),('Σ','ς'),('İ','i'),('İ','i̇'),('é','É'),('é','é'),('a','b'),('a','ab'),('ab','a'),('中','中'),('ǅ','ǆ'),('straße','STRASSE'),('ﬃ','FFI'),('ﬃ','FF')]
    body=''.join(f'    print(strings.equal_fold("{a}", "{b}"), strings.equal_fold("{b}", "{a}"))\n' for a,b in pairs)
    c.same('import std.strings as strings\nfn main() {\n'+body+'}\n',''.join((f'{str(a.casefold()==b.casefold()).lower()} '*2).strip()+'\n' for a,b in pairs))
    c.fails('import std.strings as strings\nfn main() { print(len(strings.upper(strings.repeat("ΐ", 3000000)))) }\n','E4099')
    c.fails('import std.strings as strings\nfn main() { print(len(strings.casefold(strings.repeat("ΐ", 3000000)))) }\n','E4099')
    c.same('import std.strings as strings\nfn main() { print(strings.equal_fold(strings.repeat("ﬃ", 2000000), strings.repeat("FFI", 2000000))) }\n','true\n')
    for body,error in [('strings.upper(1)','E3004'),('strings.lower(1)','E3004'),('strings.casefold([1])','E3004'),('strings.equal_fold("a", 1)','E3004'),('strings.equal_fold("a")','E3005'),('strings.upper("a", "b")','E3005')]:
        c.static('import std.strings as strings\nfn main() { print('+body+') }\n',error)
print(f'unicode case checks passed: {c.checks}')
