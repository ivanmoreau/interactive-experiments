#!/usr/bin/env python3
"""Reject app bundles whose native binaries still depend on a build machine."""
import pathlib
import subprocess
import sys


def check(bundle):
    success = True
    binaries = list((bundle / "Contents/MacOS").iterdir())
    binaries += list((bundle / "Contents/Frameworks").glob("*.dylib"))
    for binary in binaries:
        result = subprocess.run(["otool", "-L", str(binary)], capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            if not line.startswith("\t"):
                continue
            dependency = line.strip().rsplit(" (", 1)[0]
            if dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if dependency.startswith("@rpath/"):
                name = dependency.removeprefix("@rpath/")
                if (bundle / "Contents/Frameworks" / name).is_file():
                    continue
            print(f"FAIL: {binary.name} has an unbundled dependency: {dependency}", file=sys.stderr)
            success = False
        load_commands = subprocess.check_output(["otool", "-l", str(binary)], text=True)
        if any(prefix in load_commands for prefix in ["/nix/store/", "/opt/homebrew/", "/usr/local/"]):
            print(f"FAIL: {binary.name} still contains a build dependency path", file=sys.stderr)
            success = False
    if success:
        print(f"PASS: {bundle.name} uses bundled libraries and macOS system libraries.")
    return success


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("Usage: check-bundle-dependencies.py APP_BUNDLE [...]")
    results = [check(pathlib.Path(path)) for path in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)
