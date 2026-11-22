#!/bin/sh
# CLAP plug-ins as Melodee opens them (clap_check.cpp), after `make`. Run inside the build image with the plug-ins
# mounted, for example:
#   docker --context colima-melodee run --rm --platform linux/arm64 -v "$PWD:/work" -v "<plug-in folder>:/plugins:ro" \
#       melodee-builder sh /work/.local/checks/clap-check.sh
# With no arguments it checks every .clap under /plugins.
set -eu
cd /work
OUT=build/clap-check
mkdir -p "$OUT"
JUCE=build/deps/JUCE/modules
g++ -std=c++17 -O2 -pthread -Isrc -Icore/src -Ibase/src -I$JUCE -Ibuild/deps/clap/include -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 \
    -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG -DJUCE_PLUGINHOST_VST3=1 -DJUCE_PLUGINHOST_LV2=1 \
    .local/checks/clap_check.cpp build/obj/melodee/plugin/clap_instance.o build/obj/melodee/plugin/plugin_files.o \
    build/obj/melodee/plugin/plugin_support.o build/obj/plugin_support_pack.o build/obj/juce_core.o \
    build/obj/juce_core_CompilationTime.o build/obj/juce_core_zlib.o build/obj/juce_events.o build/obj/juce_audio_basics.o \
    build/obj/juce_audio_processors_headless.o build/obj/juce_audio_processors_headless_lv2_libs.o \
    -ldl -pthread -lrt -o "$OUT/clap_check"
if [ $# -gt 0 ]; then
    exec "$OUT/clap_check" "$@"
fi
find /plugins -name '*.clap' -type f | sort | tr '\n' '\0' | xargs -0 "$OUT/clap_check"
