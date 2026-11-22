#!/bin/sh
# Build tests/melodee.cpp as tests/test_melodee.py does, into build/test-melodee (inside the build container).
set -eu
cd /work
SOURCES=$(python3 - <<'PY'
import re,sys
sys.path.insert(0,'tests')
import test_melodee
print(' '.join(test_melodee.SOURCES))
PY
)
mkdir -p build/hosttest
c++ -std=c++17 ${OPT:--O2} ${EXTRA:-} -Wall -Wextra -Werror -Isrc -Icore/src -Ibase/src tests/melodee.cpp $SOURCES -o ${OUT:-build/hosttest/melodee}
