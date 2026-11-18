"""Assemble a relocatable Windows x64 seed SDK from an existing Release build."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zlib


ROOT = Path(__file__).resolve().parents[1]
ENTRY = "archive_main.hua"
TIMEOUT = 600

# These compiler modules use flat, named imports; the standard modules ship in Hua.
LOCAL_IMPORT = re.compile(r"^import ([a-zA-Z_][a-zA-Z_0-9]*) as [a-zA-Z_][a-zA-Z_0-9]*$")


def compiler_sources():
    pending = [ENTRY]
    sources = set()
    while pending:
        module = pending.pop()
        if module in sources:
            continue
        source = ROOT / "selfhost" / module
        for line in source.read_text(encoding="utf-8-sig").splitlines():
            line = line.strip()
            if line.startswith("import ") and not line.startswith("import std."):
                match = LOCAL_IMPORT.fullmatch(line)
                if not match:
                    raise ValueError(f"{source}: unsupported compiler import {line!r}")
                pending.append(f"{match[1]}.hua")
        sources.add(module)
    return sorted(sources)


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def archive_info(file):
    data = file.read_bytes()
    if len(data) < 32:
        raise ValueError(f"Truncated HUAB header: {file}")
    magic, version, abi, size, crc, flags = struct.unpack("<8sIIQII", data[:32])
    if (magic != b"HUAB\r\n\x1a\n" or abi != 1 or flags != 0
            or size != len(data) - 32 or crc != zlib.crc32(data[32:])):
        raise ValueError(f"Invalid HUAB header or payload: {file}")
    return {"format": version, "native_abi": abi, "crc32": crc}


def require_x64(file):
    with file.open("rb") as image:
        if image.read(2) != b"MZ":
            raise ValueError(f"Not a Windows executable: {file}")
        image.seek(0x3C)
        image.seek(int.from_bytes(image.read(4), "little"))
        if image.read(6) != b"PE\0\0\x64\x86":
            raise ValueError(f"Expected a Windows x64 image: {file}")


def clean_environment():
    env = os.environ.copy()
    system_root = os.environ["SystemRoot"]
    env["PATH"] = os.path.join(system_root, "System32")
    env.pop("HUA_HOME", None)
    env.pop("HUA_PATH", None)
    return env


def assemble(build, destination):
    for name in ("hua.exe", "wasmtime.dll"):
        require_x64(build / name)
    if destination.exists() and (not destination.is_dir() or any(destination.iterdir())):
        raise ValueError(f"Use a new or empty SDK directory: {destination}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hua-sdk-", dir=destination.parent) as scratch:
        sdk = Path(scratch) / "package"
        sdk.mkdir()
        files = {}

        def record(file, source, role):
            files[file.relative_to(sdk).as_posix()] = {
                "role": role, "source": source, "bytes": file.stat().st_size,
                "sha256": digest(file),
            }

        def copy(source, relative, role):
            file = sdk / relative
            file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, file)
            origin = source.relative_to(ROOT) if source.is_relative_to(ROOT) else Path(source.name)
            record(file, origin.as_posix(), role)

        copy(build / "hua.exe", "bin/hua.exe", "host")
        copy(build / "wasmtime.dll", "bin/wasmtime.dll", "host_dependency")
        copy(build / "wasmtime-LICENSE.txt", "licenses/Wasmtime.txt", "license")
        copy(ROOT / "LICENSE", "licenses/Hua.txt", "license")
        copy(ROOT / "scripts/hua-build.ps1", "bin/hua-build.ps1", "launcher")
        copy(ROOT / "std/alg_integer.hua", "src/std/alg_integer.hua", "stdlib_source")
        for module in compiler_sources():
            copy(ROOT / "selfhost" / module, f"src/compiler/{module}", "compiler_source")
        copy(ROOT / "selfhost/contract.json", "src/compiler/contract.json", "compiler_contract")
        for module in ("main.hua", "model.hua"):
            relative = f"examples/frontend_modules/{module}"
            copy(ROOT / relative, relative, "example")

        def run(*args):
            return subprocess.run(
                [str(sdk / "bin/hua.exe"), *args], cwd=sdk, env=clean_environment(),
                check=True, timeout=TIMEOUT, stdout=subprocess.PIPE, text=True,
                encoding="utf-8",
            ).stdout.strip()

        version = run("version")
        seed = sdk / "lib/bootstrap/compiler.huab"
        seed.parent.mkdir(parents=True)
        print("Building C1 with the packaged Hua host", flush=True)
        compiler_entry = f"src/compiler/{ENTRY}"
        seed_name = seed.relative_to(sdk).as_posix()
        run("build", compiler_entry, "-o", seed_name)
        info = archive_info(seed)
        if info["format"] < 7:
            raise ValueError(f"Seed requires at least HUAB 7, found {info['format']}")
        run("check", seed_name)
        record(seed, compiler_entry, "compiler_seed")

        manifest = {
            "schema": "hua.sdk.v1",
            "target": "windows-x86_64",
            "host_version": version.splitlines(),
            "host_runtime_dependencies": ["bin/wasmtime.dll", "Windows system DLLs and UCRT"],
            "compiler_entry": compiler_entry,
            "source_modules": {"std.alg_integer": "src/std/alg_integer.hua"},
            "compiler_seed": {
                "path": seed_name, "producer": "bin/hua.exe build",
                "producer_sha256": files["bin/hua.exe"]["sha256"], **info,
                "invocation": "bin/hua.exe run <compiler.huab> --execution-profile compiler -- <source> <output.huab>",
            },
            "files": files,
        }
        (sdk / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8",
        )
        # Publish the manifest last, after all files it describes are present.
        for relative in [*files, "manifest.json"]:
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with (sdk / relative).open("rb") as source, target.open("xb") as output:
                shutil.copyfileobj(source, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--destination", type=Path, default=ROOT / "huaBin")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Windows is required to assemble and check this SDK")
    try:
        assemble(args.build_dir.resolve(), args.destination.resolve())
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"SDK assembly failed: {exc}\n")
    print(f"SDK assembled at {args.destination.resolve()}", flush=True)


if __name__ == "__main__":
    main()
