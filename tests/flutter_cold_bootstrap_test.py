from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import flutter_cold_bootstrap  # noqa: E402


class FlutterColdBootstrapModeTest(unittest.TestCase):
    def test_release_and_profile_use_distinct_apk_outputs(self) -> None:
        release = flutter_cold_bootstrap.flutter_fixture_apk_path("release")
        profile = flutter_cold_bootstrap.flutter_fixture_apk_path("profile")
        self.assertEqual("app-release.apk", release.name)
        self.assertEqual("app-profile.apk", profile.name)
        self.assertNotEqual(release, profile)

    def test_release_and_profile_use_matching_engine_gen_snapshot(self) -> None:
        flutter = "/opt/flutter/3.22.3/bin/flutter"
        release = flutter_cold_bootstrap.flutter_gen_snapshot_path(flutter, "release")
        profile = flutter_cold_bootstrap.flutter_gen_snapshot_path(flutter, "profile")
        self.assertIn("android-arm64-release", str(release))
        self.assertIn("android-arm64-profile", str(profile))
        self.assertEqual("gen_snapshot", release.name)
        self.assertEqual("gen_snapshot", profile.name)

    def test_unknown_flutter_mode_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported Flutter AOT mode"):
            flutter_cold_bootstrap.flutter_fixture_apk_path("debug")


if __name__ == "__main__":
    unittest.main()
