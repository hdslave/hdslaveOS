#!/bin/sh
# Inside the profiler image: build, then the engine's output hashes (10 s playing and stopped) and the
# instructions a 3 s run takes, playing and stopped. The reference hashes are the unmodified engine's.
cd /work
make -j10 MATH_FLAGS="-fno-trapping-math -g" > .local/p2-make-step.log 2>&1 || { echo "BUILD FAILED"; grep -n "error" .local/p2-make-step.log | head -20 | cut -c1-300; exit 1; }
h() { build/melodee --melodee-dsp-bench seconds=10 threads=0 pace=0 guard=0 $1 | grep -o "heard=[0-9a-f]*" | cut -d= -f2; }
ir() { valgrind --tool=callgrind --callgrind-out-file=${2:-/dev/null} build/melodee --melodee-dsp-bench seconds=3 threads=0 pace=0 guard=0 $1 2>&1 | grep -o "refs: *[0-9,]*" | tr -d "refs: ,"; }
P=$(h); S=$(h play=0)
[ "$P" = 1620747a68067d5f ] && echo "playing: SAME ($P)" || echo "playing: DIFFERENT ($P)"
[ "$S" = d232e1298a12cb83 ] && echo "stopped: SAME ($S)" || echo "stopped: $S (reference d232e1298a12cb83)"
echo "Ir playing $(ir "" .local/prof/play-now.out)  (baseline 3011417144)"
echo "Ir stopped $(ir play=0 .local/prof/stop-now.out)  (baseline 1397723477)"
