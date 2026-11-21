"""Build a Windows executable from C++ emitted by the Hua native generator."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
CLANG = ROOT / ".tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe"
WASMTIME = ROOT / ".tools/wasmtime-v49.0.2-x86_64-windows-c-api/lib/wasmtime.dll.lib"
SUPERVISOR_SECONDS = 600


def link_cpp(host, cpp, output, *, count_steps, timeout=None, runtime_dir=None):
    """Compile a generated translation unit against the selected runtime."""
    runtime = runtime_dir if runtime_dir is not None else host.parent
    return subprocess.run(
        [str(CLANG), "-std=c++20", "-O2", "-DNDEBUG",
         f"-DHUA_NATIVE_COUNT_STEPS={int(count_steps)}",
         "-I", str(ROOT / "include"), str(cpp), "-o", str(output),
         str(runtime / "libhua_semantics.a"), str(runtime / "libhua_frontend.a"),
         "-lpthread", "-lbcrypt", str(WASMTIME), "-static", "-Wl,--stack,8388608"],
        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout,
    )


def run(options):
    source = options.source.resolve()
    output = options.output.resolve()
    host = options.hua.resolve() if options.hua else None
    runtime = options.runtime_dir.resolve() if options.runtime_dir else host.parent
    environment = os.environ.copy()
    environment["PATH"] = str(runtime) + os.pathsep + environment.get("PATH", "")
    suffix = ".cpp" if options.emit_cpp else ".exe"
    if source.suffix != ".hua" or output.suffix != suffix:
        raise ValueError(f"expected a .hua input and a {suffix} output")
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix=".hua-native-", dir=output.parent) as staging:
        stage = Path(staging)
        cpp = stage / "generated.cpp"
        if options.native_generator:
            executable = options.native_generator.resolve()
            generator = executable
            command = [executable, "emit", source, cpp]
        else:
            executable = host
            generator = options.generator.resolve()
            command = [host, "run", generator, "--execution-profile", "compiler", "--", "emit", source, cpp]
        generated = subprocess.run(
            command, env=environment, capture_output=True, encoding="utf-8", timeout=SUPERVISOR_SECONDS,
        )
        generation_seconds = time.monotonic() - started
        if generated.returncode:
            sys.stderr.write(generated.stderr)
            return 3
        receipt = json.loads(generated.stdout)
        if receipt.get("schema") != "hua.native.cpp.v1" or receipt.get("status") not in ("ok", "error"):
            raise ValueError("invalid Hua native generator receipt")
        if receipt["status"] == "error":
            print(receipt["detail"], file=sys.stderr)
            return 2
        if generated.stderr or not cpp.is_file() or not cpp.stat().st_size:
            raise ValueError("Hua generator did not produce a clean C++ artifact")
        cpp_hash = hashlib.sha256(cpp.read_bytes()).hexdigest()
        product = cpp
        link_seconds = 0.0
        if not options.emit_cpp:
            link_started = time.monotonic()
            remaining = SUPERVISOR_SECONDS - (link_started - started)
            if remaining <= 0:
                raise subprocess.TimeoutExpired(str(CLANG), SUPERVISOR_SECONDS)
            product = stage / "program.exe"
            linked = link_cpp(host, cpp, product, count_steps=False, timeout=remaining, runtime_dir=runtime)
            if linked.returncode:
                sys.stderr.write(linked.stderr)
                return 3
            link_seconds = time.monotonic() - link_started
        retained = options.keep_cpp.resolve() if options.keep_cpp else None
        if retained:
            with tempfile.TemporaryDirectory(prefix=".hua-cpp-", dir=retained.parent) as directory:
                staged_cpp = Path(directory) / retained.name
                staged_cpp.write_bytes(cpp.read_bytes())
                os.replace(staged_cpp, retained)
        os.replace(product, output)
    print(json.dumps(dict(status="ok", cpp_sha256=cpp_hash, generator_seconds=generation_seconds,
                          generator_command=[str(arg) for arg in command],
                          generator_sha256=hashlib.sha256(generator.read_bytes()).hexdigest(),
                          link_seconds=link_seconds, total_seconds=time.monotonic() - started)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--hua", type=Path, help="host for a HUAB generator")
    generators = parser.add_mutually_exclusive_group(required=True)
    generators.add_argument("--generator", type=Path, help="Hua-generated generator HUAB")
    generators.add_argument("--native-generator", type=Path, help="native Hua generator executable")
    parser.add_argument("--runtime-dir", type=Path, help="prebuilt Hua runtime libraries and DLLs")
    parser.add_argument("--emit-cpp", action="store_true")
    parser.add_argument("--keep-cpp", type=Path, help="retain the C++ used by a successful EXE build")
    options = parser.parse_args()
    if options.generator and not options.hua:
        parser.error("--generator requires --hua")
    if not options.runtime_dir and not options.hua:
        parser.error("--native-generator requires --runtime-dir when --hua is absent")
    if options.keep_cpp and (options.emit_cpp or options.keep_cpp.suffix != ".cpp"):
        parser.error("--keep-cpp requires an EXE build and a .cpp path")
    try:
        return run(options)
    except subprocess.TimeoutExpired:
        print(f"native build exceeded {SUPERVISOR_SECONDS} seconds", file=sys.stderr)
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
    return 3


if __name__ == "__main__":
    sys.exit(main())
