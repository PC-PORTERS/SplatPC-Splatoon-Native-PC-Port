#!/usr/bin/env python3
"""Update-combinator: merge a base v16 dump and a newer update into one ROM folder.

The base dump is copied first, then the update overlays it. The result is a normal
Wii U folder structure (code/, content/, meta/) that Setup.bat and setup.sh can use
as their game dump. Original dumps are never modified.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
import tempfile
from pathlib import Path

REQUIRED_FOLDERS = ("code", "content", "meta")


class CombinatorError(RuntimeError):
    pass


def _within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def validate_dump(path: Path, label: str) -> None:
    if not path.is_dir():
        raise CombinatorError(f"{label} folder not found: {path}")
    for name in REQUIRED_FOLDERS:
        if not (path / name).is_dir():
            raise CombinatorError(f"{label} is not a game dump: missing '{name}' in {path}")
    for relative in ("code/Gambit.rpx", "meta/meta.xml"):
        if not (path / relative).is_file():
            raise CombinatorError(f"{label} is missing {relative}: {path}")


def title_version(dump: Path) -> str | None:
    try:
        text = (dump / "meta" / "meta.xml").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r"<title_version>\s*([^<\s]+)\s*</title_version>", text)
    return match.group(1) if match else None


def _copy_entry(source: Path, destination: Path) -> None:
    if source.is_symlink():
        _remove_path(destination)
        os.symlink(os.readlink(source), destination)
    elif source.is_dir():
        if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
            _remove_path(destination)
        shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=True)
    else:
        if destination.is_dir() and not destination.is_symlink():
            shutil.rmtree(destination)
        elif destination.is_symlink() or destination.exists():
            destination.unlink()
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def merge_tree(source: Path, destination: Path) -> None:
    for entry in sorted(source.iterdir(), key=lambda item: item.name):
        _copy_entry(entry, destination / entry.name)


def _remove_path(output: Path) -> None:
    if output.is_symlink() or output.is_file():
        output.unlink()
    elif output.exists():
        shutil.rmtree(output)


def combine(base: Path, update: Path, output: Path, force: bool = False) -> Path:
    base = base.resolve()
    update = update.resolve()
    output = output.resolve()
    validate_dump(base, "Base dump")
    validate_dump(update, "Update dump")
    if base == update:
        raise CombinatorError("The base and update dumps are the same folder.")
    for label, source in (("base", base), ("update", update)):
        if output == source or _within(output, source):
            raise CombinatorError(f"The output folder cannot be inside the {label} dump: {output}")
    if output.exists():
        if not output.is_dir() or output.is_symlink():
            raise CombinatorError(f"The output path exists and is not a folder: {output}")
        if any(output.iterdir()) and not force:
            raise CombinatorError(f"The output folder is not empty: {output} (use --force to replace it).")

    output.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        merge_tree(base, staging)
        merge_tree(update, staging)
        if output.exists():
            _remove_path(output)
        staging.rename(output)
    except Exception:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return output


def _dump_argument(value: str) -> Path:
    return Path(value).expanduser()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="Update-combinator",
        description="Merge a base v16 dump and a future update dump into one ROM folder.",
    )
    parser.add_argument("--base", required=True, type=_dump_argument, help="base v16 game dump (code/, content/, meta/)")
    parser.add_argument("--update", required=True, type=_dump_argument, help="newer update dump (code/, content/, meta/)")
    parser.add_argument("--output", required=True, type=_dump_argument, help="new ROM folder to create")
    parser.add_argument("--force", action="store_true", help="replace the output folder if it already contains files")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        result = combine(args.base, args.update, args.output, args.force)
    except CombinatorError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    version = title_version(result)
    suffix = f" (title version {version})" if version else ""
    print(f"Merged ROM ready: {result}{suffix}")
    print("Select this folder in Setup.bat or setup.sh. Original dumps were not changed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
