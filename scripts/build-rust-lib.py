#!/usr/bin/env python3
"""Build a cargo staticlib and place it where meson expects it."""

import os
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: build-rust-lib.py CARGO MANIFEST TARGET_DIR OUTPUT")
    cargo, manifest, target_dir, output = sys.argv[1:5]

    env = dict(os.environ)
    env.pop("RUSTFLAGS", None)

    result = subprocess.run(
        [cargo, "build", "--release",
         "--manifest-path", manifest,
         "--target-dir", target_dir],
        env=env,
    )
    if result.returncode != 0:
        sys.exit(result.returncode)

    built = os.path.join(target_dir, "release", os.path.basename(output))
    if not os.path.exists(built):
        sys.exit(f"cargo did not produce {built}")
    shutil.copyfile(built, output)


if __name__ == "__main__":
    main()
