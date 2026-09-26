#!/usr/bin/env python3
"""Run the PRG32 CLI with the 64 KiB portable cartridge RAM window.

PRG32 firmware defaults (ESP32-C6 and QEMU) use the "extended" 64 KiB
cartridge RAM profile since commit 596bcf9, but the host builder's portable
fallback still checks against the 32 KiB classroom profile. This adapter only
raises that host-side check; it changes no firmware or package-format
behaviour. Remove it once the upstream fallback is updated.

Usage: PRG32_REPO=/path/to/PRG32 python3 tools/prg32_cli_64.py <prg32 args>
"""

import importlib
import os
import sys
from pathlib import Path

repo = Path(os.environ.get("PRG32_REPO", Path(__file__).resolve().parents[2] / "PRG32"))
sys.path.insert(0, str(repo.resolve()))

from prg32.utilities import env_variables  # noqa: E402

RAM_SIZE = 64 * 1024
env_variables.FALLBACK_CART_RAM_SIZE = RAM_SIZE
importlib.import_module("prg32.cartridge.build_cartridge").FALLBACK_CART_RAM_SIZE = RAM_SIZE
importlib.import_module("prg32.utilities.runtime_handler").FALLBACK_CART_RAM_SIZE = RAM_SIZE

from prg32 import prg32  # noqa: E402

prg32.main(sys.argv[1:])
