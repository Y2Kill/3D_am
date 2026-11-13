"""Prepare a project-local vcpkg clone at the manifest's exact baseline."""
from pathlib import Path
import argparse
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from context import ROOT, baseline


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def prepare(target):
    started = time.monotonic()
    target = target.resolve()
    if not target.is_relative_to((ROOT / ".cache").resolve()) or target == (ROOT / ".cache").resolve():
        raise ValueError("The vcpkg clone must be inside the project's .cache directory")
    expected = baseline()
    target.parent.mkdir(parents=True, exist_ok=True)
    created = not target.exists()
    if created:
        run("git", "-c", "core.autocrlf=false", "clone", "--filter=tree:0", "--depth", "1", "--no-checkout",
            "https://github.com/microsoft/vcpkg.git", str(target))
    elif not (target / ".git").exists():
        raise ValueError(f"Not a vcpkg git clone: {target}")
    current = subprocess.check_output(["git", "-C", str(target), "rev-parse", "HEAD"], text=True).strip()
    executable = target / ("vcpkg.exe" if os.name == "nt" else "vcpkg")
    if current == expected and executable.is_file():
        print(f"vcpkg unchanged: {expected} ({time.monotonic() - started:.2f} s)")
        return
    if not created:
        dirty = subprocess.check_output(["git", "-C", str(target), "status", "--porcelain", "--untracked-files=no"], text=True)
        if dirty:
            raise ValueError("The vcpkg clone has tracked changes; refusing to replace them")
    present = subprocess.run(["git", "-C", str(target), "cat-file", "-e", expected + "^{commit}"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if present.returncode:
        run("git", "-C", str(target), "fetch", "--filter=tree:0", "--depth", "1", "origin", expected)
    run("git", "-c", "core.autocrlf=false", "-C", str(target), "checkout", "--detach", expected)
    env = dict(os.environ, VCPKG_DISABLE_METRICS="1")
    script = target / ("bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh")
    run(str(script), "-disableMetrics", cwd=target, env=env, shell=os.name == "nt")
    print(f"vcpkg ready: {expected} ({time.monotonic() - started:.2f} s)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path,
                        default=ROOT / ".cache" / ("vcpkg" if os.name == "nt" else "vcpkg-linux"))
    args = parser.parse_args()
    try:
        prepare(args.directory)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"ERROR: {error}\n")
