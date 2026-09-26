#!/usr/bin/env bash
# Build the cartridge natively against stubbed PRG32 calls and run the tests.
#   tests/run_tests.sh [soak-ticks-per-level]
#   tests/run_tests.sh shots      -> build/shots/*.png
#   tests/run_tests.sh cost       -> estimated ESP32-C6 frame cost per level
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prg32_repo="${PRG32_REPO:-"$repo_dir/../PRG32"}"
mkdir -p "$repo_dir/build"
cc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -I "$repo_dir/src" \
  -I "$prg32_repo/components/prg32/include" \
  -I "$prg32_repo/components/prg32_audio/include" \
  -I "$prg32_repo/main" \
  "$repo_dir/tests/test_game.c" -o "$repo_dir/build/test_game"
case "${1:-}" in
  shots)
    rm -rf "$repo_dir/build/shots" && mkdir -p "$repo_dir/build/shots"
    "$repo_dir/build/test_game" shots "$repo_dir/build/shots"
    python3 - "$repo_dir/build/shots" <<'PY'
import sys
from pathlib import Path
from PIL import Image
for ppm in sorted(Path(sys.argv[1]).glob("*.ppm")):
    Image.open(ppm).save(ppm.with_suffix(".png"))
    ppm.unlink()
print(f"screenshots in {sys.argv[1]}")
PY
    ;;
  cost) "$repo_dir/build/test_game" cost ;;
  *) "$repo_dir/build/test_game" "${1:-3000}" ;;
esac
