"""A03/A04: public CLI contracts across AST, VM and source-free HUAB.

Each child is supervised; timeout aborts immediately instead of becoming an
expected language failure. No self-host compiler or external services are run.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, struct, sys, time
from process_guard import run, TestProcessTimeout

parser = argparse.ArgumentParser()
parser.add_argument('hua', type=Path)
parser.add_argument('repo', type=Path)
parser.add_argument('build', type=Path)
parser.add_argument('--section', choices=('a03', 'a04'), required=True)
parser.add_argument('--case', action='append', default=[])
parser.add_argument('--report-name', default='results')
args = parser.parse_args()
args.hua, args.repo, args.build = (p.resolve() for p in (args.hua, args.repo, args.build))
os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS'] = str(min(float(os.environ.get('HUA_TEST_SUITE_TIMEOUT_SECONDS', '240')), 240))
work = args.build / 'a03-a04-execution' / args.section
work.mkdir(parents=True, exist_ok=True)
data = args.repo / 'tests/data' / (args.section + '_execution_cases.json')
cases = json.loads(data.read_text(encoding='utf-8'))
if args.case:
    unknown = set(args.case) - {c['name'] for c in cases}
    if unknown: parser.error('unknown cases: ' + ', '.join(sorted(unknown)))
    cases = [c for c in cases if c['name'] in args.case]
checks = 0
rows = []
commands = []
started = time.monotonic()

def require(condition, detail):
    global checks
    checks += 1
    if not condition: raise AssertionError(detail)

def call(command, code=0):
    before = time.monotonic()
    p = run(list(map(str, command)), capture_output=True, encoding='utf-8', errors='replace', timeout=10)
    commands.append({'command': list(map(str, command)), 'returncode': p.returncode,
                     'seconds': round(time.monotonic()-before, 3), 'stdout': p.stdout, 'stderr': p.stderr})
    require(p.returncode == code, {'command': command, 'expected_code': code, 'actual': p.returncode,
                                  'stdout': p.stdout, 'stderr': p.stderr})
    return p

def diagnostic(p, located=True):
    if not located:
        found = re.search(r"error\[(E\d+)\]", p.stderr)
        require(found is not None, p.stderr)
        return (found[1],)
    found = re.search(r'error\[(E\d+)\].*?\n --> .*?:(\d+):(\d+)\n', p.stderr, re.S)
    require(found is not None, p.stderr)
    return found[1], int(found[2]), int(found[3])

def save(status, aborted=None):
    report = {'status': status, 'section': args.section, 'cases': len(rows), 'selected_cases': len(cases),
              'checks': checks, 'failed': sum(r['status'] == 'failed' for r in rows),
              'seconds': round(time.monotonic()-started, 3), 'commands': len(commands),
              'hua_sha256': hashlib.sha256(args.hua.read_bytes()).hexdigest(),
              'cases_sha256': hashlib.sha256(data.read_bytes()).hexdigest(),
              'engines': ['AST', 'VM', 'source-free HUAB'], 'rows': rows}
    if aborted: report['aborted'] = aborted
    (work/(args.report_name+'.json')).write_text(json.dumps(report, ensure_ascii=False, indent=2, default=str)+'\n', encoding='utf-8')
    (work/(args.report_name+'-commands.json')).write_text(json.dumps(commands, ensure_ascii=False, indent=2, default=str)+'\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'rows'}, ensure_ascii=False), flush=True)

for case in cases:
    folder = work/case['name']
    folder.mkdir(exist_ok=True)
    source, archive = folder/'input.hua', folder/'input.huab'
    source.write_text(case['source'], encoding='utf-8', newline='\n')
    archive.unlink(missing_ok=True)
    before = time.monotonic()
    try:
        static = case['phase'] == 'check'
        check = call([args.hua, 'check', source], 1 if static else 0)
        if static:
            first = diagnostic(check)
            require(first[0] == case['want'] and not check.stdout, (case, check.stdout, first))
            for action in ('interpret', 'run', 'build'):
                command = [args.hua, action, source]
                if action == 'build': command.extend(['-o', archive])
                p = call(command, 1)
                require(diagnostic(p) == first and not p.stdout, (action, case, p.stdout, p.stderr))
            require(not archive.exists(), 'static failure published an artifact')
        else:
            wanted_code = 1 if case['phase'] == 'run' else 0
            results = [call([args.hua, action, source], wanted_code) for action in ('interpret', 'run')]
            build = call([args.hua, 'build', source, '-o', archive])
            require(not build.stderr, build.stderr)
            require(archive.exists(), 'missing archive')
            blob = archive.read_bytes()
            require(len(blob) >= 32 and struct.unpack_from('<I', blob, 12)[0] == 1, 'archive ABI')
            hidden = source.with_suffix('.removed')
            source.rename(hidden)
            try:
                require(not source.exists(), 'program source was not removed')
                results.append(call([args.hua, 'run', archive], wanted_code))
            finally: hidden.rename(source)
            wanted_output = case.get('stdout', '') if case['phase'] == 'run' else case['want']
            for result in results:
                require(result.stdout == wanted_output, {'case': case['name'], 'expected_stdout': wanted_output,
                                                        'actual_stdout': result.stdout, 'stderr': result.stderr})
            if case['phase'] == 'run':
                first = diagnostic(results[0], case.get("diagnostic_location", True))
                require(first[0] == case['want'], (case, first))
                for result in results[1:]: require(diagnostic(result, case.get("diagnostic_location", True)) == first, (case['name'], first, result.stderr))
            else:
                for result in results: require(not result.stderr, result.stderr)
        row = {'case': case['name'], 'phase': case['phase'], 'status': 'passed',
               'seconds': round(time.monotonic()-before, 3)}
    except TestProcessTimeout as error:
        rows.append({'case': case['name'], 'phase': case['phase'], 'status': 'timeout'})
        save('timeout', str(error))
        raise
    except AssertionError as error:
        row = {'case': case['name'], 'phase': case['phase'], 'status': 'failed', 'error': str(error),
               'seconds': round(time.monotonic()-before, 3)}
    rows.append(row)
    print(args.section.upper(), case['name'], row['status'], flush=True)
failed = any(row['status'] == 'failed' for row in rows)
save('failed' if failed else 'passed')
sys.exit(1 if failed else 0)
