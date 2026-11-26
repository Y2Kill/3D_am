"""Run golden ZIP expectations against the compiled CLI; arrays are unordered."""
import argparse
import json
from pathlib import Path
import subprocess

PROCESS_TIMEOUT_SECONDS = 60  # hypothesis; guard for a stalled CLI on slow CI/WSL


import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from expect_match import mismatch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--am", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--case")
    args = parser.parse_args()
    cases = sorted(args.corpus.glob("*/expected.json"))
    if args.case:
        cases = [p for p in cases if p.parent.name == args.case]
    if not cases:
        parser.error("no golden cases selected")
    failures, checks = 0, 0
    for path in cases:
        expected = json.loads(path.read_text(encoding="utf-8"))
        for check in expected["checks"]:
            checks += 1
            command = [str(args.am.resolve()), "asset", check["command"],
                       str((path.parent / expected["input"]["zip"]).resolve()), *check.get("args", []), "--json"]
            label = f"{expected['case']}: {' '.join(command)}"
            try:
                result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="strict", timeout=PROCESS_TIMEOUT_SECONDS)
                actual = json.loads(result.stdout)
                error = mismatch(check["json"], actual)
                if result.returncode != check["exit_code"]:
                    error = f"$.exit_code: expected {check['exit_code']}, got {result.returncode}; {error or actual}"
                if error:
                    failures += 1
                    print(f"FAIL {label}\n  {error}\n  stderr: {result.stderr.strip()}")
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                failures += 1
                print(f"FAIL {label}\n  $: {error}")
    print(f"golden_zip: {len(cases)} cases, {checks} checks, {failures} failures")
    return int(failures != 0)


if __name__ == "__main__":
    raise SystemExit(main())
