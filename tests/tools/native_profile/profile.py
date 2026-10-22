# Sample one hua command with the native sampler and print self/inclusive function shares.
# usage: python profile.py out.json interval_ms limit_ms hua.exe args...
import json, subprocess, sys, collections, pathlib
here = pathlib.Path(__file__).resolve().parent
root = here.parents[2]
symbolizer = root / '.tools/llvm-mingw-20260922-ucrt-x86_64/bin/llvm-symbolizer.exe'
out, interval, limit, exe, *args = sys.argv[1:]
exe = str(pathlib.Path(exe).resolve())
# HUA_PROFILE_REUSE=1 analyzes an existing trace; exit 124 means the sampler stopped at limit_ms.
if not __import__('os').environ.get('HUA_PROFILE_REUSE'):
    for suffix in ('', '.stdout', '.stderr'): pathlib.Path(out + suffix).unlink(missing_ok=True)
    rc = subprocess.run([__import__('os').environ.get('HUA_SAMPLER',str(root/'tests/build-prof/sampler.exe')), out, interval, limit, exe, *args]).returncode
    if rc not in (0, 124): raise SystemExit(f'sampler failed: {rc}')
raw = json.load(open(out, encoding='utf-8'))
module = next(m for m in raw['modules'] if pathlib.Path(m['path']).resolve() == pathlib.Path(exe))
base, size = module['base'], module['size']
pcs = sorted({pc for s in raw['samples'] for pc in s['frames'] if base <= pc < base + size})
names = {}
if pcs:
    p = subprocess.run([str(symbolizer), '--obj=' + exe, '--relative-address', '--output-style=JSON', '--inlining=false'],
                       input='\n'.join(hex(pc - base) for pc in pcs), capture_output=True, text=True)
    for pc, line in zip(pcs, p.stdout.splitlines()):
        item = json.loads(line)
        item = item[0] if isinstance(item, list) else item
        sym = item['Symbol'][0]['FunctionName'] if item.get('Symbol') else '?'
        names[pc] = sym
self_c, incl = collections.Counter(), collections.Counter()
total = len(raw['samples'])
for s in raw['samples']:
    frames = s['frames']
    leaf = frames[0] if frames else 0
    self_c[names.get(leaf, '[system]')] += 1
    for fn in {names[pc] for pc in frames if pc in names}:
        incl[fn] += 1
short = lambda n: (n[:150] + '…') if len(n) > 150 else n
print(f"samples={total} wall={raw['wall_seconds']:.2f}s")
print('--- self ---')
for n, c in self_c.most_common(25): print(f'{100*c/total:5.1f}% {short(n)}')
print('--- inclusive ---')
for n, c in incl.most_common(40): print(f'{100*c/total:5.1f}% {short(n)}')
