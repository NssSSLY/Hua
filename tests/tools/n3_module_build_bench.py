"""Measure split and incremental builds of the Hua-authored native generator."""
import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import psutil

ROOT = Path(__file__).resolve().parents[2]


def timed_build(command, cwd, env, log):
    started = time.monotonic()
    peak = 0
    with log.with_suffix(".stdout").open("wb") as stdout, log.with_suffix(".stderr").open("wb") as stderr:
        with subprocess.Popen(command, cwd=cwd, env=env, stdout=stdout, stderr=stderr) as build:
            parent = psutil.Process(build.pid)
            while build.poll() is None:
                try:
                    active = [parent, *parent.children(recursive=True)]
                    resident = 0
                    for proc in active:
                        try:
                            resident += proc.memory_info().rss
                        except psutil.NoSuchProcess:
                            pass
                    peak = max(peak, resident)
                except psutil.NoSuchProcess:
                    pass
                if time.monotonic() - started > 605:
                    build.kill()
                    raise TimeoutError(f"{log.name} exceeded the release budget")
                time.sleep(0.15)
    assert build.returncode == 0, log.with_suffix(".stderr").read_text(encoding="utf-8")
    return {"wall_ms": round((time.monotonic() - started) * 1000), "sampled_tree_rss": peak,
            **json.loads(log.with_suffix(".stdout").read_bytes())}


