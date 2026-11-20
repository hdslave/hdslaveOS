# The build image plus a profiler, for counting the instructions the engine runs (callgrind). Not part of the build.
FROM melodee-builder
RUN apt-get update && apt-get install -y --no-install-recommends valgrind binutils && rm -rf /var/lib/apt/lists/*
