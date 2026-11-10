"""Build a native executable from a Hua program (prototype driver).

usage: python native_build.py <hua.exe> <program.hua> <out.exe>

Runs `hua native` to generate C++, then compiles it with the repository's clang and links
the runtime library next to hua.exe. Exit code 2 means the program is outside the native
backend's current subset (the reason is printed).
"""
import pathlib, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[2]
clang = root / '.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe'
wasmtime = root / '.tools/wasmtime-v49.0.2-x86_64-windows-c-api/lib/wasmtime.dll.lib'
def build(hua, program, out):
    hua = pathlib.Path(hua).resolve(); out = pathlib.Path(out).resolve(); cpp = out.with_suffix('.cpp')
    generated = subprocess.run([str(hua), 'native', str(program), '-o', str(cpp)], capture_output=True, text=True, encoding='utf-8')
    if generated.returncode: return generated.returncode, generated.stderr
    lib = hua.parent
    compiled = subprocess.run([str(clang), '-std=c++20', '-O2', '-DNDEBUG', '-I', str(root / 'include'), str(cpp), '-o', str(out),
                               str(lib / 'libhua_semantics.a'), str(lib / 'libhua_frontend.a'), '-lpthread', '-lbcrypt', str(wasmtime),
                               '-static', '-Wl,--stack,8388608'], capture_output=True, text=True, encoding='utf-8', errors='replace')
    if compiled.returncode: return 3, compiled.stderr
    return 0, ''
if __name__ == '__main__':
    code, message = build(*sys.argv[1:4])
    if message: sys.stderr.write(message)
    sys.exit(code)
