"""Check native archive parity and Hua-owned native compiler generations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys

from n4_scalar_acceptance import (
    CLANG, DRIVER, SUPERVISOR_SECONDS, WASMTIME, bootstrap, digest, run, save,
)


def build(options, entry, output, generator, native=False):
    cpp = output.with_suffix(".cpp")
    command = [sys.executable, DRIVER, entry, "-o", output, "--keep-cpp", cpp,
               "--runtime-dir", options.runtime]
    if native:
        command += ["--native-generator", generator]
    else:
        command += ["--hua", options.host, "--generator", generator]
    execution = run(command, output.parent, SUPERVISOR_SECONDS + 5)
    receipt = json.loads(execution["stdout"])
    assert receipt["status"] == "ok" and receipt["cpp_sha256"] == digest(cpp)
    if native:
        invocation = receipt["generator_command"]
        assert Path(invocation[0]) == generator and invocation[1] == "emit", invocation
        assert receipt["generator_sha256"] == digest(generator)
    print(output.name, receipt["total_seconds"], "seconds", flush=True)
    save(output.with_suffix(".build.json"), receipt)
    return receipt


def archive(executable, entry, target):
    execution = run([executable, entry, target], target.parent, SUPERVISOR_SECONDS)
    receipt = json.loads(execution["stdout"])
    assert not execution["stderr"] and receipt["status"] == "ok", execution
    assert receipt["file_bytes"] == target.stat().st_size
    result = dict(sha256=digest(target), seconds=execution["seconds"], receipt=receipt)
    save(target.with_name(target.name + ".build.json"), result)
    return result


def parser_parity(options, frozen, work):
    program = work / "parser.exe"
    entry = frozen / "parser_compiler_main.hua"
    native = build(options, entry, program, options.generator)
    invalid = work / "bad.hua"
    invalid.write_text("fn broken( {\n", encoding="utf-8")
    results = {}
    for source in (frozen / "lexer.hua", frozen / "ast_pool.hua", frozen / "parser.hua", invalid):
        command = [source, source.name]
        vm = run([options.host, "run", entry, "--execution-profile", "compiler", "--", *command],
                 work, SUPERVISOR_SECONDS)
        exe = run([program, *command], work, SUPERVISOR_SECONDS)
        expected = json.loads(vm["stdout"])
        actual = json.loads(exe["stdout"])
        assert expected == actual, (source.name, expected.get("error"), actual.get("error"))
        count = len(actual.get("nodes", []))
        if source == invalid:
            assert actual["status"] == "error" and actual["error"].startswith("E2001@")
        else:
            assert actual["status"] == "ok" and count > 0, actual.get("error")
        results[source.name] = dict(nodes=count, native=exe["seconds"], vm=vm["seconds"])
    return dict(build=native, cases=results)


def archive_parity(options, frozen, work):
    entry = frozen / "archive_main.hua"
    reference = work / "reference.huab"
    compiler = work / "archive.exe"
    reference_build = bootstrap(options.host, options.seed, entry, reference)
    native_build = build(options, entry, compiler, options.generator)
    product = work / "native.huab"
    product_build = archive(compiler, entry, product)
    assert product.read_bytes() == reference.read_bytes(), (product_build, reference_build)
    run([options.host, "check", product], work)
    sample = work / "sample"
    sample.mkdir()
    (sample / "m.hua").write_text("pub fn answer() int { return 42 }\n", encoding="utf-8")
    entry_sample = sample / "main.hua"
    entry_sample.write_text("import m\nfn main(){ print(m.answer()) }\n", encoding="utf-8")
    compiled = work / "sample.huab"
    detached = frozen.with_name("source-moved")
    frozen.rename(detached)
    try:
        detached_build = bootstrap(options.host, product, entry_sample, compiled)
        sample.rename(sample.with_name("sample-moved"))
        assert run([options.host, "run", compiled], work)["stdout"] == "42\n"
    finally:
        detached.rename(frozen)
    bad = work / "failure.hua"
    bad.write_text("fn main() { unknown_symbol() }\n", encoding="utf-8")
    original = digest(product)
    failed = run([compiler, bad, product], work)
    failure = json.loads(failed["stdout"])
    assert failure["status"] == "error" and failure["stage"] == "types", failure
    assert digest(product) == original
    return dict(vm=reference_build, exe=native_build, native=product_build, detached=detached_build, failure=failure)


def pe_without_timestamps(path):
    data = bytearray(path.read_bytes())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE" + bytes(2)
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20B
    debug_rva, debug_size = struct.unpack_from("<II", data, optional + 112 + 8 * 6)
    data[pe + 8:pe + 12] = bytes(4)
    if debug_size:
        assert debug_size % 28 == 0
        for i in range(section_count):
            section = optional + optional_size + i * 40
            rva, size, offset = struct.unpack_from("<III", data, section + 12)
            if rva <= debug_rva and debug_rva + debug_size <= rva + size:
                first = offset + debug_rva - rva
                for entry in range(first, first + debug_size, 28):
                    data[entry + 4:entry + 8] = bytes(4)
                break
        else:
            raise AssertionError(f"PE debug directory missing in {path}")
    return hashlib.sha256(data).hexdigest()


def generations(options, frozen, work):
    entry = frozen / "native_main.hua"
    previous = options.generator
    evidence = {}
    for generation in ("A", "B", "C"):
        directory = work / generation
        directory.mkdir()
        executable = directory / "generator.exe"
        result = build(options, entry, executable, previous, native=generation != "A")
        evidence[generation] = dict(exe=digest(executable), build=result)
        previous = executable
    b = work / "B" / "generator.exe"
    c = work / "C" / "generator.exe"
    assert b.with_suffix(".cpp").read_bytes() == c.with_suffix(".cpp").read_bytes()
    for generation, executable in (("B", b), ("C", c)):
        evidence[generation]["exe_without_pe_timestamps"] = pe_without_timestamps(executable)
    for generation, executable in (("B", b), ("C", c)):
        compiler = executable.parent / "archive.exe"
        build(options, frozen/"archive_main.hua", compiler, executable, native=True)
        compiled = archive(compiler, frozen/"archive_main.hua", executable.parent/"archive.huab")
        evidence[generation]["archive"] = compiled
    assert (b.parent/"archive.huab").read_bytes() == (c.parent/"archive.huab").read_bytes()

    mutated = work / "mutated-source"
    shutil.copytree(frozen, mutated)
    emitter = mutated / "native_cpp.hua"
    code = emitter.read_text(encoding="utf-8")
    old = 'if op=="+" { return "add" }'
    assert code.count(old) == 1
    emitter.write_text(code.replace(old, 'if op=="+" { return "sub" }'), encoding="utf-8", newline="\n")
    child = work / "changed.exe"
    build(options, mutated/"native_main.hua", child, work/"A"/"generator.exe", native=True)
    probe = work / "probe.hua"
    probe.write_text("fn main() { print(9+4) }\n", encoding="utf-8")
    for name, generator, expected in (("original", b, "13\n"), ("changed", child, "5\n")):
        executable = work / (name+"-probe.exe")
        build(options, probe, executable, generator, native=True)
        assert run([executable], work)["stdout"] == expected
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("parser", "archive", "generations"))
    for name in ("host", "seed", "generator", "runtime", "source", "output"):
        parser.add_argument(name, type=Path)
    options = parser.parse_args()
    for name in ("host", "seed", "generator", "runtime", "source", "output"):
        setattr(options, name, getattr(options, name).resolve())
    work = options.output
    work.mkdir(parents=True, exist_ok=False)
    initial_runtime = options.runtime
    runtime = work / "runtime"
    runtime.mkdir()
    for name in ("wasmtime.dll", "libhua_semantics.a", "libhua_frontend.a"):
        shutil.copy2(initial_runtime / name, runtime / name)
    options.runtime = runtime
    path = [item for item in os.environ.get("PATH", "").split(os.pathsep)
            if item and Path(item).resolve() not in (options.host.parent, initial_runtime)]
    os.environ["PATH"] = os.pathsep.join([str(runtime), *path])
    frozen = work / "source"
    shutil.copytree(options.source, frozen)
    files = [CLANG, CLANG.with_name("ld.lld.exe"), WASMTIME, options.generator,
             options.runtime/"libhua_semantics.a", options.runtime/"libhua_frontend.a",
             options.runtime/"wasmtime.dll",
             *sorted(frozen.glob("*.hua"))]
    hashes = {str(path): digest(path) for path in files}
    save(work/"inputs.json", hashes)
    if options.stage == "parser":
        report = parser_parity(options, frozen, work)
    elif options.stage == "archive":
        report = archive_parity(options, frozen, work)
    else:
        report = generations(options, frozen, work)
    assert hashes == {str(path): digest(path) for path in files}
    save(work/"report.json", report)


if __name__ == "__main__":
    main()
