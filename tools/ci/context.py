"""Values derived from the checkout and compiler, shared by local scripts and CI."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def baseline():
    value = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))["builtin-baseline"]
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("vcpkg.json must contain a full builtin-baseline commit SHA")
    return value


def compiler_key():
    if os.name == "nt":
        vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
        installation = subprocess.check_output([
            str(vswhere), "-latest", "-products", "*", "-version", "[17.0,18.0)",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath",
        ], text=True).strip()
        if not installation:
            raise ValueError("Visual Studio 2022 with x64 C++ tools was not found by vswhere")
        temp = ROOT / ".cache/tmp"
        temp.mkdir(parents=True, exist_ok=True)
        probe = temp / "ci-compiler.cmd"
        probe.write_text(
            '@echo off\ncall "' + installation + '\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1\n'
            'cl 2>&1\n', encoding="utf-8")
        result = subprocess.run([os.environ["COMSPEC"], "/d", "/c", str(probe)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if b"Microsoft" not in result.stdout or b"C/C++" not in result.stdout:
            raise ValueError("Cannot read the MSVC compiler version")
        version = re.search(rb"\b\d+\.\d+\.\d+\b", result.stdout)
        if version is None:
            raise ValueError("Cannot parse the MSVC compiler version")
        output = b"cl " + version.group() + b" x64\n"
    else:
        output = subprocess.check_output(["gcc", "--version"])
    print(output.decode(errors="replace").strip(), file=sys.stderr)
    return hashlib.sha256(output).hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("value", choices=("baseline", "compiler-key"))
    args = parser.parse_args()
    try:
        print(baseline() if args.value == "baseline" else "compiler=" + compiler_key())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"ERROR: {error}\n")
