import re, sys, difflib, pathlib, collections

root = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
vers = ["0.1","0.2","0.3","0.4","0.5","0.6","0.7","0.8","0.9","1.0","1.1"]
docs = {v: (root / f"TZ_catalog_3D_2D_draft_v{v}.md").read_text(encoding="utf-8") for v in vers}

def units(text):
    res = []
    sec = ""
    for line in text.splitlines():
        s = line.strip()
        if not s or s.startswith("---") or s.startswith("|---"):
            continue
        if s.startswith("#"):
            sec = s.lstrip("#").strip()
            continue
        if sec.startswith("18.") or "История изменений" in sec:
            continue
        res.append((sec, s))
    return res

def norm(s):
    s = re.sub(r"\*\*|\[[А-ЯA-Z ,]+\]|`", "", s)
    s = re.sub(r"^\s*(\d+\.|-|\|)\s*", "", s)
    s = re.sub(r"\s+", " ", s).lower()
    return s.strip()

def toks(s):
    return set(re.findall(r"[a-zа-яё0-9]{4,}", s))

U = {v: [(sec, raw, norm(raw), toks(norm(raw))) for sec, raw in units(docs[v])] for v in vers}

def best(u, pool):
    _, _, n, t = u
    cands = []
    for p in pool:
        if not t or not p[3]:
            continue
        j = len(t & p[3]) / len(t | p[3])
        cands.append((j, p))
    cands.sort(key=lambda x: -x[0])
    b = 0.0; bp = None
    for j, p in cands[:8]:
        r = difflib.SequenceMatcher(None, n, p[2]).ratio()
        sc = max(r, j)
        # containment: old text largely included in new
        cont = len(t & p[3]) / max(1, len(t))
        sc = max(sc, cont * 0.9)
        if sc > b:
            b, bp = sc, p
    return b, bp

lines = []
TH = 0.55
for a, b in zip(vers, vers[1:]):
    lines.append(f"\n\n######## {a} -> {b}\n")
    pool = U[b]
    for u in U[a]:
        sc, bp = best(u, pool)
        if sc < TH and len(u[2]) > 25:
            # also check whole new doc contains the token set
            lines.append(f"[{sc:.2f}] §{u[0]}\n   OLD: {u[1][:600]}\n   NEW≈ {(bp[1][:300] if bp else '-')}\n")

# Terms present in older versions but absent in final
final_text = norm(docs["1.1"])
final_toks = toks(final_text)
term_first = collections.OrderedDict()
for v in vers[:-1]:
    for w in toks(norm(docs[v])):
        if w not in final_toks and w not in term_first:
            term_first[w] = v
lines.append("\n\n######## TERMS present earlier, absent in v1.1\n")
by = collections.defaultdict(list)
for w, v in term_first.items():
    by[v].append(w)
for v in vers[:-1]:
    lines.append(f"{v}: {' '.join(sorted(by[v]))}\n")

out.write_text("".join(lines), encoding="utf-8")
print("written", out, sum(1 for l in lines if l.startswith('[')))
