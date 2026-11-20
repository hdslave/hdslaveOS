#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""A fork's SD card package without the update signature: python3 tools/package_unsigned.py VERSION

tools/make_release.py signs a release's file list with the key whose public half is in
src/melodee/shell/update.h, so that an installed Melodee takes updates only from its author. A fork has no such
key. This writes the same autorun folder (the same files, the same SHA256SUMS that sd-worker.sh checks at boot,
the same autorun.check) as dist/Melodee-MPC-Sample-<version>-unsigned.zip, without RELEASE.sha256.sig.

Such a package installs by hand only: unzip it and copy its autorun folder to the card (or into a boot menu's
folder for Melodee). Melodee's own updater, on the MPC or on its web page, will not take it, and a Melodee built
from this fork will not take the author's releases as updates either until update.h holds a key of the fork's.

Run after ./build.sh and the strip that release.sh does (build/melodee.release).
"""
import argparse
import hashlib
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import make_release  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", help="such as v0.5.1-fork.1")
    parser.add_argument("--binary", default=str(ROOT / "build/melodee.release"), help="stripped build to ship")
    arguments = parser.parse_args()
    if not make_release.VERSION.fullmatch(arguments.version):
        raise SystemExit("Version must look like v0.1.0 or v0.1.0-alpha.1")
    commit = make_release.git("rev-parse", "--short=12", "HEAD")
    if make_release.git("status", "--porcelain"):
        commit += "+"  # the tree has changes that are in no commit
    files = make_release.collect(Path(arguments.binary), arguments.version, commit)
    for name, data in files.items():
        make_release.scan(name, data)
    files[make_release.LIST] = make_release.release_list(files)
    output = ROOT / "dist" / f"Melodee-MPC-Sample-{arguments.version}-unsigned.zip"
    make_release.write_zip(files, output, int(make_release.git("log", "-1", "--format=%ct")))
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + ".sha256").write_text(f"{digest}  {output.name}\n")
    print(f"Wrote {output} ({output.stat().st_size // 1024} KiB) at {commit}, unsigned")
    print(f"SHA256: {digest}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, OSError) as error:
        raise SystemExit(str(error))
