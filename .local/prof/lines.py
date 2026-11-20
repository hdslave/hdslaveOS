#!/usr/bin/env python3
"""Per-line instruction counts from a callgrind_annotate --auto=yes listing: lines.py LISTING SOURCE FROM TO [MIN%]"""
import re, sys
listing, source, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
low = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0
lines = open(listing, errors='replace').read().split('\n')
name = source.split('/')[-1]
start = next(i for i, l in enumerate(lines) if re.match(r'-- (Auto|User)-annotated source:', l) and l.rstrip().endswith(source if '/' in source else name))
cost, cur = {}, None
for l in lines[start + 1:]:
    if re.match(r'-- (Auto|User)-annotated source:', l): break
    m = re.match(r'-- line (\d+) -+', l)
    if m: cur = int(m.group(1)); continue
    if cur is None: continue
    m = re.match(r'\s*([\d,]+) \(\s*([\d.]+)%\)', l)
    if m: cost[cur] = (int(m.group(1).replace(',', '')), float(m.group(2)))
    elif not re.match(r'\s*\.', l): continue
    cur += 1
src = open(source).read().split('\n') if '/' in source else None
print(f"{name}: {sum(c for c, _ in cost.values()):,} Ir in all; lines {a}-{b}: {sum(c for n, (c, _) in cost.items() if a <= n <= b):,}")
for i in range(a, b + 1):
    c = cost.get(i)
    if c and c[1] < low: c = None
    if low and not c: continue
    print(f"{i:5} {('%5.2f%%' % c[1]) if c else '      '} {src[i - 1][:160] if src else ''}")
