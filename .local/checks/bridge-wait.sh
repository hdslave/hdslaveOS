#!/bin/sh
# The bridge's handoff, measured where no armv7 code runs: vst2_bridge32 and a fixture plug-in are built for this
# machine (aarch64) and driven by Melodee's own Vst2BridgeInstance (bridge_wait.cpp), after `make`.
# Run inside the build image; for real-time priority give the container SYS_NICE:
#   docker --context colima-melodee run --rm --platform linux/arm64 --cap-add SYS_NICE --ulimit rtprio=99 \
#       -v "$PWD:/work" melodee-builder sh /work/.local/checks/bridge-wait.sh [BLOCKS] [WORK_US ...]
set -eu
cd /work
OUT=build/bridge-wait
BLOCKS=${1:-2000}
shift 2>/dev/null || true
mkdir -p "$OUT/bridge/armhf"
JUCE=build/deps/JUCE/modules
FLAGS="-std=c++17 -O2 -pthread -Isrc -I$JUCE -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 \
    -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG"
g++ -std=c++17 -O2 -Isrc src/melodee/plugin/bridge32/vst2_bridge32.cpp -ldl -o "$OUT/bridge/vst2_bridge32"
ln -sf /lib/ld-linux-aarch64.so.1 "$OUT/bridge/armhf/ld-linux-armhf.so.3"
g++ -std=c++17 -O2 -Isrc -shared -fPIC .local/checks/bridge_fixture.cpp -o "$OUT/bridge_fixture.so"
# shellcheck disable=SC2086
g++ $FLAGS .local/checks/bridge_wait.cpp build/obj/melodee/plugin/vst2_bridge_instance.o build/obj/juce_core.o \
    build/obj/juce_core_CompilationTime.o build/obj/juce_core_zlib.o build/obj/juce_events.o build/obj/juce_audio_basics.o \
    build/obj/juce_audio_processors_headless.o build/obj/juce_audio_processors_headless_lv2_libs.o \
    -ldl -pthread -lrt -o "$OUT/bridge_wait"
for work in ${*:-0 400}; do
    BRIDGE_FIXTURE_WORK_US=$work "$OUT/bridge_wait" "$PWD/$OUT/bridge_fixture.so" "$PWD/$OUT/bridge" "$BLOCKS"
done
