"""Game integrity: which release this install is, proved by a hash of its code (Python 3, standard library only).

The release lists the files that make up the game's code in scripts\\integrity-manifest.txt (one
"<sha256> <path>" per line, made by `integrity.py --make` before a version is tagged). The player's PC
hashes those files as they are on disk (line endings ignored, so a Git checkout and a ZIP agree) and
sends the result with its sign-in; the server compares it with the hashes of the releases it accepts.

    py -3 scripts\\integrity.py                 print this install's version and code hash (JSON)
    py -3 scripts\\integrity.py --make          rewrite scripts\\integrity-manifest.txt from the files Git tracks
    py -3 scripts\\integrity.py --check         say which listed files differ from the manifest (exit 1 when any)

The server-side value of a release is `release_hash` = sha256 of the manifest text, printed by --make.
This makes casual changes visible and refuses old or unofficial versions. It cannot stop someone who
reads this file and sends the right hash: it is a speed bump, not a lock (moderation does the rest).
"""
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "scripts", "integrity-manifest.txt")
COVERED_DIRS = ("recompiler", "profiles", "Manifest", "scripts", os.path.join("tools", "boss"))
COVERED_FILES = ("VERSION", "Play-Online.bat", "setup.sh", "Setup.bat")
SKIPPED = ("scripts/tests/", "scripts/integrity-manifest.txt", "scripts/server.txt")


def hash_file(path):
    """sha256 of a file with CR bytes removed: autocrlf checkouts and ZIPs hash the same."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block.replace(b"\r", b""))
    return digest.hexdigest()


def covered(relative):
    relative = relative.replace("\\", "/")
    if relative.startswith(SKIPPED) or relative.endswith(".pyc") or "/__pycache__/" in relative:
        return False
    return relative in COVERED_FILES or relative.startswith(tuple(d.replace("\\", "/") + "/" for d in COVERED_DIRS))


def tracked_files():
    out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    return sorted(p for p in out.decode("utf-8").split("\0") if p and covered(p))


def manifest_text(paths):
    return "".join(f"{hash_file(os.path.join(ROOT, p))} {p}\n" for p in paths)


def read_manifest(path=MANIFEST):
    """[(sha256, relative path)] from the manifest, or None when the install has none."""
    try:
        with open(path, encoding="utf-8", newline="") as handle:
            lines = handle.read().replace("\r", "").split("\n")
    except OSError:
        return None
    entries = []
    for line in lines:
        digest, _, name = line.partition(" ")
        if len(digest) == 64 and name:
            entries.append((digest, name))
    return entries


def release_hash(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def current_text(entries, root=ROOT):
    """The manifest text this PC's files would give. A missing file hashes to zeros, so the result differs."""
    lines = []
    for _, name in entries:
        try:
            digest = hash_file(os.path.join(root, name))
        except OSError:
            digest = "0" * 64
        lines.append(f"{digest} {name}\n")
    return "".join(lines)


def read_version(root=ROOT):
    for relative in (os.path.join("build", "installed-version.txt"), "VERSION"):
        try:
            with open(os.path.join(root, relative), encoding="ascii", errors="replace") as handle:
                text = handle.read().strip().lstrip("v")
        except OSError:
            continue
        if text and all(c.isdigit() or c == "." for c in text):
            return text
    return ""


def attestation(root=ROOT):
    """{'version', 'code'} to send with a sign-in; 'code' is empty when the install has no manifest."""
    entries = read_manifest(os.path.join(root, "scripts", "integrity-manifest.txt"))
    return {"version": read_version(root), "code": release_hash(current_text(entries, root)) if entries else ""}


def differing(root=ROOT):
    entries = read_manifest(os.path.join(root, "scripts", "integrity-manifest.txt")) or []
    mine = dict((name, digest) for digest, name in (line.split(" ", 1) for line in current_text(entries, root).splitlines()))
    return [name for digest, name in entries if mine.get(name) != digest]


def main(argv):
    if "--make" in argv:
        text = manifest_text(tracked_files())
        with open(MANIFEST, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print(json.dumps({"files": text.count("\n"), "release_hash": release_hash(text), "version": read_version()}))
        return 0
    if "--check" in argv:
        bad = differing()
        for name in bad:
            print(name)
        return 1 if bad else 0
    print(json.dumps(attestation()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
