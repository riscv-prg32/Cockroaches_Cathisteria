#!/usr/bin/env python3
"""Validate a Cartridge Store bundle the way the store will ingest it.

The Cartridge Store rebuilds every cartridge from the bundle (manifest
metadata + icon + splash + colophon) and serves that rebuilt image to boards,
so the rebuilt image must still fit the 64 KiB cartridge limit. This script
reuses CartridgeStore's own format module when the repository is available
(CARTRIDGE_STORE_REPO, default ../CartridgeStore) and otherwise performs
basic structural checks only.
"""

from __future__ import annotations

import json
import os
import sys
import zipfile
from pathlib import Path

LIMIT = 64 * 1024
ROOT = Path(__file__).resolve().parents[1]


def main(bundle: Path) -> int:
    store_repo = Path(os.environ.get("CARTRIDGE_STORE_REPO", ROOT.parent / "CartridgeStore"))
    fmt = None
    if (store_repo / "cartridge_store/prg32_format.py").exists():
        sys.path.insert(0, str(store_repo))
        from cartridge_store import prg32_format as fmt  # type: ignore
    with zipfile.ZipFile(bundle) as zf:
        manifest = json.loads(zf.read("manifest.json"))
        icon = zf.read(manifest["assets"]["icon"])
        splash = zf.read(manifest["assets"]["splash"])
        metadata = {k: v for k, v in manifest.items()
                    if k not in ("assets", "architectures", "colophon")}
        ok = True
        for arch in manifest["architectures"]:
            data = zf.read(arch["file"])
            if fmt is None:
                size = len(data)
                note = "structural check only"
            else:
                fmt.validate_metadata(manifest)
                image = fmt.build_cartridge(data, metadata=metadata, icon=icon,
                                            screenshot=splash,
                                            colophon=manifest.get("colophon"),
                                            architecture=arch["id"])
                parsed = fmt.parse_cartridge(image)
                assert parsed.metadata["id"] == manifest["id"]
                assert parsed.metadata["runtime"]["architecture"] == arch["id"]
                size = len(image)
                note = "rebuilt by CartridgeStore format module"
            fits = size <= LIMIT
            ok &= fits
            print(f"{arch['id']:8s} {arch['file']:34s} {size:6d} bytes "
                  f"({LIMIT - size:+d} free) {'OK' if fits else 'TOO LARGE'} [{note}]")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1])))
