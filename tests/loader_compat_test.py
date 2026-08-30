from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import check_loader_compat  # noqa: E402


class LoaderCompatTest(unittest.TestCase):
    def test_detects_aarch64_tlsdesc(self) -> None:
        output = """
000000000017f708  0000000000000407 R_AARCH64_TLSDESC 0
000000000017f750  0000000000000402 R_AARCH64_JUMP_SLOT 0
"""
        self.assertEqual(
            {"R_AARCH64_TLSDESC"},
            check_loader_compat.forbidden_relocations(output),
        )

    def test_plain_relocations_are_allowed(self) -> None:
        output = """
000000000017f750  0000000000000402 R_AARCH64_JUMP_SLOT 0
000000000017f758  0000000000000401 R_AARCH64_GLOB_DAT 0
"""
        self.assertEqual(set(), check_loader_compat.forbidden_relocations(output))


if __name__ == "__main__":
    unittest.main()
