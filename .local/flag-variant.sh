#!/bin/sh
# Rebuild Melodee's own objects (not JUCE's) with the given ARCH_FLAGS and MATH_FLAGS, and print the DSP bench's hash.
cd /work
rm -rf build/obj/melodee build/obj/base build/obj/main.o
make -j10 ARCH_FLAGS="$1" MATH_FLAGS="$2" > .local/make-variant.log 2>&1 || { echo "make failed"; tail -5 .local/make-variant.log; exit 1; }
for n in 1 2; do build/melodee --melodee-dsp-bench seconds=6 | tr " " "\n" | grep -E "guard|mean_us|heard" | tr "\n" " "; done
echo " <= arch='$1' math='$2'"
