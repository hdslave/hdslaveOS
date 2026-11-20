#!/bin/sh
# Compile the files touched so far, with the build's own flags, without linking (inside the build container).
set -eu
cd /work
FLAGS="-Isrc -Ibase/src -Ibuild/deps/JUCE/modules $(pkg-config --cflags freetype2 fontconfig alsa libdrm libsystemd) -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_STANDALONE_APPLICATION=1 -DJUCE_USE_CURL=0 -DJUCE_WEB_BROWSER=0 -DNDEBUG -DJUCE_PLUGINHOST_VST3=1 -DJUCE_PLUGINHOST_LV2=1 -Ibuild/deps/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK -Ibuild/deps/clap/include -std=c++17 -O1 -pthread -Wall -Wextra -Werror -fsyntax-only"
for f in "$@"; do g++ $FLAGS "$f" && echo "ok $f"; done
