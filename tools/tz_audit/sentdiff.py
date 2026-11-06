import re, sys, pathlib, difflib
root = pathlib.Path(sys.argv[1])
vers = ["0.1","0.2","0.3","0.4","0.5","0.6","0.7","0.8","0.9","1.0","1.1"]
def body(v):
    t=(root/f"TZ_catalog_3D_2D_draft_v{v}.md").read_text(encoding="utf-8")
    t=t.split("## 18. История изменений")[0]
    return t
def sents(t):
    out=[]; sec=""; tag=""
    for line in t.splitlines():
        s=line.strip()
        if s.startswith("#"): sec=s.lstrip("# "); continue
        if not s: continue
        m=re.search(r"\[(СОГЛАСОВАНО[^\]]*|ПРЕДЛОЖЕНИЕ|ОТКРЫТО|СТАТУС)\]", s)
        tag = m.group(1) if m else tag
        for x in re.split(r"(?<=[.;:!?])\s+(?=[А-ЯA-Z*«(])", s):
            n=re.sub(r"\*\*|\[[^\]]*\]|`|^\s*(\d+\.|-|\|)\s*","",x).lower()
            n=re.sub(r"\s+"," ",n).strip()
            if len(n)>30: out.append((sec,tag,x,n,set(re.findall(r"[a-zа-яё0-9]{4,}",n))))
    return out
S={v:sents(body(v)) for v in vers}
def stem(w): return w[:6]
for a,b in zip(vers,vers[1:]):
    pool=S[b]; bstems=[set(map(stem,p[4])) for p in pool]
    print(f"\n######## {a} -> {b}")
    for sec,tag,raw,n,t in S[a]:
        if not tag.startswith("СОГЛАСОВАНО"): continue
        ts=set(map(stem,t))
        if not ts: continue
        best=max(len(ts&bs)/len(ts) for bs in bstems)
        if best<0.6:
            print(f"[{best:.2f}] §{sec[:40]} | {raw[:400]}")
