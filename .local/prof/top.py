#!/usr/bin/env python3
"""The costliest source lines of one file in a callgrind_annotate --auto=yes listing: top.py LISTING FILE-SUFFIX [COUNT]"""
import re, sys
listing, suffix = sys.argv[1], sys.argv[2]
count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
L = open(listing, errors='replace').read().split('\n')
rows, inside, line = [], False, None
for l in L:
    m = re.match(r'-- (?:Auto|User)-annotated source: (.*)', l)
    if m: inside = m.group(1).strip().endswith(suffix); line = None; continue
    if not inside: continue
    m = re.match(r'-- line (\d+) -+', l)
    if m: line = int(m.group(1)); continue
    if line is None: continue
    if '=>' in l: continue  # a call's inclusive cost, not this line's own
    m = re.match(r'\s*([\d,]+) \(\s*([\d.]+)%\)\s?(.*)$', l)
    if m: rows.append((int(m.group(1).replace(',', '')), float(m.group(2)), line, m.group(3)))
    if m or re.match(r'\s+\.(\s|$)', l): line += 1
print(f"{suffix}: {sum(r[0] for r in rows):,} Ir on its own lines")
for ir, pc, n, text in sorted(rows, reverse=True)[:count]:
    print(f"{pc:5.2f}% {n:5}  {text.strip()[:150]}")
