"""Assemble a relocatable Windows x64 Hua SDK from prebuilt artifacts."""

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
from time import sleep
import zlib
import zipfile


ROOT = Path(__file__).resolve().parents[1]
ENTRY = "archive_main.hua"
TIMEOUT = 600

# These compiler modules use flat, named imports; the standard modules ship in Hua.
LOCAL_IMPORT = re.compile(r"^import ([a-zA-Z_][a-zA-Z_0-9]*) as [a-zA-Z_][a-zA-Z_0-9]*$")


def compiler_sources(*entries):
    pending = list(entries or (ENTRY,))
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


def prepare_release_assets(sdk, release, record):
    clang = sdk / release["clang"]
    config = clang.with_name("x86_64-w64-windows-gnu.cfg")
    compiler = [str(clang), "--no-default-config", "--config=" + str(config),
                "-std=c++20", "-O2", "-DNDEBUG", "-DHUA_NATIVE_COUNT_STEPS=0"]
    env = clean_environment()
    env["SOURCE_DATE_EPOCH"] = "0"
    env["PATH"] = str(clang.parent) + os.pathsep + env["PATH"]

    runtime = "lib/windows-x86_64/hua_native_scalar.obj"
    subprocess.run(
        [*compiler, "-ffile-prefix-map=" + str(ROOT) + "=.", "-ffunction-sections", "-fdata-sections",
         "-I", str(sdk / "include"), "-c", str(ROOT / "src/native_scalar.cpp"), "-o", runtime],
        cwd=sdk, env=env, check=True, timeout=TIMEOUT,
    )
    record(sdk / runtime, "src/native_scalar.cpp", "target_runtime")
    release["release_policy"] = "small-app-v1"
    release["scalar_runtime"] = runtime

    wrapper = "include/hua/release_runtime.hpp"
    pch = "include/hua/release_runtime.pch"
    (sdk / wrapper).write_bytes(b'#include "hua/native_rt.hpp"\n')
    dependencies = sdk / "release-pch.d"
    subprocess.run(
        [*compiler, "-I", str(sdk / "include"), "-isysroot", str(sdk),
         "-Xclang", "-relocatable-pch", "-Xclang", "-fvalidate-ast-input-files-content",
         "-MD", "-MF", str(dependencies), "-MT", "hua-release-pch", "-x", "c++-header", wrapper, "-o", pch],
        cwd=sdk, env=env, check=True, timeout=TIMEOUT,
    )
    body = dependencies.read_text(encoding="utf-8").partition(":")[2].replace("\\\n", " ")
    names = set()
    for token in re.findall(r"(?:\\[ #]|[^\s])+", body):
        dependency = Path(token.replace(r"\ ", " ").replace(r"\#", "#"))
        names.add((sdk / dependency).resolve().relative_to(sdk.resolve()).as_posix())
    dependencies.unlink()
    if wrapper not in names or "include/hua/native_rt.hpp" not in names:
        raise ValueError("Incomplete native precompiled header dependencies")
    inventory = "include/hua/release_runtime.json"
    inputs = [{"path": name, "sha256": digest(sdk / name)} for name in sorted(names)]
    (sdk / inventory).write_bytes((json.dumps(
        {"schema": "hua.pch.v1", "sha256": digest(sdk / pch), "inputs": inputs},
        separators=(",", ":"),
    ) + "\n").encode("utf-8"))
    for relative in (wrapper, pch, inventory):
        record(sdk / relative, "include/hua/native_rt.hpp", "precompiled_header")
    release["pch_manifest"] = inventory


def native_configuration(toolchain, build, sdk, copy):
    for binary, name in (("generator", "hua-generator"), ("archive", "hua-archive")):
        source = build / f"{binary}.exe"
        require_x64(source)
        copy(source, f"lib/compiler/{name}.exe", "native_compiler")
    for lib in ("libhua_semantics.a", "libhua_frontend.a"):
        copy(build / lib, f"lib/windows-x86_64/{lib}", "target_runtime")
    for source in sorted((ROOT / "include/hua").rglob("*")):
        if source.is_file():
            copy(source, source.relative_to(ROOT).as_posix(), "runtime_header")

    target = "toolchain/windows-x86_64"
    for file in ("clang++.exe", "clang-23.exe", "ld.lld.exe", "libclang-cpp.dll",
                 "libLLVM-23.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll",
                 "mingw32-common.cfg", "x86_64-w64-windows-gnu.cfg", "x86_64-pc-windows-gnu.cfg"):
        copy(toolchain / "bin" / file, f"{target}/bin/{file}", "toolchain")
    for directory in ("include", "lib/clang/23/include", "x86_64-w64-mingw32/lib"):
        for source in sorted((toolchain / directory).rglob("*")):
            if source.is_file():
                copy(source, f"{target}/{source.relative_to(toolchain).as_posix()}", "toolchain")
    builtins = "lib/clang/23/lib/windows/libclang_rt.builtins-x86_64.a"
    copy(toolchain / builtins, f"{target}/{builtins}", "toolchain")
    copy(toolchain / "LICENSE.TXT", "licenses/LLVM-MinGW.txt", "license")
    clang = f"{target}/bin/clang++.exe"
    version = subprocess.run(
        [str(sdk / clang), "--version"], cwd=sdk, env=clean_environment(),
        capture_output=True, check=True, timeout=30, text=True, encoding="utf-8",
    ).stdout.splitlines()[0]
    return {
        "schema": "hua.sdk.native.v1",
        "driver": "lib/compiler/release.huab",
        "generator": "lib/compiler/hua-generator.exe",
        "generator_mode": "modules-v1",
        "archive": "lib/compiler/hua-archive.exe",
        "generator_entry": "src/compiler/native_main.hua",
        "archive_entry": f"src/compiler/{ENTRY}",
        "clang": clang,
        "clang_version": version,
        "linker": f"{target}/bin/ld.lld.exe",
        "headers": "include",
        "semantics": "lib/windows-x86_64/libhua_semantics.a",
        "frontend": "lib/windows-x86_64/libhua_frontend.a",
    }


