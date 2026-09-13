from __future__ import annotations

import base64
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import flutter_cold_bootstrap  # noqa: E402


class FlutterColdBootstrapModeTest(unittest.TestCase):
    @staticmethod
    def _decode_gradle_defines(encoded: str) -> list[str]:
        return [base64.b64decode(item).decode() for item in encoded.split(",")]

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

    def test_ordinary_source_contract_is_mode_specific(self) -> None:
        self.assertEqual(
            (
                "source=live-vm+artifact-evidence",
                "source_live=1",
                "source_offline=0",
            ),
            flutter_cold_bootstrap._required_ordinary_source_markers("profile"),
        )
        self.assertEqual(
            (
                "source=artifact-index",
                "source_live=0",
                "source_offline=1",
            ),
            flutter_cold_bootstrap._required_ordinary_source_markers("release"),
        )

    def test_unknown_flutter_mode_fails_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported Flutter AOT mode"):
            flutter_cold_bootstrap.flutter_fixture_apk_path("debug")

    def test_gradle_dart_defines_follow_toolchain_version_injection_contract(self) -> None:
        common = dict(
            flutter_version="3.44.1",
            dart_version="3.12.1",
            channel="stable",
            repository_url="https://github.com/flutter/flutter.git",
            framework_revision="0123456789abcdef",
            engine_revision="fedcba9876543210",
        )
        old = flutter_cold_bootstrap.FlutterToolchain(
            **common, injects_flutter_version_defines=False
        )
        old_defines = self._decode_gradle_defines(
            flutter_cold_bootstrap._gradle_dart_defines(old)
        )
        self.assertEqual(
            [
                "DARTPLANT_CI_FLUTTER_VERSION=3.44.1",
                "DARTPLANT_CI_DART_VERSION=3.12.1",
                "DARTPLANT_CI_TARGET_ABI=arm64-v8a",
                "validate-deferred-components=false",
            ],
            old_defines,
        )

        new = flutter_cold_bootstrap.FlutterToolchain(
            **common, injects_flutter_version_defines=True
        )
        new_defines = self._decode_gradle_defines(
            flutter_cold_bootstrap._gradle_dart_defines(new)
        )
        self.assertIn("FLUTTER_VERSION=3.44.1", new_defines)
        self.assertIn("FLUTTER_FRAMEWORK_REVISION=0123456789", new_defines)
        self.assertIn("FLUTTER_ENGINE_REVISION=fedcba9876", new_defines)
        self.assertEqual("validate-deferred-components=false", new_defines[-1])

    def test_fixture_build_temporarily_binds_and_restores_flutter_sdk(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory)
            android = fixture / "android"
            android.mkdir(parents=True)
            lock = fixture / "pubspec.lock"
            local_properties = android / "local.properties"
            original_lock = b"original-lock\n"
            original_properties = (
                b"sdk.dir=/opt/android-sdk\n"
                b"flutter.sdk=/opt/flutter/original\n"
                b"custom.property=kept\n"
            )
            lock.write_bytes(original_lock)
            local_properties.write_bytes(original_properties)

            def fail_after_observing_binding(
                flutter: str, *, dobby_root: Path | None, build_mode: str
            ) -> flutter_cold_bootstrap.FlutterToolchain:
                self.assertEqual("/opt/flutter/3.24.0/bin/flutter", flutter)
                self.assertEqual("release", build_mode)
                self.assertIsNone(dobby_root)
                bound = local_properties.read_text().splitlines()
                self.assertIn("sdk.dir=/opt/android-sdk", bound)
                self.assertIn("custom.property=kept", bound)
                self.assertIn("flutter.sdk=/opt/flutter/3.24.0", bound)
                self.assertNotIn("flutter.sdk=/opt/flutter/original", bound)
                lock.write_text("mutated-lock\n")
                local_properties.write_text("mutated-properties\n")
                raise RuntimeError("synthetic fixture failure")

            with (
                mock.patch.object(flutter_cold_bootstrap, "FIXTURE_DIR", fixture),
                mock.patch.object(
                    flutter_cold_bootstrap,
                    "_build_fixture",
                    side_effect=fail_after_observing_binding,
                ),
            ):
                with self.assertRaisesRegex(RuntimeError, "synthetic fixture failure"):
                    flutter_cold_bootstrap.build_flutter_fixture(
                        flutter="/opt/flutter/3.24.0/bin/flutter",
                        build_mode="release",
                    )

            self.assertEqual(original_lock, lock.read_bytes())
            self.assertEqual(original_properties, local_properties.read_bytes())

    def test_missing_deferred_aab_entry_reports_packaging_context(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            aab_path = Path(directory) / "app-profile.aab"
            with zipfile.ZipFile(aab_path, "w") as archive:
                archive.writestr("base/lib/arm64-v8a/libapp.so", b"root")
                archive.writestr("deferred_probe/manifest/AndroidManifest.xml", b"manifest")

            with zipfile.ZipFile(aab_path) as archive:
                with self.assertRaisesRegex(
                    RuntimeError,
                    r"Flutter profile app bundle is missing required AOT entry "
                    r"deferred_probe/lib/arm64-v8a/libapp\.so-2\.part\.so",
                ) as failure:
                    flutter_cold_bootstrap._read_required_aab_entry(
                        archive,
                        "deferred_probe/lib/arm64-v8a/libapp.so-2.part.so",
                        build_mode="profile",
                        aab_path=aab_path,
                    )

            self.assertIn("base/lib/arm64-v8a/libapp.so", str(failure.exception))
            self.assertIn("deferred_probe/manifest/AndroidManifest.xml", str(failure.exception))

    def test_deferred_feature_declares_matching_profile_variant(self) -> None:
        feature_gradle = (
            ROOT / "tests" / "flutter_fixture" / "android" / "deferred_probe" / "build.gradle"
        ).read_text()
        self.assertIn("buildTypes {", feature_gradle)
        self.assertIn("profile {", feature_gradle)
        self.assertIn("initWith debug", feature_gradle)
        self.assertIn('matchingFallbacks = ["debug"]', feature_gradle)


if __name__ == "__main__":
    unittest.main()
