#!/usr/bin/env bash
# Starts the game on Linux: the sign-in screen (pseudo, language), then the online game.
# Same thing Play-Online.bat does on Windows. Arguments go to scripts/splatoon_login.py
# (for example: ./Play-Online.sh --cli --pseudo Ana, or --offline).
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [ ! -x Game/Splatoon ] && [ ! -x Game/Splatoon-v288 ]; then
    echo "The game is not installed yet: run ./setup.sh first." >&2
    echo "Le jeu n'est pas encore installé : lance d'abord ./setup.sh." >&2
    exit 1
fi
for python in python3 python; do
    if command -v "$python" >/dev/null 2>&1 &&
       "$python" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; then
        exec "$python" scripts/splatoon_login.py "$@"
    fi
done
echo "Python 3.10 or newer is required (for example: sudo apt install python3 python3-tk)." >&2
exit 1
