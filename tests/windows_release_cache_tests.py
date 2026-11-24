"""Black-box incremental release acceptance using an isolated installed Windows SDK."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
import json
import os
from pathlib import Path
import shutil
import subprocess
from time import monotonic


def verify(sdk, work, benchmark):
    work.mkdir()
    env = os.environ.copy()
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    for key in ("HUA_HOME", "HUA_PATH"):
        env.pop(key, None)
    root = work / "cache"
    env["HUA_RELEASE_CACHE"] = str(root)
    host = sdk / "bin/hua.exe"
    report = {}
    sdk_manifest = json.loads((sdk / "manifest.json").read_text(encoding="utf-8"))
    split = sdk_manifest["native_release"].get("generator_mode") == "modules-v1"
    small = sdk_manifest["native_release"].get("release_policy") == "small-app-v1"

    def run(name, source, output, calls=None, success=True, override=None):
        args = [host, "build", "--release", source, "-o", output]
        start = monotonic()
        try:
            completed = subprocess.run(args, cwd=work, env=env | (override or {}),
                                       timeout=605, capture_output=True)
        except subprocess.TimeoutExpired as error:
            (work / f"{name}.stdout").write_bytes(error.stdout or b"")
            (work / f"{name}.stderr").write_bytes(error.stderr or b"")
            raise
        (work / f"{name}.stdout").write_bytes(completed.stdout)
        (work / f"{name}.stderr").write_bytes(completed.stderr)
        assert (completed.returncode == 0) == success, (name, completed.stderr, completed.stdout)
        if not success:
            return completed.stderr.decode("utf-8")
        receipt = json.loads(completed.stdout)
        stages = tuple(receipt[key + "_calls"] for key in ("generate", "compile", "link"))
        if calls is not None:
            assert stages == calls, (name, receipt)
        report[name] = {"wall_ms": round((monotonic() - start) * 1000), **receipt}
        print(name, stages, receipt["cache_status"], receipt["total_ms"], flush=True)
        (work / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
        return receipt

    def executable(file, expected):
        assert subprocess.check_output([file], cwd=work, env=env, timeout=30, encoding="utf-8").strip() == expected

    def write(file, text):
        file.write_text(text, encoding="utf-8")
        return file

    hello = write(work / "你好.hua", 'fn main() { print("hello") }\n')
    out = work / "应用.exe"
    run("hello-cold", hello, out, (1, 1, 1))
    baseline = sha256(out.read_bytes()).hexdigest()
    run("hello-hot", hello, out, (0, 0, 0))
    out.unlink()
    run("recover", hello, out, (0, 0, 0))
    assert sha256(out.read_bytes()).hexdigest() == baseline
    moved = work / "发布目录" / "重新交付.exe"
    run("alternate-output", hello, moved, (0, 0, 0))
    assert sha256(moved.read_bytes()).hexdigest() == baseline
    executable(moved, "hello")
    with out.open("rb") as locked:
        # The MSVCRT/Python handle is opened without FILE_SHARE_DELETE.
        assert run("locked-output", hello, out, success=False)
        assert sha256(locked.read()).hexdigest() == baseline
    (root / (baseline + ".exe")).write_bytes(b"broken")
    run("bad-exe", hello, out, (1, 0, 1))
    executable(out, "hello")
    indexes = list(root.glob("*.obj.key"))
    assert len(indexes) == 1
    obj = root / (indexes[0].read_text() + ".obj")
    obj.write_bytes(b"broken")
    for item in root.glob("*.exe"):
        item.write_bytes(b"broken")
    run("bad-object", hello, out, (1, 1, 1))
    executable(out, "hello")

    main = write(work / "main.hua", "import foo as foo\nfn main() { print(foo.value()) }\n")
    package = work / "foo"
    package.mkdir()
    write(package / "package.hua", "import leaf as leaf\npub fn value() int { return leaf.value() }\n")
    leaf = write(work / "leaf.hua", "pub fn value() int { return 11 }\n")
    run("package-cold", main, out, (1, 4 if split else 1, 1))
    run("package-hot", main, out, (0, 0, 0))
    stamp = leaf.stat()
    write(leaf, "pub fn value() int { return 22 }\n")
    os.utime(leaf, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    changed = run("transitive-content-edit", main, out, (1, 1, 1))
    if split:
        assert [unit["module"] for unit in changed["units"] if unit["status"] == "compiled"] == ["leaf.hua"]
    executable(out, "22")
    write(work / "foo.hua", "pub fn value() int { return 33 }\n")
    run("higher-priority-import", main, out, (1, 3 if split and not small else 1, 1))
    executable(out, "33")
    clean = work / "clean.exe"
    run("no-cache-reference", main, clean, (1, 3 if split and not small else 1, 1), override={"HUA_RELEASE_CACHE": "off"})
    executable(clean, "33")
    old = sha256(out.read_bytes()).hexdigest()
    main.write_text("fn main() { print(undefined) }\n")
    assert "E" in run("invalid-source", main, out, success=False)
    assert sha256(out.read_bytes()).hexdigest() == old

    alg = write(work / "算法.hua", "import std.alg_integer as alg\nfn main() { print(alg.lower_bound([1,3,5],3)) }\n")
    stdlib = run("stdlib-cold", alg, out)
    expected = 3 if split and stdlib.get("layout") != "combined" else 1
    assert (stdlib["generate_calls"],stdlib["compile_calls"],stdlib["link_calls"]) == (1,expected,1), stdlib
    run("stdlib-hot", alg, out, (0, 0, 0))
    executable(out, "1")
    saved_stdlib = sdk / "src/std/alg_integer.hua"
    previous = saved_stdlib.read_bytes()
    saved_stdlib.write_bytes(previous + b"\n# cache dependency fixture\n")
    try:
        updated = run("stdlib-source-edit", alg, out)
        assert updated["generate_calls"] == 1
        executable(out, "1")
    finally:
        saved_stdlib.write_bytes(previous)

    manifest_file = sdk / "manifest.json"
    original = manifest_file.read_bytes()
    manifest = json.loads(original)
    release = manifest["native_release"]
    generator = sdk / release["generator"]
    alternate = generator.with_name("alternate-generator.exe")
    shutil.copyfile(generator, alternate)
    run("before-generator-switch", hello, out)
    release["generator"] = alternate.relative_to(sdk).as_posix()
    manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
    try:
        run("generator-switch", hello, out, (1, 0, 0))
    finally:
        manifest_file.write_bytes(original)
        alternate.unlink()

    headers = sdk / release["headers"] / "hua/native_rt.hpp"
    old_header = headers.read_bytes()
    stamp = headers.stat()
    # A same-size comment spelling change with restored mtime must still
    # invalidate an object key. The toolchain snapshot includes NTFS ChangeTime.
    variant = old_header
    pos = variant.find(b"//")
    assert pos >= 0
    point = pos + 2
    variant = variant[:point] + (b"_" if variant[point] != 95 else b" ") + variant[point+1:]
    headers.write_bytes(variant)
    os.utime(headers, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    try:
        run("same-mtime-header", hello, out, (1, 1, 1))
        executable(out, "hello")
    finally:
        headers.write_bytes(old_header)

    run("before-library-switch", hello, out, (1, 1, 1))
    semantics = sdk / release["semantics"]
    alternative_lib = semantics.with_name("alternate-semantics.a")
    shutil.copyfile(semantics, alternative_lib)
    release["generator"] = generator.relative_to(sdk).as_posix()
    release["semantics"] = alternative_lib.relative_to(sdk).as_posix()
    manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
    try:
        run("runtime-switch", hello, out, (1, 0, 1))
        executable(out, "hello")
    finally:
        manifest_file.write_bytes(original)
        alternative_lib.unlink()

    unavailable = work / "not-directory"
    unavailable.write_text("file")
    run("cache-unavailable", hello, out, (1, 1, 1), override={"HUA_RELEASE_CACHE": str(unavailable)})
    executable(out, "hello")

    simultaneous = write(work / "parallel.hua", 'fn main() { print("parallel") }\n')
    shared = work / "parallel.exe"
    with ThreadPoolExecutor(max_workers=2) as workers:
        futures = [workers.submit(run, f"parallel-{i}", simultaneous, shared) for i in range(2)]
        for future in futures:
            future.result()
    run("parallel-hot", simultaneous, shared, (0, 0, 0))
    executable(shared, "parallel")
    assert not list(work.rglob(".hua-job-*"))

    if split:
        # Diamond with externally shared layout, an enum payload and a single
        # dependency initializer. Its identity must survive an unrelated body edit.
        write(work / "model.hua", """
