"""Verify a packaged SDK in a separate Windows directory using only its runtime."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def verify(package, evidence, external):
    sdk = external / "Relocated Hua SDK"
    work = external / "Detached Work"
    work.mkdir()
    shutil.copytree(package, sdk)
    host = sdk / "bin/hua.exe"
    manifest = json.loads((sdk / "manifest.json").read_text(encoding="utf-8"))
    env = os.environ.copy()
    env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
    env.pop("HUA_HOME", None)
    env.pop("HUA_PATH", None)
    powershell = Path(env["PATH"]) / "WindowsPowerShell/v1.0/powershell.exe"
    report = {"status": "running", "sdk": str(sdk), "cwd": str(work),
              "timeout_seconds_per_command": 600, "commands": {}}

    def save():
        (evidence / "report.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8",
        )

    def run(label, args, expected=0):
        command = [str(arg) for arg in args]
        print(f"Start {label}", flush=True)
        start = time.perf_counter()
        with ((evidence / f"{label}.stdout").open("wb") as out,
              (evidence / f"{label}.stderr").open("wb") as err):
            try:
                result = subprocess.run(command, cwd=work, env=env,
                                        stdout=out, stderr=err, timeout=600)
                state = {"exit_code": result.returncode}
            except subprocess.TimeoutExpired:
                state = {"status": "timeout"}
        state.update(seconds=round(time.perf_counter() - start, 3), argv=command)
        report["commands"][label] = state
        error = (evidence / f"{label}.stderr").read_text(encoding="utf-8", errors="replace")
        for line in error.splitlines():
            if line.startswith('{"schema":"hua.execution.budget.v1"'):
                state["budget"] = json.loads(line)
        save()
        print(f"Finish {label} ({state['seconds']} s)", flush=True)
        assert state.get("exit_code") == expected, f"{label}: {state}; {error}"
        return (evidence / f"{label}.stdout").read_text(encoding="utf-8"), error

    def sha(file):
        return hashlib.sha256(file.read_bytes()).hexdigest()

    def launch(script, source, output, *options):
        return [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                script, source, output, *options]

    def compile_huab(label, compiler, source, output):
        stdout, _ = run(label, [host, "run", compiler, "--execution-profile",
                                "compiler", "--report-budget", "--", source, output])
        receipt = json.loads(stdout)
        assert receipt["schema"] == "hua.archive.result.v1" and receipt["status"] == "ok", receipt
        assert output.stat().st_size == receipt["file_bytes"], receipt
        budget = report["commands"][label]["budget"]
        assert budget["profile"] == "compiler" and budget["steps"] <= budget["per_executor_step_limit"]
        return receipt

    try:
        assert manifest["target"] == "windows-x86_64"
        paths = {p.relative_to(sdk).as_posix() for p in sdk.rglob("*") if p.is_file()}
        assert paths == set(manifest["files"]) | {"manifest.json"}
        for relative, item in manifest["files"].items():
            file = sdk / relative
            assert file.stat().st_size == item["bytes"] and sha(file) == item["sha256"], relative
        seed = sdk / manifest["compiler_seed"]["path"]
        source = sdk / manifest["compiler_entry"]
        example_sources = sdk / "examples/frontend_modules"
        example = example_sources / "main.hua"
        launcher = sdk / "bin/hua-build.ps1"
        example_artifact = work / "from seed.huab"
        version, _ = run("host-version", [host, "version"])
        assert version.splitlines() == manifest["host_version"]
        run("seed-example-build", launch(launcher, example, example_artifact))
        # Move the original module tree away from its compiled source path.
        moved_examples = external / "Moved Example Sources"
        example_sources.rename(moved_examples)
        assert not example.exists()
        baseline, _ = run("seed-example-run", [host, "run", example_artifact])
        assert baseline.strip() == "2 2", baseline

        # Exercise the SDK's error reporting from clean package locations.
        incomplete = external / "Incomplete SDK/bin"
        incomplete.mkdir(parents=True)
        shutil.copyfile(host, incomplete / host.name)
        shutil.copyfile(launcher, incomplete / launcher.name)
        _, error = run("missing-dll", launch(incomplete / launcher.name, example, example_artifact),
                       expected=1)
        assert "Hua SDK dependency missing:" in error and "wasmtime.dll" in error, error
        _, error = run("missing-seed", launch(launcher, example, example_artifact,
                                              "-Compiler", work / "missing seed.huab"), expected=1)
        assert "Hua SDK dependency missing:" in error and "missing seed.huab" in error, error

        c2, c3 = work / "compiler C2.huab", work / "compiler C3.huab"
        receipt = compile_huab("C2", seed, source, c2)
        assert receipt == compile_huab("C3", c2, source, c3)
        assert c2.read_bytes() == c3.read_bytes(), "C2 and C3 differ"
        report["fixed_point"] = {"sha256": sha(c2), "receipt": receipt}
        source.parent.rename(external / "Moved Compiler Sources")
        assert not source.exists()
        for generation, artifact in (("C2", c2), ("C3", c3)):
            run(f"{generation}-check", [host, "check", artifact])
            generated = work / f"from {generation}.huab"
            compile_huab(f"{generation}-example-build", artifact, moved_examples / "main.hua", generated)
            assert generated.read_bytes() == example_artifact.read_bytes()
            stdout, _ = run(f"{generation}-example-run", [host, "run", generated])
            assert stdout == baseline
        report["status"] = "passed"
    finally:
        if report["status"] == "running":
            report["status"] = "failed"
        save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    package = args.package.resolve()
    evidence = args.evidence.resolve()
    repository = Path(__file__).resolve().parents[1]
    if Path(tempfile.gettempdir()).resolve().is_relative_to(repository):
        parser.error("TEMP must point outside the Hua repository")
    evidence.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="Hua SDK acceptance ") as directory:
        verify(package, evidence, Path(directory))


if __name__ == "__main__":
    main()
