"""Compare the Hua-written lexer to the independent C++ frontend."""
import json
import subprocess
import sys
from pathlib import Path

hua, oracle, root, build = (Path(p).resolve() for p in sys.argv[1:])
work = build / "selfhost-tests"
work.mkdir(parents=True, exist_ok=True)
seed = root / "selfhost" / "main.hua"
checks = 0
def run(args):
    global checks
    result = subprocess.run([str(p) for p in args], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=40)
    assert result.returncode == 0, (args, result.returncode, result.stdout, result.stderr)
    checks += 1
    return result.stdout

def tokens(text):
    if text.startswith("error "): return text.strip()
    return [(a, int(b), int(c), json.loads(d)) for a,b,c,d in (line.split(" ", 3) for line in text.splitlines())]

# Compile a copied seed, then remove its source modules. The archive must stand alone.
copy = work / "source"
copy.mkdir(exist_ok=True)
for name in ("main.hua", "lexer.hua"):
    (copy / name).write_bytes((root / "selfhost" / name).read_bytes())
artifact = work / "lexer.huab"
run([hua, "build", copy / "main.hua", "-o", artifact])
for name in ("main.hua", "lexer.hua"):
    path = (copy / name).resolve()
    assert path.parent == copy.resolve()
    path.unlink()

cases = [
    "", "fn main() {\n print(3//2, 2**4)\n}\n", "# line\n## doc\n#! shebang\nlet x=1\n",
    "#* outer #* inner *#\n*# x", "#** documentation #* ordinary *# **#\n",
    "#* a #** doc **# *#", "#** a #** nested **# **#", "#** a *# not closing **#",
    "0 1_000 0xAb_ff 0b10_01 0o77 1.2 2e+3 3.4e-2 1..=2\n",
    '"中😀" "a\\n\\t\\0\\r\\\\\\\""\n',
    "let var const fn return struct interface pub if else match enum for while break continue in import as async await spawn taskgroup parallel simd defer unsafe extern true false nil name\n",
    "..= **= //= .. ** // ++ -- += -= *= /= %= => == != <= >= && || << >> ( ) [ ] { } , : . ? @ + - * / % = < > ! & | ^ ~\n",
    "\ufefffn main() {\r\n}\r", "\t# comment 中\nname", "#* 注释\n*# \"字符串\"",
    "1__2", "0x", "0b2", "1_", "1e+", "1.2.3", "7suffix", '"bad\\q"',
    '"unterminated', '"line\n"', '"raw\x01"', "#* missing", "#** missing *#", ";", "中文", '"bad\\',
]
fixtures = []
for i, source in enumerate(cases):
    path = work / f"case{i}.hua"
    path.write_bytes(source.encode("utf-8"))
    fixtures.append(path)
fixtures += sorted((root / "tests" / "spec").glob("*.hua"))
fixtures += [root / "examples" / "concurrency.hua", root / "examples" / "language_features.hua", root / "selfhost" / "lexer.hua", seed]
for fixture in fixtures:
    expected = tokens(run([oracle, fixture]))
    for command in ("interpret", "run"):
        actual = tokens(run([hua, command, seed, "--", fixture]))
        assert actual == expected, (fixture, command, actual[:4], expected[:4])
    assert tokens(run([hua, "run", artifact, "--", fixture])) == expected, (fixture,"persisted")
print(f"{len(fixtures)} selfhost lexer fixtures, {checks} process checks; C++/Interpreter/VM/source-free HUAB agree")