pub struct Item { value int }
pub enum Answer { Some(Item), Missing }
pub fn make(value int) Answer { return Answer.Some(Item{value:value}) }
print("model")
""".lstrip())
        left = write(work / "left.hua", """
import model as model
fn adjust(x int) int { return x+1 }
pub fn make(x int) model.Answer { return model.make(adjust(x)) }
print("left")
""".lstrip())
        right = write(work / "right.hua", """
import model as model
pub fn read(answer model.Answer) int {
    match answer {
        model.Answer.Some(item) => { return item.value }
        model.Answer.Missing => { return -1 }
    }
}
print("right")
""".lstrip())
        diamond = write(work / "diamond.hua", """
import left as left
import right as right
fn main() { print(right.read(left.make(1))) }
""".lstrip())
        run("diamond-cold", diamond, out, (1, 5, 1))
        executable(out, "model\nleft\nright\n2")
        write(left, left.read_text().replace("return x+1", "let step=2\n    return x+step"))
        edited = run("diamond-body", diamond, out, (1, 1, 1))
        assert [unit["module"] for unit in edited["units"] if unit["status"] == "compiled"] == ["left.hua"]
        executable(out, "model\nleft\nright\n3")
        model = work / "model.hua"
        write(model, model.read_text().replace("value int }", "extra bool\n    value int }")
              .replace("Item{value:value}", "Item{extra:true,value:value}").replace("Some(Item), Missing", "Missing, Some(Item)"))
        write(right, right.read_text().replace("return item.value", "if item.extra { return item.value }\n            return 0"))
        changed = run("diamond-layout", diamond, out)
        names = {unit["module"] for unit in changed["units"] if unit["status"] == "compiled"}
        assert {"model.hua","left.hua","right.hua"} <= names, changed
        executable(out, "model\nleft\nright\n3")
        run("diamond-layout-clean", diamond, clean, (1, 5, 1), override={"HUA_RELEASE_CACHE": "off"})
        executable(clean, "model\nleft\nright\n3")
        write(diamond, diamond.read_text().replace("import left as left\nimport right as right",
                                                 "import right as right\nimport left as left"))
        run("diamond-init-order", diamond, out)
        executable(out, "model\nright\nleft\n3")

    if benchmark:
        compiler_entry = sdk / release["generator_entry"]
        candidate = work / "candidate.exe"
        run("compiler-cold", compiler_entry, candidate, (1, 14 if split else 1, 1))
        expected = sha256(candidate.read_bytes()).hexdigest()
        candidate.unlink()
        run("compiler-hot", compiler_entry, candidate, (0, 0, 0))
        assert sha256(candidate.read_bytes()).hexdigest() == expected
        cpp = work / "candidate.cpp"
        probe = subprocess.run([candidate, "emit", hello, cpp], env=env, capture_output=True, check=True, timeout=60)
        assert json.loads(probe.stdout)["status"] == "ok" and cpp.stat().st_size > 0

    (work / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path, help="disposable SDK copy")
    parser.add_argument("--work", required=True, type=Path)
    parser.add_argument("--benchmark", action="store_true")
    options = parser.parse_args()
    verify(options.sdk.resolve(), options.work.resolve(), options.benchmark)
