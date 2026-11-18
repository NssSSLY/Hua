"""Build two successive generations with the SDK's previous seed."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time


def build_generations(sdk, source, evidence):
    host = sdk / "bin/hua.exe"
    seed = sdk / "lib/bootstrap/compiler.huab"
    snapshot = evidence / "compiler source"
    shutil.copytree(source, snapshot, ignore=shutil.ignore_patterns("*.huab"))
    entry = snapshot / "archive_main.hua"
    report = {
        "seed_sha256": hashlib.sha256(seed.read_bytes()).hexdigest(),
        "sources": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in sorted(snapshot.glob("*.hua"))},
        "status": "running", "generations": {},
    }

    def save():
        (evidence / "bootstrap.json").write_text(json.dumps(report, indent=2) + "\n",
                                                encoding="utf-8")

    try:
        for name in ("C2", "C3"):
            output = evidence / f"{name}.huab"
            command = [host, "run", seed, "--execution-profile", "compiler",
                       "--report-budget", "--", entry, output]
            row = {"command": [str(part) for part in command]}
            report["generations"][name] = row
            save()
            print(f"{name}: compiling {entry.name}", flush=True)
            start = time.perf_counter()
            with ((evidence / f"{name}.stdout").open("wb") as stdout,
                  (evidence / f"{name}.stderr").open("wb") as stderr):
                try:
                    result = subprocess.run(command, cwd=evidence, stdout=stdout, stderr=stderr,
                                            timeout=600)
                    row["exit_code"] = result.returncode
                except subprocess.TimeoutExpired:
                    row["timeout"] = True
                    raise
                finally:
                    row["seconds"] = round(time.perf_counter() - start, 3)
                    save()
            receipt = json.loads((evidence / f"{name}.stdout").read_text(encoding="utf-8"))
            row["receipt"] = receipt
            assert result.returncode == 0 and receipt["status"] == "ok", row
            row["sha256"] = hashlib.sha256(output.read_bytes()).hexdigest()
            budget = (evidence / f"{name}.stderr").read_text(encoding="utf-8").splitlines()
            row["budget"] = next(json.loads(line) for line in budget
                                 if line.startswith('{"schema":"hua.execution.budget.v1"'))
            save()
            print(f"{name}: {row['seconds']} s, {row['sha256']}", flush=True)
            subprocess.run([host, "check", output], cwd=evidence, timeout=30, check=True)
            suite = Path(__file__).with_name("selfhost_async_tests.py")
            subprocess.run([sys.executable, suite, host, output], cwd=evidence, timeout=240,
                           check=True)
            seed = output

        assert (evidence / "C2.huab").read_bytes() == (evidence / "C3.huab").read_bytes()
        report["status"] = "passed"
    finally:
        if report["status"] == "running":
            report["status"] = "failed"
        save()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("source", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=False)
    build_generations(args.sdk.resolve(), args.source.resolve(), args.evidence.resolve())
