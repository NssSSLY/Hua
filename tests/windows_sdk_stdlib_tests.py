"""Check the packaged Hua source standard module through both compilers."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


SOURCE = """
import std.alg_integer as integer
import std.alg as generic
fn main() {
    let empty []int = []
    let values = [-9223372036854775807,1,1,2,9223372036854775807]
    print(integer.lower_bound(empty,1),generic.lower_bound(empty,1))
    for key in [-9223372036854775807,1,2,3,9223372036854775807] {
        print(integer.lower_bound(values,key),generic.lower_bound(values,key))
    }
    print(integer.lower_bound([1,1,2],3),generic.lower_bound([1,1,2],3))
    print(values[1],values[2])
}
"""
EXPECTED = "0 0\n0 0\n1 1\n3 3\n4 4\n4 4\n3 3\n1 1\n"
RELATIVE = "src/std/alg_integer.hua"
LOGICAL_ID = b"@sdk/src/std/alg_integer.hua"


def verify(sdk):
    with tempfile.TemporaryDirectory(prefix="Hua SDK standard module ") as temporary:
        outside = Path(temporary)
        package = outside / "Installed Hua SDK"
        project = outside / "Unrelated Project"
        shutil.copytree(sdk, package)
        project.mkdir()
        host = package / "bin/hua.exe"
        manifest_file = package / "manifest.json"
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        module = package / RELATIVE
        dep = manifest["files"][RELATIVE]
        assert dep["sha256"] == hashlib.sha256(module.read_bytes()).hexdigest()
        assert dep["bytes"] == module.stat().st_size

        entry = project / "main.hua"
        entry.write_text(SOURCE.lstrip(), encoding="utf-8")
        (project / "std").mkdir()
        (project / "std/alg_integer.hua").write_text(
            "pub fn lower_bound(xs []int, key int) int { return 99 }\n", encoding="utf-8"
        )
        environment = os.environ.copy()
        environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
        environment["HUA_HOME"] = str(package)
        powershell = Path(environment["PATH"]) / "WindowsPowerShell/v1.0/powershell.exe"

        def invoke(*args, env=environment):
            return subprocess.run(args, cwd=project, env=env, timeout=30, text=True,
                                  encoding="utf-8", capture_output=True)

        def host_build(output, env=environment):
            return invoke(host, "build", entry, "-o", output, env=env)

        def hua_build(output, env=environment):
            launcher = package / "bin/hua-build.ps1"
            return invoke(powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                          launcher, entry, output, env=env)

        stale_environment = {**environment, "HUA_HOME": str(project)}
        products = [project / "host.huab", project / "selfhost.huab"]
        for result in (host_build(products[0]), hua_build(products[1], stale_environment)):
            assert result.returncode == 0 and not result.stderr, result
        for path in products:
            data = path.read_bytes()
            assert LOGICAL_ID in data
            assert int.from_bytes(data[8:12], "little") == 6
            assert invoke(host, "check", path).returncode == 0

        # The package root and project are distinct. Explicitly remove both source files
        # from their original lookup paths before running either archive.
        saved_entry = entry.rename(entry.with_suffix(".saved"))
        saved_module = module.rename(module.with_suffix(".saved"))
        outside_env = environment.copy()
        outside_env.pop("HUA_HOME")
        for path in products:
            result = invoke(host, "run", path, env=outside_env)
            assert (result.returncode, result.stdout, result.stderr) == (0, EXPECTED, ""), result
        saved_entry.rename(entry)

        def rejected(build, name, expected, env=environment):
            target = project / f"{name}.huab"
            target.write_bytes(b"keep this output")
            result = build(target, env)
            assert result.returncode != 0 and expected in result.stderr, result
            assert target.read_bytes() == b"keep this output"

        rejected(host_build, "host-missing-source", "E5001")
        rejected(hua_build, "hua-missing-source", "E5001")
        saved_module.rename(module)
        old_manifest = manifest_file.read_bytes()
        manifest_file.write_text("{", encoding="utf-8")
        rejected(host_build, "host-broken-manifest", "E5001")
        rejected(hua_build, "hua-broken-manifest", "E5001")
        manifest_file.write_bytes(old_manifest)

        # Windows junctions look like directories to symlink_status.
        redirected = module.parent.rename(outside / "External Library")
        junction = module.parent
        link = invoke(Path(environment["PATH"]) / "cmd.exe", "/d", "/c",
                      "mklink", "/J", junction, redirected)
        assert link.returncode == 0, link
        try:
            rejected(host_build, "host-junction", "E5001")
            rejected(hua_build, "hua-junction", "E5001")
        finally:
            junction.rmdir()
            redirected.rename(junction)
        rejected(host_build, "host-no-root", "E5001", outside_env)
        direct = invoke(host,"run",package / manifest["compiler_seed"]["path"],
                        "--execution-profile","compiler","--", entry, project / "no-root.huab",
                        env=outside_env)
        failure = json.loads(direct.stdout)
        assert failure["status"] == "error" and "E5001" in failure["error"], direct
        assert not (project / "no-root.huab").exists()
        entry.write_text('import std.alg_integer as integer\n'
                         'fn main() { print(integer.lower_bound(["x"],1)) }\n', encoding="utf-8")
        rejected(host_build, "host-signature", "E3004")
        rejected(hua_build, "hua-signature", "E3004")

        entry.write_text('import std.fs as fs\n'
                         'fn main() { print(is_ok(fs.canonical("."))) }\n', encoding="utf-8")
        canonical = project / "canonical.huab"
        result = hua_build(canonical)
        assert result.returncode == 0, result
        assert int.from_bytes(canonical.read_bytes()[8:12], "little") == 17
        checked = invoke(host, "check", canonical)
        assert checked.returncode == 0, checked

        # Rebuild a program after changing only the Hua library implementation.
        entry.write_text('import std.alg_integer as integer\n'
                         'fn main() { print(integer.lower_bound([1,2],2)) }\n', encoding="utf-8")
        implementation = module.read_text(encoding="utf-8")
        assert implementation.count("return low") == 1
        module.write_text(implementation.replace("return low", "return low + 100"), encoding="utf-8")
        dep["bytes"] = module.stat().st_size
        dep["sha256"] = hashlib.sha256(module.read_bytes()).hexdigest()
        manifest_file.write_text(json.dumps(manifest), encoding="utf-8")
        for build, product in zip((host_build, hua_build), products):
            assert build(product).returncode == 0
            result = invoke(host, "run", product)
            assert (result.returncode, result.stdout) == (0, "101\n"), result

    print("Passed SDK module relocation, signature, namespace, compatibility and diagnostics")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    args = parser.parse_args()
    verify(args.sdk.resolve())
