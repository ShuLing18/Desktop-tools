"""Fetch the pinned, hash-verified C/C++ toolchain into this workspace only."""
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = "0.13.0"
DEST = ROOT / ".tools"
COMPILER = DEST / f"zig-windows-x86_64-{VERSION}" / "zig.exe"
EXPECTED_SHA256 = "d859994725ef9402381e557c60bb57497215682e355204d754ee3df75ee3c158"


def main():
    if COMPILER.is_file():
        print(COMPILER)
        return
    DEST.mkdir(exist_ok=True)
    with urllib.request.urlopen("https://ziglang.org/download/index.json", timeout=30) as response:
        info = json.load(response)[VERSION]["x86_64-windows"]
    if info["shasum"] != EXPECTED_SHA256:
        raise RuntimeError("Official compiler checksum differs from the pinned checksum")
    archive = DEST / f"zig-{VERSION}.zip"
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != EXPECTED_SHA256:
        print("Downloading the C/C++ compiler from ziglang.org...", flush=True)
        with urllib.request.urlopen(info["tarball"], timeout=120) as response, archive.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != EXPECTED_SHA256:
        raise RuntimeError("Compiler archive checksum verification failed")
    print("Checksum verified. Extracting...", flush=True)
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            target = (DEST / member.filename).resolve()
            if not target.is_relative_to(DEST.resolve()):
                raise RuntimeError("Unsafe archive path")
        package.extractall(DEST)
    print(COMPILER)


if __name__ == "__main__":
    main()

