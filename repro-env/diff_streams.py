import re, sys
pat = re.compile(r"date\{([0-9a-f]+); ([0-9a-f]+); ([0-9a-f]+)\}; ([0-9a-f]+); ([0-9a-f]+); ([0-9a-f]+); (.*)")
def load(path):
    d = {}   # (date, frame) -> last checksum
    order = []
    with open(path, errors="replace") as f:
        for ln in f:
            m = pat.match(ln.strip())
            if not m: continue
            date, _, _, frame, _, csum, rest = m.groups()
            k = (date, frame)
            if k not in d: order.append(k)
            d[k] = (csum, rest)
    return d, order
s, sorder = load(sys.argv[1])
c, _ = load(sys.argv[2])
print(f"server frames={len(sorder)} client frames={len(c)}")
cset = set(c)
start = next(i for i,k in enumerate(sorder) if k in cset)
print(f"client starts at server frame index {start}")
for k in sorder[start:]:
    if k not in c:
        print(f"first unmatched (client missing) frame {k}"); break
    if s[k][0] != c[k][0]:
        print(f"FIRST CHECKSUM DIVERGENCE at date={k[0]} frame={k[1]}")
        print("  server:", s[k][1][:150])
        print("  client:", c[k][1][:150])
        break
