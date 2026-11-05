#!/usr/bin/env python3
"""
bundle_builder_assets.py

Packages the required source files (Manifest, recompiler, scripts, LICENSES, setup.sh, build.sh)
into Builder/embedded/assets.zip so that the standalone SplatPC-Builder executable
can function completely offline and standalone without needing an external repository clone.
"""

import os
import sys
import zipfile

IGNORED_DIRS = {"__pycache__", ".git", ".vscode", "build", "dist", "Game"}
IGNORED_EXTS = {".pyc", ".pyo", ".o", ".a", ".so", ".exe", ".dll", ".dylib", ".DS_Store"}
IGNORED_FILES = {"gambit_plt_stubs.cpp", "gambit_plt_auto.cpp"}


def should_include_file(path: str) -> bool:
    if os.path.basename(path) in IGNORED_FILES:
        return False
    parts = path.replace("\\", "/").split("/")
    for part in parts:
        if part in IGNORED_DIRS:
            return False
    _, ext = os.path.splitext(path)
    if ext in IGNORED_EXTS:
        return False
    return True


def bundle_assets(repo_root: str, output_zip: str):
    os.makedirs(os.path.dirname(os.path.abspath(output_zip)), exist_ok=True)
    count = 0
    total_uncompressed = 0

    with zipfile.ZipFile(output_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for folder in ["Manifest", "recompiler", "scripts", "LICENSES"]:
            folder_path = os.path.join(repo_root, folder)
            if not os.path.exists(folder_path):
                continue
            for root, dirs, files in os.walk(folder_path):
                dirs[:] = [d for d in dirs if d not in IGNORED_DIRS]
                for file in files:
                    file_path = os.path.join(root, file)
                    rel_path = os.path.relpath(file_path, repo_root).replace("\\", "/")
                    if should_include_file(rel_path):
                        zf.write(file_path, rel_path)
                        count += 1
                        total_uncompressed += os.path.getsize(file_path)

        for single_file in ["setup.sh", "build.sh", "CMakeLists.txt", "Setup.bat"]:
            file_path = os.path.join(repo_root, single_file)
            if os.path.exists(file_path):
                rel_path = single_file
                zf.write(file_path, rel_path)
                count += 1
                total_uncompressed += os.path.getsize(file_path)

    zip_size = os.path.getsize(output_zip)
    print(f"Bundled {count} files ({total_uncompressed / (1024*1024):.2f} MB uncompressed) into {output_zip} ({zip_size / (1024*1024):.2f} MB compressed)")


if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, ".."))
    output_zip = os.path.join(repo_root, "Builder", "embedded", "assets.zip")
    bundle_assets(repo_root, output_zip)
