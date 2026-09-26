#!/usr/bin/env bash
# Pack the Cartridge Store bundle from the built cartridges.
#
#   scripts/build.sh && scripts/pack-store-bundle.sh
#
# The manifest is generated from metadata/metadata.json and
# metadata/colophon.json so there is a single source of truth. The bundle is
# then validated with the Cartridge Store's own image builder when the
# CartridgeStore repository is available (CARTRIDGE_STORE_REPO).
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prg32_repo="$(cd "${PRG32_REPO:-"$repo_dir/../PRG32"}" && pwd)"
name="cockroaches"
stage="$repo_dir/dist/store-bundle"
bundle="$repo_dir/dist/$name-store-bundle.zip"

rm -rf "$stage"
mkdir -p "$stage"
cp "$repo_dir/assets/icon.png" "$stage/icon.png"
cp "$repo_dir/assets/screenshot.png" "$stage/screenshot.png"
for arch in esp32c6 qemu; do
  cart="$repo_dir/dist/$name-$arch.prg32"
  [[ -f "$cart" ]] || { echo "missing $cart; run scripts/build.sh first" >&2; exit 1; }
  cp "$cart" "$stage/"
done

python3 - "$repo_dir" "$stage/manifest.json" <<'PY'
import json, sys
from pathlib import Path
root, out = Path(sys.argv[1]), Path(sys.argv[2])
manifest = json.loads((root / "metadata/metadata.json").read_text())
manifest.pop("runtime", None)  # the store derives runtime per architecture
manifest["colophon"] = json.loads((root / "metadata/colophon.json").read_text())
manifest["assets"] = {"icon": "icon.png", "splash": "screenshot.png"}
manifest["architectures"] = [
    {"id": "esp32c6", "file": "cockroaches-esp32c6.prg32"},
    {"id": "qemu", "file": "cockroaches-qemu.prg32"},
]
out.write_text(json.dumps(manifest, indent=2) + "\n")
PY

rm -f "$bundle"
(cd "$prg32_repo" && python3 -m prg32 store pack-bundle \
  --manifest "$stage/manifest.json" --out "$bundle")

python3 "$repo_dir/tools/validate_bundle.py" "$bundle"
echo "$bundle"