def verify_layout(sdk, base):
    """Change the lexer Token layout and make the parser read the new field."""
    work = base / "layout"
    work.mkdir()
    sources = work / "sources"
    shutil.copytree(base / "sources", sources)
    lexer = sources / "lexer.hua"
    parser = sources / "parser.hua"
    lexer.write_text(lexer.read_text(encoding="utf-8")
                     .replace("pub struct Token {", "pub struct Token {\n    n34_line int", 1)
                     .replace("Token{", "Token{n34_line:77,"), encoding="utf-8")
    prior = parser.read_text(encoding="utf-8")
    modified = prior.replace('str(t.line)+":"+str(t.column)', 'str(t.n34_line)+":"+str(t.column)', 1)
    assert prior != modified
    parser.write_text(modified, encoding="utf-8")
    host = sdk / "bin/hua.exe"
    env = os.environ | {"HUA_HOME": str(sdk), "HUA_RELEASE_CACHE": str(base / "cache")}
    image = work / "layout.exe"
    result = timed_build([host, "build", "--release", sources / "native_main.hua", "-o", image],
                         work, env, work / "compiler-layout-edit")
    (work / "report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    rebuilt = {unit["module"] for unit in result["units"] if unit["status"] == "compiled"}
    assert rebuilt == {"lexer.hua", "parser.hua"}, result
    invalid = work / "bad-parser.hua"
    invalid.write_text("fn main( {\n", encoding="utf-8")
    diagnostic = subprocess.check_output([image, "emit", invalid, work / "probe.cpp"], env=env, timeout=30)
    assert "E2001@77:10" in json.loads(diagnostic)["detail"], diagnostic
    (work / "diagnostic.json").write_bytes(diagnostic)
    print("compiler-layout-edit", result["wall_ms"], result["compile_calls"], flush=True)


def verify(sdk, work, *, legacy):
    work.mkdir()
    sources = work / "sources"
    shutil.copytree(ROOT / "selfhost", sources, ignore=shutil.ignore_patterns("__pycache__"))
    manifest = sdk / "manifest.json"
    original = manifest.read_bytes()
    contents = json.loads(original)
    release = contents["native_release"]
    host = sdk / "bin/hua.exe"
    entry = sources / "native_main.hua"
    candidate = work / "candidate.exe"
    output = work / "incremental.exe"
    env = os.environ | {"HUA_HOME": str(sdk), "HUA_RELEASE_CACHE": str(work / "cache")}
    report = {}

    def build(label, destination, override=None):
        result = timed_build([host, "build", "--release", entry, "-o", destination],
                             work, env | (override or {}), work / label)
        report[label] = result
        (work / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(label, result["wall_ms"], result["compile_calls"], flush=True)
        return result

    def probe(generator, source, destination):
        result = subprocess.run([generator, "emit", source, destination], env=env, capture_output=True, timeout=30, check=True)
        return json.loads(result.stdout)

    hello = work / "hello.hua"
    hello.write_text('fn main() { print("hello") }\n', encoding="utf-8")
    invalid = work / "invalid.hua"
    invalid.write_text('fn main() {\n    $\n}\n', encoding="utf-8")
    out_cpp = work / "probe.cpp"
    try:
        cold = build("compiler-cold", candidate)
        assert cold["compile_calls"] == 13, cold
        baseline = sha256(candidate.read_bytes()).hexdigest()
        candidate.unlink()
        hot = build("compiler-hot", candidate)
        assert hot["compile_calls"] == hot["generate_calls"] == hot["link_calls"] == 0, hot
        assert sha256(candidate.read_bytes()).hexdigest() == baseline
        baseline_error = probe(candidate, invalid, out_cpp)
        assert "E1001" in baseline_error["detail"], baseline_error

        # Rebuild from the native compiler it just produced. A full reuse of the
        # object set and linked EXE also checks all generated module inputs.
        generation = sdk / "lib/compiler/n34-generation-b.exe"
        shutil.copyfile(candidate, generation)
        release["generator"] = generation.relative_to(sdk).as_posix()
        manifest.write_text(json.dumps(contents), encoding="utf-8")
        next_gen = build("compiler-next-generation", work / "generation-c.exe")
        assert (next_gen["generate_calls"], next_gen["compile_calls"], next_gen["link_calls"]) == (1, 0, 0), next_gen
        assert sha256((work / "generation-c.exe").read_bytes()).hexdigest() == baseline

        lexer = sources / "lexer.hua"
        before = lexer.read_text(encoding="utf-8")
        signature = "fn failure(code string, line int, column int) string {"
        updated = before.replace(signature, signature + '\n    if code=="E1001" { return code+"@"+str(line)+":"+str(column)+" N3.4" }', 1)
        assert before != updated
        lexer.write_text(updated, encoding="utf-8")
        delta = build("compiler-leaf-edit", output)
        assert [unit["module"] for unit in delta["units"] if unit["status"] == "compiled"] == ["lexer.hua"], delta
        changed_error = probe(output, invalid, out_cpp)
        assert changed_error["detail"].replace(" N3.4", "") == baseline_error["detail"], changed_error
        assert changed_error != baseline_error
        assert probe(output, hello, out_cpp)["status"] == "ok"
        expected_cpp = out_cpp.read_bytes()

        clean = work / "clean.exe"
        build("compiler-edit-clean", clean, {"HUA_RELEASE_CACHE": "off"})
        assert sha256(clean.read_bytes()).digest() == sha256(output.read_bytes()).digest()
        assert probe(clean, hello, out_cpp)["status"] == "ok"
        assert out_cpp.read_bytes() == expected_cpp
        assert probe(clean, invalid, out_cpp) == changed_error

        verify_layout(sdk, work)

        if legacy:
            release.pop("generator_mode")
            manifest.write_text(json.dumps(contents), encoding="utf-8")
            single = work / "single.exe"
            build("compiler-edit-single", single, {"HUA_RELEASE_CACHE": "off"})
            assert probe(single, hello, out_cpp)["status"] == "ok"
            assert out_cpp.read_bytes() == expected_cpp
            assert probe(single, invalid, out_cpp) == changed_error
    finally:
        manifest.write_bytes(original)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True, help="disposable SDK copy")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--legacy", action="store_true")
    options = parser.parse_args()
    verify(options.sdk.resolve(), options.work.resolve(), legacy=options.legacy)
