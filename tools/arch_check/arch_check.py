"""Architecture checks (ТЗ §12.1.5, §12.9.1; ADR-0024).

1. #include rules: a file in level X may include "am/<Y>/..." only if Y is in
   ALLOWED[X]. Applies to include/am/<level>/** and src/<level>/**.
   The plugin SDK (sdk/include) must not include any internal header.
2. CMake target graph (optional, --build-dir): no cycles between am_* targets,
   and every am_<X> -> am_<Y> link must be allowed by the same matrix.
   Also no am_* level may link Qt/GUI targets except the application level.

Exit code 0 = clean, 1 = violations (printed), 2 = usage/tool error.
Usage: python arch_check.py --root <repo> [--build-dir <cmake build dir>]
"""
import argparse
import pathlib
import re
import subprocess
import sys

# Allowed direct dependencies between levels (transitive closure is implied).
ALLOWED = {
    "foundation": set(),
    "contracts": {"foundation"},
    "domain": {"foundation", "contracts"},
    "jobs": {"foundation", "contracts"},
    "storage": {"foundation", "contracts", "domain"},
    "processing": {"foundation", "contracts", "domain"},
    "services": {"foundation", "contracts", "domain", "jobs", "storage", "processing"},
    "automation": {"foundation", "contracts", "domain", "jobs"},
    "plugin_runtime": {"foundation", "contracts", "domain"},
    "ai": {"foundation", "contracts", "domain"},
    "application": {"foundation", "contracts", "domain", "jobs", "storage", "processing",
                    "services", "automation", "plugin_runtime", "ai"},
}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]am/([a-z_]+)/', re.M)
SRC_EXT = {".hpp", ".h", ".cpp", ".cc", ".ipp", ".in"}


def closure(level):
    seen, stack = set(), list(ALLOWED[level])
    while stack:
        x = stack.pop()
        if x not in seen:
            seen.add(x)
            stack.extend(ALLOWED[x])
    return seen


def check_includes(root):
    errors = []
    for base in (root / "include" / "am", root / "src"):
        if not base.is_dir():
            continue
        for level_dir in base.iterdir():
            if not level_dir.is_dir():
                continue
            level = level_dir.name
            if level not in ALLOWED:
                errors.append(f"unknown level directory: {level_dir.relative_to(root)}")
                continue
            ok = closure(level) | {level}
            for f in level_dir.rglob("*"):
                if f.suffix not in SRC_EXT:
                    continue
                text = f.read_text(encoding="utf-8", errors="replace")
                for m in INCLUDE_RE.finditer(text):
                    dep = m.group(1)
                    if dep == "storage" and f.is_relative_to(root / "src" / "application" / "cli"):
                        errors.append(f"{f.relative_to(root)}: CLI must use the application facade, not storage headers")
                    if dep not in ok:
                        errors.append(f"{f.relative_to(root)}: level '{level}' must not include am/{dep}/")
    sdk = root / "sdk" / "include"
    if sdk.is_dir():
        for f in sdk.rglob("*.h*"):
            for m in INCLUDE_RE.finditer(f.read_text(encoding="utf-8", errors="replace")):
                errors.append(f"{f.relative_to(root)}: SDK must not include internal header am/{m.group(1)}/")
    return errors


def check_cmake_graph(build_dir):
    dot_dir = build_dir / "arch_check"
    dot_dir.mkdir(exist_ok=True)
    dot = dot_dir / "targets.dot"
    r = subprocess.run(["cmake", f"--graphviz={dot}", str(build_dir)], capture_output=True, text=True)
    if r.returncode != 0 or not dot.exists():
        print(r.stdout, r.stderr, file=sys.stderr)
        raise RuntimeError("cmake --graphviz failed")
    text = dot.read_text(encoding="utf-8", errors="replace")
    # Labels look like "am_storage\n(am::storage)": keep the real target name.
    labels = {n: lab.split("\\n")[0]
              for n, lab in re.findall(r'"(node\d+)"\s*\[\s*label\s*=\s*"([^"]+)"', text)}
    if not any(v.startswith("am_") for v in labels.values()):
        raise RuntimeError("no am_* targets found in graphviz output (format changed?)")
    edges = {}
    for a, b in re.findall(r'"(node\d+)"\s*->\s*"(node\d+)"', text):
        # A self-edge is not a dependency between targets: CMake cannot link a
        # target to itself. It appears when a POST_BUILD command refers to
        # $<TARGET_FILE:self>, e.g. vcpkg's z-applocal DLL copy step.
        if labels[a] == labels[b]:
            continue
        edges.setdefault(labels[a], set()).add(labels[b])

    errors = []
    lvl = lambda t: t[3:] if t.startswith("am_") and t[3:] in ALLOWED else None
    for src, dsts in edges.items():
        s = lvl(src)
        if s is None:
            continue
        for d in dsts:
            dl = lvl(d)
            if dl is not None and dl not in closure(s):
                errors.append(f"CMake: {src} -> {d} is not allowed (ADR-0024)")
            if dl is None and s != "application" and re.match(r"(?i)^qt6?::|^qt6", d):
                errors.append(f"CMake: {src} -> {d}: only the application level may depend on GUI")

    # cycle detection (DFS, colours)
    WHITE, GREY, BLACK = 0, 1, 2
    colour = {}

    def dfs(n, path):
        colour[n] = GREY
        for m in edges.get(n, ()):
            c = colour.get(m, WHITE)
            if c == GREY:
                cyc = path[path.index(m):] + [m] if m in path else [n, m]
                errors.append("CMake cycle: " + " -> ".join(cyc))
            elif c == WHITE:
                dfs(m, path + [m])
        colour[n] = BLACK

    for n in list(edges):
        if colour.get(n, WHITE) == WHITE:
            dfs(n, [n])
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--build-dir")
    a = ap.parse_args()
    root = pathlib.Path(a.root).resolve()
    errors = check_includes(root)
    if a.build_dir:
        try:
            errors += check_cmake_graph(pathlib.Path(a.build_dir).resolve())
        except (RuntimeError, OSError) as e:
            print(f"arch_check: {e}", file=sys.stderr)
            return 2
    for e in errors:
        print(e)
    print(f"arch_check: {len(errors)} violation(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