def assemble(build, destination, toolchain=None):
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
        copy(ROOT / "std/sse.hua", "src/std/sse.hua", "stdlib_source")
        copy(ROOT / "std/csv.hua", "src/std/csv.hua", "stdlib_source")
        entries = (ENTRY, "native_main.hua") if toolchain else (ENTRY,)
        for module in compiler_sources(*entries):
            copy(ROOT / "selfhost" / module, f"src/compiler/{module}", "compiler_source")
        copy(ROOT / "selfhost/contract.json", "src/compiler/contract.json", "compiler_contract")
        copy(ROOT / "selfhost/stdlib_manifest.json", "src/compiler/stdlib_manifest.json", "compiler_contract")
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
            "source_modules": {
                "std.alg_integer": "src/std/alg_integer.hua",
                "std.sse": "src/std/sse.hua",
                "std.csv": "src/std/csv.hua",
            },
            "compiler_seed": {
                "path": seed_name, "producer": "bin/hua.exe build",
                "producer_sha256": files["bin/hua.exe"]["sha256"], **info,
                "invocation": "bin/hua.exe run <compiler.huab> --execution-profile compiler -- <source> <output.huab>",
            },
            "files": files,
        }
        if toolchain:
            release = native_configuration(toolchain, build, sdk, copy)
            prepare_release_assets(sdk, release, record)
            for module in ("release_main.hua", "release_cache.hua"):
                copy(ROOT / f"selfhost/{module}", f"src/compiler/{module}", "driver_source")
            run("build", "src/compiler/release_main.hua", "-o", release["driver"])
            run("check", release["driver"])
            record(sdk / release["driver"], "src/compiler/release_main.hua", "release_driver")
            release["driver_archive"] = archive_info(sdk / release["driver"])
            toolchain_files = {name: item for name, item in files.items() if item["role"] == "toolchain"}
            for name in toolchain_files:
                del files[name]
            inventory = sdk / "toolchain/files.json"
            inventory.write_text(json.dumps(toolchain_files, sort_keys=True) + "\n", encoding="utf-8")
            record(inventory, release["clang"], "toolchain_inventory")
            release["toolchain_inventory"] = "toolchain/files.json"
            manifest["native_release"] = release

        (sdk / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8",
        )
        if toolchain:
            entry = "examples/frontend_modules/main.hua"
            env = clean_environment()
            env["HUA_HOME"] = str(sdk)
            probe = "native-probe.huab"
            command = subprocess.run([str(sdk / release["archive"]), entry, probe],
                cwd=sdk, env=env, capture_output=True, check=True, timeout=TIMEOUT,
                text=True, encoding="utf-8")
            receipt = json.loads(command.stdout)
            if receipt.get("schema") != "hua.archive.result.v1" or receipt.get("status") != "ok":
                raise ValueError(f"Native archive compiler failed: {command.stdout} {command.stderr}")
            release["archive_probe"] = archive_info(sdk / probe)
            run("check", probe)
            (sdk / probe).unlink()
            run("build", "--release", entry, "-o", "native-probe.exe")
            output = subprocess.run([str(sdk / "native-probe.exe")], cwd=scratch,
                env=clean_environment(), check=True, capture_output=True, text=True, timeout=30)
            if output.stdout.strip() != "2 2":
                raise ValueError(f"Native SDK smoke output: {output.stdout!r}")
            (sdk / "native-probe.exe").unlink()
            (sdk / "manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8",
            )
        # The completed package is moved onto its destination on the same volume.
        if destination.is_dir():
            destination.rmdir()
        for attempt in range(6):
            try:
                sdk.rename(destination)
                break
            except PermissionError:
                if attempt == 5 or destination.exists():
                    raise
                sleep(0.2 * (attempt + 1))


def write_sdk_zip(sdk, destination):
    if destination.exists() or destination.is_relative_to(sdk):
        raise ValueError(f"Use a new ZIP path outside the SDK: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hua-sdk-zip-", dir=destination.parent) as scratch:
        temporary = Path(scratch) / "package.zip"
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=3) as package:
            for file in sorted(sdk.rglob("*"), key=lambda path: path.relative_to(sdk).as_posix()):
                if not file.is_file():
                    continue
                name = "huaBin/" + file.relative_to(sdk).as_posix()
                info = zipfile.ZipInfo(name)
                info.compress_type = zipfile.ZIP_DEFLATED
                with file.open("rb") as source, package.open(info, "w") as output:
                    shutil.copyfileobj(source, output)
        temporary.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--destination", type=Path, default=ROOT / "huaBin")
    parser.add_argument("--toolchain", type=Path,
                        help="Windows x64 LLVM-MinGW root, enabling native release packaging")
    parser.add_argument("--zip", type=Path, help="Also create a portable ZIP containing huaBin")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Windows is required to assemble and check this SDK")
    try:
        assemble(args.build_dir.resolve(), args.destination.resolve(),
                 args.toolchain.resolve() if args.toolchain else None)
        if args.zip:
            write_sdk_zip(args.destination.resolve(), args.zip.resolve())
            print(f"SDK ZIP assembled at {args.zip.resolve()}", flush=True)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(1, f"SDK assembly failed: {exc}\n")
    print(f"SDK assembled at {args.destination.resolve()}", flush=True)


if __name__ == "__main__":
    main()
