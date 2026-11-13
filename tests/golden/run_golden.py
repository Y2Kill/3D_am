"""Run golden ZIP expectations against the compiled CLI; arrays are unordered."""
import argparse
import json
from pathlib import Path
import subprocess


def mismatch(pattern, actual, path="$"):
    if isinstance(pattern, dict):
        if not isinstance(actual, dict):
            return f"{path}: expected object, got {type(actual).__name__}"
        for key, value in pattern.items():
            if key not in actual:
                return f"{path}.{key}: missing key"
            error = mismatch(value, actual[key], f"{path}.{key}")
            if error:
                return error
    elif isinstance(pattern, list):
        if not isinstance(actual, list) or len(pattern) != len(actual):
            return f"{path}: expected array length {len(pattern)}, got {actual!r}"
        edges = [[j for j, item in enumerate(actual) if mismatch(value, item) is None] for value in pattern]
        matched = {}

        def assign(i, seen):
            for j in edges[i]:
                if j in seen:
                    continue
                seen.add(j)
                if j not in matched or assign(matched[j], seen):
                    matched[j] = i
                    return True
            return False

        for i in range(len(pattern)):
            if not assign(i, set()):
                return f"{path}[{i}]: no distinct subset match for {pattern[i]!r}; actual={actual!r}"
    elif type(pattern) is not type(actual) or pattern != actual:
        return f"{path}: expected {pattern!r}, got {actual!r}"
    return None


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
                result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="strict", timeout=60)
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
