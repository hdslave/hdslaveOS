#!/bin/sh
# Run a command in the profiler image (the build image plus valgrind), the repository at /work.
cd "$(dirname "$0")/.."
exec docker --context colima-melodee run --rm --platform linux/arm64 --ulimit stack=268435456:268435456 \
    -v "$PWD:/work" -e MELODEE_VERSION melodee-profiler sh -c "$*"
