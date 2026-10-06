"""CLI contracts: exit status, diagnostics, Unicode paths and complete AST output."""
from pathlib import Path
import subprocess
import sys
import tempfile

exe, root = Path(sys.argv[1]), Path(sys.argv[2])
checks = 0

def run(args, code, stdout=None, stderr=None):
    global checks
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True,
                            text=True, encoding="utf-8", timeout=10)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    if stdout is not None:
        assert stdout in result.stdout, (args, result.stdout)
    if stderr is not None:
        assert stderr in result.stderr, (args, result.stderr)
    checks += 1
    return result

run(["version"], 0, "Hua 0.1.0-dev\nspec 0.1\nabi 1\nbytecode 6\n")
run(["--version"], 0, "Hua 0.1.0-dev")
run(["--help"], 0, "Usage:")
for args in ([], ["run"], ["ast"], ["check"], ["version", "extra"], ["unknown", "file"]):
    run(args, 2, stderr="Usage:")
run(["check", root / "does-not-exist.hua"], 1, stderr="error[E0001]")
for path in sorted((root / "examples").glob("*.hua")):
    run(["check", path], 0, "OK\n")
    run(["ast", path], 0, "(Program")
result = run(["check", root / "tests/spec/101_invalid_binding.hua"], 1,
             stderr="101_invalid_binding.hua:1:5")
assert not result.stdout and "let = 1" in result.stderr and "^" in result.stderr
assert "error[E2001]" in result.stderr
with tempfile.TemporaryDirectory(prefix="hua-cli-", dir=root / "build") as temp:
    path = Path(temp) / "中文路径 测试.hua"
    path.write_bytes(b"\xef\xbb\xbffn main() {\r\n    print(\"Hua\")\r\n}\r\n")
    run(["check", path], 0, "OK\n")
    path.write_text('let text = "中文"\n', encoding="utf-8")
    run(["ast", path], 0, '(String "中文")')
    path.write_bytes(b'let x = "\xc0\xaf"\n')
    run(["check", path], 1, stderr="error[E1009]")
    path.write_text("let x = 5 // 2\n", encoding="utf-8")
    run(["check", path], 0, "OK\n")
    path.write_text("print(5 / 2, 5 // 2)\n", encoding="utf-8")
    run(["run", path], 0, "2.5 2\n")
    path.write_bytes(b"\xef\xbb\xbf#!/usr/bin/env hua\r\n## docs\r\n#** docs #* nested *# **#\r\nprint(7/2,7//2,7%2,2**3)# tail\r\n")
    run(["check", path], 0, "OK\n")
    run(["run", path], 0, "3.5 3 1 8\n")
    path.write_text("// old comment\n", encoding="utf-8")
    run(["check", path], 1, stderr="error[E2001]")
    path.write_text("#* outer #* inner *#\n", encoding="utf-8")
    run(["check", path], 1, stderr="error[E1005]")
    path.write_text("print(\"must not run\")\nlet x=1\nx=2\n", encoding="utf-8")
    result = run(["run", path], 1, stderr="error[E3003]")
    assert not result.stdout
    path.write_bytes(b" " * (16 * 1024 * 1024 + 1))
    run(["check", path], 1, stderr="exceeds 16 MiB")
print(f"{checks} CLI contract checks passed")
