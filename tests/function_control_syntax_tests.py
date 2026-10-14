"""A04 source-only frontend gate. Never invokes VM/AST execution or HUAB."""
from pathlib import Path
import json
import os
import re
import sys
import tempfile
import time

os.environ['HUA_TEST_SUITE_TIMEOUT_SECONDS'] = str(min(60, float(os.environ.get('HUA_TEST_SUITE_TIMEOUT_SECONDS', '60'))))
from process_guard import run

def main():
    hua, root, build = [Path(value).resolve() for value in sys.argv[1:4]]
    cases = json.loads((root/'tests/data/a04_frontend_cases.json').read_text(encoding='utf-8'))['cases']
    out = build/'a04-frontend'
    out.mkdir(parents=True, exist_ok=True)
    results = []
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix='a04-', dir=build) as temporary:
        for case in cases:
            source = case['source']+'\n'
            path = Path(temporary)/(case['id']+'.hua')
            path.write_text(source, encoding='utf-8')
            for mode, expected in [('ast',case['parse_error']),('check',case['check_error'])]:
                result = run([str(hua),mode,str(path)], cwd=root, capture_output=True,
                             text=True, encoding='utf-8', timeout=10)
                if expected:
                    assert result.returncode == 1 and f'error[{expected}]' in result.stderr, (case['id'],mode,result)
                    if 'error_token' in case:
                        start = source.index(case['error_token'])
                        line = source[:start].count('\n')+1
                        column = start-source.rfind('\n',0,start)
                        assert f':{line}:{column}\n' in result.stderr, (case['id'],mode,result.stderr)
                else:
                    assert result.returncode == 0 and not result.stderr, (case['id'],mode,result)
                    if mode == 'check':
                        assert result.stdout.strip() == 'OK', (case['id'],mode,result)
                results.append({'id':case['id'],'mode':mode,'expected_error':expected})
        result = run([str(hua),'check',str(root/'selfhost/parser.hua')], cwd=root,
                     capture_output=True,text=True,encoding='utf-8',timeout=10)
        assert result.returncode == 0 and result.stdout.strip() == 'OK', result
    report={'status':'passed','cases':len(cases),'checks':len(results)+1,
            'seconds':round(time.monotonic()-started,3),'results':results,
            'scope':'source ast/check only; Hua Parser source check does not execute it'}
    (out/'syntax-results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(f"{len(cases)} A04 cases, {len(results)+1} frontend checks, {report['seconds']} seconds; no execution")

if __name__ == '__main__':
    main()
