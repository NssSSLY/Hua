"""Profile a complete selfhost compilation against an accepted archive."""

import argparse
import json
from pathlib import Path
import shutil

from audit_bootstrap_phases import WATCHES, closure, command, digest, reduce_events, save


def read_phases(stdout):
    events = []
    receipt = None
    for line in stdout.decode("utf-8").splitlines():
        if line.startswith("@@phase "):
            _, event, stage, stamp = line.split()
            events.append({"event": event, "phase": stage, "ns": int(stamp),
                           "cpu_ns": 0, "counters": {}})
        elif line.startswith("{"):
            receipt = json.loads(line)
    assert receipt is not None and receipt["status"] == "ok", receipt
    phases = {name: {key: data[key] for key in ("ns", "entries")}
              for name, data in reduce_events(events).items()}
    return receipt, phases


def profile(root, out, host, baseline):
    out.mkdir(parents=True, exist_ok=False)
    frozen = out / "frozen"
    instrumented = out / "instrumented"
    frozen.mkdir()
    instrumented.mkdir()
    for name, path in closure(root / "selfhost/archive_main.hua").items():
        shutil.copyfile(path, frozen / name)
        shutil.copyfile(path, instrumented / name)
    shutil.copyfile(baseline, out / "accepted.huab")
    report = {"status": "running", "host_sha256": digest(host),
              "baseline_sha256": digest(out / "accepted.huab"),
              "sources": {file.name: digest(file) for file in sorted(frozen.iterdir())}}
    save(out / "profile.json", report)
    # The older return-infer hook was removed when return types became explicit.
    watches = {name: [(sig, phase) for sig, phase in items if phase != "return-infer"]
               for name, items in WATCHES.items()}
    watches["archive_main.hua"] = [("fn main() {", "execution")]
    for name, hooks in watches.items():
        path = instrumented / name
        text = path.read_text(encoding="utf-8")
        for signature, phase in hooks:
            assert text.count(signature) == 1, (name, signature)
            text = text.replace(signature, signature + f'\n    _profile_phase("begin","{phase}")\n    defer _profile_phase("end","{phase}")\n', 1)
        text = ("import std.time as profile_clock\n" + text +
                '\nfn _profile_phase(event string, phase string) {\n'
                '    let stamp = profile_clock.monotonic_ns()\n'
                '    print("@@phase "+event+" "+phase+" "+str(stamp))\n}\n')
        path.write_text(text, encoding="utf-8", newline="\n")

    component = out / "instrumented.huab"
    product = out / "observed.huab"
    try:
        print("Building instrumented compiler", flush=True)
        command(root, out, "prepare", [host, "build", instrumented / "archive_main.hua",
                                        "-o", component], 600)
        report["instrumented_sha256"] = digest(component)
        save(out / "profile.json", report)
        print("Profiling complete compiler source closure", flush=True)
        row, stdout, stderr = command(out, out, "full", [
            host, "run", component, "--execution-profile", "compiler",
            "--report-budget", "--", frozen / "archive_main.hua", product], 600)
        report["receipt"], report["phases_exclusive"] = read_phases(stdout)
        report["budget"] = next(json.loads(line) for line in stderr.decode("utf-8").splitlines()
                                if line.startswith('{"schema":"hua.execution.budget.v1"'))
        report["measurement"] = row
        assert product.read_bytes() == (out / "accepted.huab").read_bytes()
        command(out, out, "reader", [host, "check", product], 30)
        report["status"] = "passed"
    finally:
        if report["status"] == "running":
            report["status"] = "stopped"
        save(out / "profile.json", report)


def profile_native(root, base, host, baseline, build, clang, wasmtime):
    vm = json.loads((base / "profile.json").read_text(encoding="utf-8"))
    assert vm["status"] == "passed" and vm["baseline_sha256"] == digest(baseline)
    assert vm["host_sha256"] == digest(host)
    out = base / "native"
    out.mkdir()
    cpp = out / "instrumented.cpp"
    exe = out / "instrumented.exe"
    artifact = out / "observed.huab"
    report = {"status": "running", "host_sha256": digest(host),
              "baseline_sha256": digest(baseline), "sources": vm["sources"]}
    save(out / "profile.json", report)
    try:
        command(root, out, "generate", [host, "native", base / "instrumented/archive_main.hua",
                                        "-o", cpp], 600)
        command(root, out, "link", [
            clang, "-std=c++20", "-O2", "-DNDEBUG", "-DHUA_NATIVE_COUNT_STEPS=0",
            "-I", root / "include", cpp, "-o", exe,
            build / "libhua_semantics.a", build / "libhua_frontend.a",
            "-lpthread", "-lbcrypt", wasmtime, "-static", "-Wl,--stack,8388608",
        ], 600)
        shutil.copyfile(host.with_name("wasmtime.dll"), out / "wasmtime.dll")
        report["compiler_sha256"] = digest(exe)
        row, stdout, _ = command(out, out, "full", [
            exe, base / "frozen/archive_main.hua", artifact], 600)
        report["receipt"], report["phases_exclusive"] = read_phases(stdout)
        report["measurement"] = row
        assert artifact.read_bytes() == baseline.read_bytes()
        command(out, out, "reader", [host, "check", artifact], 30)
        report["status"] = "passed"
    finally:
        if report["status"] == "running":
            report["status"] = "stopped"
        save(out / "profile.json", report)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--native-build", type=Path,
                        help="Profile native execution using a completed VM run and this Release build")
    parser.add_argument("--clang", type=Path)
    parser.add_argument("--wasmtime-lib", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    inputs = (root, args.output.resolve(), args.host.resolve(), args.baseline.resolve())
    native = (args.native_build, args.clang, args.wasmtime_lib)
    if any(native):
        if not all(native):
            parser.error("--native-build, --clang and --wasmtime-lib must be provided together")
        profile_native(*inputs, *(path.resolve() for path in native))
    else:
        profile(*inputs)
