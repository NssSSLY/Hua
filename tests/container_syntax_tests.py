"""A03 frontend gate: source parsing/static checking only; no execution or HUAB."""
from pathlib import Path
import json
import sys
import tempfile
import time
from process_guard import run

PREFIX = "struct Empty {}\nstruct Box { empty Empty\nflag bool }\n"
CASES = [
    ("array_header", PREFIX + "for e in [Empty{}] {}", None, None),
    ("map_header", PREFIX + "for k in map[int]Empty{1:Empty{}} {}", None, None),
    ("struct_if", PREFIX + "if Box{empty:Empty{},flag:true}.flag {}", None, None),
    ("struct_while", PREFIX + "while Box{empty:Empty{},flag:false}.flag {}", None, None),
    ("nested_array", PREFIX + "for e in [[Empty{}]] {}", None, None),
    ("multiline_array", PREFIX + "for e in [\nEmpty{}\n] {}", None, None),
    ("multiline_map", PREFIX + "for k in map[int]Empty{\n1:Empty{}\n} {}", None, None),
    ("nested_map", PREFIX + "for k in map[int][]Empty{1:[Empty{}]} {}", None, None),
    ("restored_if", "var flag=true\nif [true][0] && flag {} else {}", None, None),
    ("restored_while", "var flag=false\nwhile [true][0] && flag {}", None, None),
    ("restored_for", "var xs=[1,2]\nfor e in [1] {}\nfor e in xs {}", None, None),
    ("slice_forms", "var xs=[1,2]\nlet a=xs[:]\nlet b=xs[:1]\nlet c=xs[1:]\nlet d=xs[0:1]", None, None),
    ("canonical_extents", "var zero [0]int=[]\nvar grid [1][2]int=[[1,2]]", None, None),
    ("ordinary_integer", "var value=0_2\nvar hex=0x02", None, None),
    ("array_trailing_comma", "let x=[1,]", "E2001", "E2001"),
    ("map_trailing_comma", "let x=map[int]int{1:2,}", "E2001", "E2001"),
    ("struct_trailing_comma", PREFIX + "let x=Box{empty:Empty{},flag:true,}", "E2001", "E2001"),
    ("map_missing_colon", "let x=map[int]int{1 2}", "E2001", "E2001"),
    ("array_missing_comma", "let x=[1 2]", "E2001", "E2001"),
    ("empty_index", "let x=[1]\nlet y=x[]", "E2001", "E2001"),
    ("slice_third_endpoint", "let x=[1]\nlet y=x[0:1:1]", "E2001", "E2001"),
    ("size_leading_zero", "var x [02]int=[1,2]", "E2006", "E2006"),
    ("size_separator", "var x [0_2]int=[1,2]", "E2006", "E2006"),
    ("index_type", "let x=[1]\nlet y=x[true]", None, "E3004"),
    ("slice_type", "let x=[1]\nlet y=x[:true]", None, "E3004"),
    ("map_key_type", "let x=map[int]int{true:1}", None, "E3004"),
    ("map_value_type", "let x=map[int]int{1:true}", None, "E3004"),
    ("array_length", "var x [2]int=[1]", None, "E3004"),
    ("array_element_type", "var x []int=[1,true]", None, "E3004"),
]

def main():
    hua, root, build = map(lambda arg: Path(arg).resolve(), sys.argv[1:4])
    evidence = build / "a03-frontend"
    evidence.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    results = []
    with tempfile.TemporaryDirectory(prefix="a03-", dir=build) as temporary:
        for name, source, parse_error, check_error in CASES:
            path = Path(temporary) / (name + ".hua")
            path.write_text(source + "\n", encoding="utf-8")
            for mode, expected in [("ast", parse_error), ("check", check_error)]:
                result = run([str(hua), mode, str(path)], cwd=root,
                             capture_output=True, text=True, encoding="utf-8", timeout=10)
                if expected:
                    assert result.returncode == 1 and f"error[{expected}]" in result.stderr, (name, mode, result)
                else:
                    assert result.returncode == 0 and not result.stderr, (name, mode, result)
                    if mode == "check":
                        assert result.stdout.strip() == "OK", (name, mode, result)
                results.append({"case": name, "mode": mode, "expected_error": expected, "returncode": result.returncode})
        # Parse and statically check the updated Hua Parser, without executing it.
        parser = run([str(hua), "check", str(root / "selfhost/parser.hua")], cwd=root,
                     capture_output=True, text=True, encoding="utf-8", timeout=10)
        assert parser.returncode == 0 and parser.stdout.strip() == "OK", parser
    report = {"status": "passed", "cases": len(CASES), "checks": len(results) + 1,
              "seconds": round(time.monotonic() - started, 3), "results": results,
              "hua_parser": "source statically checked; execution deferred",
              "scope": "native source parser and semantic checker only; no VM/AST execution or archive"}
    (evidence / "syntax-results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{len(CASES)} container cases, {len(results)+1} frontend checks, {report['seconds']} seconds; no execution")

if __name__ == "__main__":
    main()
