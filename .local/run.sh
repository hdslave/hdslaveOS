#!/bin/sh
# Run a command in the Melodee build container (linux/arm64) with the repository at /work.
# The engine tests keep whole projects on the stack and need more than Docker's default 8 MB.
cd "$(dirname "$0")/.."
exec docker --context colima-melodee run --rm --platform linux/arm64 --ulimit stack=268435456:268435456 \
    -v "$PWD:/work" -e MELODEE_VERSION melodee-builder sh -c "$*"
