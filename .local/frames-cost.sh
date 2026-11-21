#!/bin/sh
# Inside the profiler image: what a screen frame costs with the MPC's cache sizes, snapshot and drawing apart.
# Usage: frames-cost.sh [PAGE [TRACK [PLAY]]]. Counts 200 frames and 100, so that start-up cancels out.
cd /work
PAGE=${1:-0}; TRACK=${2:-3}; PLAY=${3:-1}
cg() { valgrind --tool=callgrind --cache-sim=yes --I1=32768,4,64 --D1=32768,4,64 --LL=524288,16,64 --callgrind-out-file=/dev/null \
       build/melodee --melodee-frames $1 $PAGE $TRACK $PLAY $2 2>&1 | grep -E "I +refs|D1 +misses|LL misses" | sed -e "s/==[0-9]*== //" -e "s/(.*//" | tr -d ", " | cut -d: -f2 | tr "\n" " "; }
for parts in 3 2 1; do echo "$parts $(cg 200 $parts) $(cg 100 $parts)"; done | awk '
{ ir = ($2 - $5) / 100; d1 = ($3 - $6) / 100; ll = ($4 - $7) / 100;
  name = $1 == 3 ? "snapshot + draw" : $1 == 2 ? "draw only      " : "snapshot only  ";
  printf "%s per frame: %9.0f instructions, %6.0f L1 misses, %6.0f last-level misses, about %9.0f cycles\n", name, ir, d1, ll, ir + 10 * d1 + 100 * ll }'
