"""Trace test annotations to CTest results (stdlib only; no coverage inference).

Closed ID grammar (T-013, decision V-1): requirements/decisions K-n,
NFR-14.x.y, SC-n, ADR-nnnn, O-nn, R-nn[.n], X-nn, MF-nn; acceptance
cases S-nn, M-nn, N-nn, D-nn, U-nn. Digits are ASCII, fixed widths are
significant. Properties split on '|'; each req:* CTest label is one ID.
Other IDs fail the report with their source locations. Matrix membership
uses exact tokens anywhere in the file, including expanded same-family
ranges such as M-01...M-03 (also the Unicode ellipsis and en/em dash).
Unknown IDs and matrix rows without annotated tests are informational.
Row associations use the requirement column and referenced acceptance cases;
incidental requirement/ADR mentions in descriptive cells do not cover a row.
CTest JUnit is authoritative for outcomes; GTest XML supplies annotations.
CTest command filters join GTest's canonical names to pretty CTest names.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

PROCESS_TIMEOUT_SECONDS = 60  # hypothesis; CTest discovery on slow CI/WSL
SELFTEST_TIMEOUT_SECONDS = 30  # hypothesis; isolated report subprocess


REQUIREMENT = r"(?:K-[0-9]+|NFR-14\.[0-9]+\.[0-9]+|SC-[0-9]+|ADR-[0-9]{4}|O-[0-9]{2}|R-[0-9]{2}(?:\.[0-9]+)?|X-[0-9]{2}|MF-[0-9]{2})"
CASE = r"[SMNDU]-[0-9]{2}"
ID = f"(?:{REQUIREMENT}|{CASE})"
TOKEN = re.compile(rf"(?<![A-Za-z0-9_.-]){ID}(?![A-Za-z0-9_.-])")
RANGE = re.compile(rf"(?P<left>{ID})\s*(?:…|\.?\.\.|–|—|-)\s*(?P<right>{ID})")


def kind(value):
    if re.fullmatch(REQUIREMENT, value):
        return "requirement"
    if re.fullmatch(CASE, value):
        return "case"
    return None


def matrix_ids(text):
    ids = set(TOKEN.findall(text))
    for match in RANGE.finditer(text):
        left, right = match.group("left", "right")
        a = re.fullmatch(r"(.+-)([0-9.]+)", left)
        b = re.fullmatch(r"(.+-)([0-9.]+)", right)
        if a[1] != b[1]:
            continue
        x, y = a[2].split("."), b[2].split(".")
        if x[:-1] != y[:-1] or len(x) != len(y):
            continue
        for number in range(int(x[-1]), int(y[-1]) + 1):
            value = a[1] + ".".join(x[:-1] + [str(number).zfill(len(x[-1]))])
            if kind(value):
                ids.add(value)
    return ids


def aggregate(statuses):
    for status in ("FAIL", "NOT_RUN", "PASS", "SKIP"):
        if status in statuses:
            return status
    return "NOT_RUN"


def collect(metadata, junit, gtest_files, matrix_text):
    tests = {test["name"]: {"name": test["name"], "status": "NOT_RUN", "ids": set()}
             for test in metadata["tests"]}
    invalid = []
    canonical = {}

    def annotate(name, raw, source):
        if kind(raw):
            tests[name]["ids"].add(raw)
        else:
            invalid.append({"id": raw, "test": name, "source": source})

    for test in metadata["tests"]:
        name = test["name"]
        for argument in test.get("command", []):
            if argument.startswith("--gtest_filter="):
                canonical.setdefault(argument.split("=", 1)[1], []).append(name)
        for prop in test.get("properties", []):
            if prop["name"] == "LABELS":
                for label in prop["value"]:
                    if label.startswith("req:"):
                        annotate(name, label[4:], f"CTest label {label}")
    for test in ET.parse(junit).iter("testcase"):
        name = test.get("name")
        if name not in tests:
            continue
        status = "PASS"
        if test.find("failure") is not None or test.find("error") is not None:
            status = "FAIL"
        elif test.find("skipped") is not None or test.get("status") in ("notrun", "disabled"):
            status = "SKIP"
        tests[name]["status"] = status
    for path in sorted(gtest_files):
        for test in ET.parse(path).iter("testcase"):
            full_name = test.get("classname", "") + "." + test.get("name", "")
            names = canonical.get(full_name, [full_name] if full_name in tests else [])
            for prop in test.findall("./properties/property"):
                if prop.get("name") == "req":
                    for value in prop.get("value", "").split("|"):
                        for name in names:
                            annotate(name, value, f"{path.name}: {full_name}: property req")
    annotated = set().union(*(test["ids"] for test in tests.values()))
    entries = []
    for value in sorted(annotated):
        matching = [{"name": test["name"], "status": test["status"]}
                    for test in tests.values() if value in test["ids"]]
        entries.append({"id": value, "kind": kind(value), "status": aggregate([t["status"] for t in matching]),
                        "tests": matching})
    rows = []
    for line_number, line in enumerate(matrix_text.splitlines(), 1):
        if line.startswith("|") and matrix_ids(line.split("|", 2)[1]):
            ids = matrix_ids(line.split("|", 2)[1]) | {value for value in matrix_ids(line) if kind(value) == "case"}
            if not ids.intersection(annotated):
                rows.append({"line": line_number, "ids": sorted(ids), "text": line})
    return {"format": "am-req-report", "version": 1, "entries": entries,
            "tests": [{**test, "ids": sorted(test["ids"])} for test in tests.values()],
            "tests_without_req": [test["name"] for test in tests.values() if not test["ids"]],
            "ids_outside_matrix": sorted(annotated - matrix_ids(matrix_text)),
            "matrix_rows_without_tests": rows, "invalid_ids": invalid}


def markdown(report):
    lines = ["# Трассировка тестов", "", "PASS означает результат теста, а не автоматическую приёмку требования.", ""]
    for category, title in [("requirement", "Требования и решения"), ("case", "Случаи приёмки")]:
        lines += [f"## {title}", "", "| ID | Итог | Тесты и результаты |", "|---|---|---|"]
        for entry in report["entries"]:
            if entry["kind"] == category:
                tests = "; ".join(f"{t['name'].replace('|', '&#124;')}: {t['status']}" for t in entry["tests"])
                lines.append(f"| {entry['id']} | {entry['status']} | {tests} |")
        lines.append("")
    lines += [f"## Тесты без req ({len(report['tests_without_req'])})", ""]
    lines += [f"- {name}" for name in report["tests_without_req"]] or ["Нет."]
    lines += ["", "## ID вне матрицы (информационно)", ""]
    lines += [f"- {value} ({kind(value)})" for value in report["ids_outside_matrix"]] or ["Нет."]
    lines += ["", "## Строки матрицы без тестов (информационно)", ""]
    lines += [f"- Строка {row['line']}: {', '.join(row['ids'])}" for row in report["matrix_rows_without_tests"]] or ["Нет."]
    lines += ["", "## Неверные ID", ""]
    lines += [f"- `{item['id']}`: {item['test']}; {item['source']}" for item in report["invalid_ids"]] or ["Нет."]
    return "\n".join(lines) + "\n"


def write_report(report, output):
    output.mkdir(parents=True, exist_ok=True)
    (output / "req_report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "req_report.md").write_text(markdown(report), encoding="utf-8")


def selftest(root):
    fixture = root / "tests/tools/fixtures/req-report"
    for value in ("K-1", "NFR-14.7.1", "SC-1", "ADR-0038", "O-01", "R-09", "R-09.8", "X-02", "MF-01",
                  "S-01", "M-01", "N-01", "D-01", "U-01"):
        assert kind(value), value
    for value in ("M-1", "MF-1", "Q-01", "ADR-12", "req:K-1", "K-1|K-2", " K-1", ""):
        assert kind(value) is None, value
    assert matrix_ids("M-01…M-03 R-06.1…R-06.3 S-01..S-03") == {
        "M-01", "M-02", "M-03", "R-06.1", "R-06.2", "R-06.3", "S-01", "S-02", "S-03"}
    assert matrix_ids("M-1 Q-01 ADR-00381 R-09.1234x") == set()
    metadata = json.loads((fixture / "ctest.json").read_text(encoding="utf-8"))
    report = collect(metadata, fixture / "junit.xml", [fixture / "gtest.xml"], (fixture / "matrix.md").read_text(encoding="utf-8"))
    entries = {entry["id"]: entry for entry in report["entries"]}
    assert entries["ADR-0038"]["status"] == "FAIL"
    assert entries["M-02"]["kind"] == "case" and entries["M-02"]["status"] == "PASS"
    assert entries["K-3"]["status"] == "SKIP"
    assert report["tests_without_req"] == ["NoAnnotation", "NotRun"]
    assert report["ids_outside_matrix"] == ["O-99"]
    assert len(report["matrix_rows_without_tests"]) == 1
    assert report["invalid_ids"] == []
    bad = json.loads((fixture / "invalid-ctest.json").read_text(encoding="utf-8"))
    invalid = collect(bad, fixture / "junit.xml", [], "")
    assert {item["id"] for item in invalid["invalid_ids"]} == {"M-1", "MF-1", "Q-01", "ADR-12"}
    assert all(item["source"] and item["test"] for item in invalid["invalid_ids"])
    cache = root / ".cache"
    cache.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="req-report-selftest-", dir=cache) as directory:
        write_report(report, Path(directory))
        assert json.loads((Path(directory) / "req_report.json").read_text(encoding="utf-8")) == report
        assert "Случаи приёмки" in (Path(directory) / "req_report.md").read_text(encoding="utf-8")
        nested = Path(directory) / "gtest-xml" / "nested"
        nested.mkdir(parents=True)
        (nested / "gtest.xml").write_bytes((fixture / "gtest.xml").read_bytes())
        result = subprocess.run([sys.executable, "-I", str(Path(__file__).resolve()), "--build-dir", directory,
                                 "--matrix", str(fixture / "matrix.md"), "--ctest-json", str(fixture / "ctest.json"),
                                 "--junit", str(fixture / "junit.xml")], capture_output=True, encoding="utf-8", timeout=SELFTEST_TIMEOUT_SECONDS)
        assert result.returncode == 0, result
        nested_report = json.loads((Path(directory) / "req_report.json").read_text(encoding="utf-8"))
        assert any(entry["id"] == "M-02" for entry in nested_report["entries"])
        result = subprocess.run([sys.executable, "-I", str(Path(__file__).resolve()), "--build-dir", directory,
                                 "--matrix", str(fixture / "matrix.md"), "--ctest-json", str(fixture / "invalid-ctest.json"),
                                 "--junit", str(fixture / "junit.xml")], capture_output=True, encoding="utf-8", timeout=SELFTEST_TIMEOUT_SECONDS)
        assert result.returncode == 1 and "Q-01" in result.stdout and "BadIDs" in result.stdout, result
        assert len(json.loads((Path(directory) / "req_report.json").read_text(encoding="utf-8"))["invalid_ids"]) == 4
    print("PASS req_report_selftest: grammar, ranges, GTest joins, PASS/FAIL/SKIP/NOT_RUN, invalid IDs, output")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--matrix", type=Path)
    parser.add_argument("--ctest-json", type=Path, help="use a previously captured CTest inventory")
    parser.add_argument("--junit", type=Path, help="override the default build/junit.xml")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.selftest:
        return selftest(root)
    if not args.build_dir or not args.matrix:
        parser.error("--build-dir and --matrix are required")
    try:
        if args.ctest_json:
            metadata = json.loads(args.ctest_json.read_text(encoding="utf-8"))
        else:
            result = subprocess.run(["ctest", "--test-dir", str(args.build_dir.resolve()), "--show-only=json-v1"],
                                    check=True, capture_output=True, encoding="utf-8", timeout=PROCESS_TIMEOUT_SECONDS)
            metadata = json.loads(result.stdout)
        report = collect(metadata, args.junit or args.build_dir / "junit.xml", list((args.build_dir / "gtest-xml").rglob("*.xml")),
                         args.matrix.read_text(encoding="utf-8"))
        write_report(report, args.build_dir)
    except (OSError, ValueError, ET.ParseError, subprocess.SubprocessError) as error:
        print(f"FAIL req_report: {error}")
        return 1
    for item in report["invalid_ids"]:
        print(f"FAIL req_report: {item['id']!r}: {item['test']}: {item['source']}")
    print(f"req_report: {len(report['entries'])} IDs, {len(report['tests'])} tests, "
          f"{len(report['tests_without_req'])} without req, {len(report['invalid_ids'])} invalid IDs")
    return int(bool(report["invalid_ids"]))


if __name__ == "__main__":
    raise SystemExit(main())
