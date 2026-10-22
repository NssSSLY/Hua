# usage: python callees.py trace.json exe substring...  -> direct callees share within samples containing the function
import json, subprocess, sys, collections, pathlib
here = pathlib.Path(__file__).resolve().parent
root = here.parents[2]
sym = root / '.tools/llvm-mingw-20260922-ucrt-x86_64/bin/llvm-symbolizer.exe'
raw = json.load(open(sys.argv[1], encoding='utf-8')); exe = str(pathlib.Path(sys.argv[2]).resolve())
m = next(m for m in raw['modules'] if pathlib.Path(m['path']).resolve() == pathlib.Path(exe)); b, n = m['base'], m['size']
pcs = sorted({pc for s in raw['samples'] for pc in s['frames'] if b <= pc < b + n})
p = subprocess.run([str(sym), '--obj=' + exe, '--relative-address', '--output-style=JSON', '--inlining=false'], input='\n'.join(hex(pc - b) for pc in pcs), capture_output=True, text=True)
names = {}
for pc, line in zip(pcs, p.stdout.splitlines()):
    it = json.loads(line); it = it[0] if isinstance(it, list) else it
    names[pc] = it['Symbol'][0]['FunctionName'] if it.get('Symbol') else '?'
total = len(raw['samples'])
for target in sys.argv[3:]:
    c = collections.Counter(); hit = 0
    for s in raw['samples']:
        fr = [names.get(pc, '[system]') for pc in s['frames']]
        idx = [i for i, f in enumerate(fr) if target in f]
        if not idx: continue
        hit += 1; i = max(idx)  # outermost occurrence
        c['<self>' if i == 0 else fr[i - 1]] += 1
    print(f'=== {target}: {100*hit/total:.1f}% of samples')
    for k, v in c.most_common(14): print(f'  {100*v/total:5.1f}% {k[:140]}')
