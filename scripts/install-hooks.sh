#!/usr/bin/env bash
# Installs git hooks for the repository
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Set core.hooksPath so git uses scripts/git-hooks directly
git config core.hooksPath scripts/git-hooks

# Also copy into .git/hooks as fallback
if [ -d ".git/hooks" ]; then
    cp scripts/git-hooks/pre-commit .git/hooks/pre-commit
    chmod +x .git/hooks/pre-commit
fi

echo "Git pre-commit hook successfully configured."
