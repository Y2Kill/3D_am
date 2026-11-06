"""Pairwise TZ version comparison: section sizes and sentences lost between versions.

Usage: python verdiff.py <drafts_dir> <ver_a> <ver_b> [<ver_c> ...]
Prints, for every consecutive pair (and first->last), per-section size deltas and
sentences of the older version whose best match in the newer one is below threshold.
"""
import re
import sys
import pathlib

root = pathlib.Path(sys.argv[1])
vers = sys.argv[2:]


def body(v):
    t = (root / f"TZ_catalog_3D_2D_draft_v{v}.md").read_text(encoding="utf-8")
    return t.split("## 18. История изменений")[0]


def sections(t):
    res, cur = {}, "0"
    for line in t.splitlines():
        m = re.match(r"^##\s+(\d+)\.", line)
        if m:
            cur = m.group(1)
        res[cur] = res.get(cur, 0) + len(line) + 1
    return res


def sents(t):
    out, sec = [], ""
    for line in t.splitlines():
        s = line.strip()
        if s.startswith("#"):
            sec = s.lstrip("# ")
            continue
        if not s or s.startswith("|---") or s == "---":
            continue
        for x in re.split(r"(?<=[.;:!?])\s+(?=[А-ЯA-Z*«(])", s):
            n = re.sub(r"\*\*|\[[^\]]*\]|`|^\s*(\d+\.|-|\|)\s*", "", x).lower()
            n = re.sub(r"\s+", " ", n).strip()
            if len(n) > 30:
                toks = {w[:6] for w in re.findall(r"[a-zа-яё0-9]{4,}", n)}
                if toks:
                    out.append((sec, x, toks))
    return out


def lost(a, b, thr=0.6):
    pool = [p[2] for p in sents(body(b))]
    res = []
    for sec, raw, ts in sents(body(a)):
        best = max(len(ts & bs) / len(ts) for bs in pool)
        if best < thr:
            res.append((best, sec, raw))
    return res


pairs = list(zip(vers, vers[1:]))
if len(vers) > 2:
    pairs.append((vers[0], vers[-1]))
for a, b in pairs:
    sa, sb = sections(body(a)), sections(body(b))
    print(f"\n######## {a} -> {b}")
    print("section sizes (chars):")
    for k in sorted(set(sa) | set(sb), key=int):
        d = sb.get(k, 0) - sa.get(k, 0)
        mark = "" if d == 0 else f" ({d:+d})"
        print(f"  §{k}: {sa.get(k, 0)} -> {sb.get(k, 0)}{mark}")
    items = lost(a, b)
    print(f"sentences without match >=0.6: {len(items)}")
    for best, sec, raw in items:
        print(f"[{best:.2f}] §{sec[:50]} | {raw[:500]}")
