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
mods, callers = collections.Counter(), collections.Counter(); total = len(raw['samples'])
for s in raw['samples']:
    fr = s['frames']
    if not fr or fr[0] in names: continue
    mm = next((pathlib.Path(x['path']).name for x in raw['modules'] if x['base'] <= fr[0] < x['base'] + x['size']), '?'); mods[mm] += 1
    first = next((names[pc] for pc in fr if pc in names), '?'); callers[first] += 1
print('system modules:', [(k, f'{100*v/total:.1f}%') for k, v in mods.most_common(6)])
for k, v in callers.most_common(15): print(f'  {100*v/total:5.1f}% {k[:140]}')
