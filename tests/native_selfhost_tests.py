# The selfhost compiler built by the native backend must behave exactly like the same
# compiler running on the VM: identical archive bytes, stdout, stderr and exit code, for
# valid inputs and for an input with an error.
import pathlib, shutil, subprocess, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / 'tools'))
import native_build
hua = pathlib.Path(sys.argv[1]).resolve(); root = pathlib.Path(sys.argv[2]).resolve()
work = pathlib.Path(tempfile.mkdtemp(prefix='hua-native-selfhost-', dir=sys.argv[3] if len(sys.argv) > 3 else None))
shutil.copy(hua.parent / 'wasmtime.dll', work / 'wasmtime.dll')
def run(command):
    p = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=root, timeout=600)
    return p.returncode, p.stdout, p.stderr.replace(str(work), '<work>')
native = work / 'compiler.exe'
code, message = native_build.build(hua, root / 'selfhost/archive_main.hua', native)
assert code == 0, message[-2000:]
vm_compiler = work / 'compiler.huab'
assert run([str(hua), 'build', 'selfhost/archive_main.hua', '-o', str(vm_compiler)])[0] == 0
bad = work / 'bad.hua'; bad.write_text('fn f(x int) int { return x + "a" }\nfn main() { print(f(1)) }\n', encoding='utf-8')
inputs = [root / 'selfhost/type_rules.hua', root / 'selfhost/ast_pool.hua', root / 'selfhost/lexer.hua', root / 'selfhost/bytecode.hua', bad]
checks = 0
for source in inputs:
    a, b = work / 'native.huab', work / 'vm.huab'
    for f in (a, b): f.unlink(missing_ok=True)
    left = run([str(native), str(source), str(a)])
    right = run([str(hua), 'run', str(vm_compiler), '--execution-profile', 'compiler', '--', str(source), str(b)])
    assert left == (right[0], right[1], right[2]), (source.name, left, right)
    assert a.exists() == b.exists() and (not a.exists() or a.read_bytes() == b.read_bytes()), (source.name, 'archive bytes differ')
    checks += 1
shutil.rmtree(work, ignore_errors=True)
print(f'native selfhost compiler identical to the VM ({checks} inputs)')
