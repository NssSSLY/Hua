"""Build a native executable from a Hua program (prototype driver).

usage: python native_build.py <hua.exe> <program.hua> <out.exe>

Runs `hua native` to generate C++, then compiles it with the repository's clang and links
the runtime library next to hua.exe. Exit code 2 means the program is outside the native
backend's current subset (the reason is printed).
"""
import pathlib, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools'))
from hua_native import link_cpp
def build(hua, program, out, *, count_steps=True):
    hua = pathlib.Path(hua).resolve(); out = pathlib.Path(out).resolve(); cpp = out.with_suffix('.cpp')
    generated = subprocess.run([str(hua), 'native', str(program), '-o', str(cpp)], capture_output=True, text=True, encoding='utf-8')
    if generated.returncode: return generated.returncode, generated.stderr
    compiled = link_cpp(hua, cpp, out, count_steps=count_steps)
    if compiled.returncode: return 3, compiled.stderr
    return 0, ''
if __name__ == '__main__':
    code, message = build(*sys.argv[1:4])
    if message: sys.stderr.write(message)
    sys.exit(code)
