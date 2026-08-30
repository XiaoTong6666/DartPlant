from __future__ import annotations

import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import generate_vm_profiles  # noqa: E402


class VmProfilesGeneratorTest(unittest.TestCase):
    def test_selects_aot_product_arm64_compressed_block(self) -> None:
        text = r"""
#if defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \
    defined(DART_COMPRESSED_POINTERS)
static constexpr dart::compiler::target::word Function_code_offset = 0x44;
#endif
#if defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \
    defined(DART_COMPRESSED_POINTERS)
static constexpr dart::compiler::target::word AOT_Function_code_offset = 0x2c;
static constexpr dart::compiler::target::word AOT_Function_entry_point_offset[] = {0x8, 0x10};
#endif
"""
        block = generate_vm_profiles._product_arm64_compressed_aot_block(text)
        self.assertIn("AOT_Function_code_offset", block)
        self.assertEqual(
            0x2C,
            generate_vm_profiles._parse_aot_offset(block, "AOT_Function_code_offset"),
        )
        self.assertEqual(
            0x8,
            generate_vm_profiles._parse_aot_offset(block, "AOT_Function_entry_point_offset"),
        )
        self.assertEqual(
            0x10,
            generate_vm_profiles._parse_aot_offset_at(
                block, "AOT_Function_entry_point_offset", 1
            ),
        )

    def test_profile_verifier_rejects_source_offset_drift(self) -> None:
        profile = generate_vm_profiles._load_manifest()[0]
        bindings = generate_vm_profiles.AOT_OFFSET_BINDINGS
        lines = [
            "#if defined(PRODUCT) && defined(TARGET_ARCH_ARM64) && \\",
            "    defined(DART_COMPRESSED_POINTERS)",
        ]
        emitted_arrays: set[str] = set()
        for (section, field), sdk_name in bindings.items():
            array_binding = generate_vm_profiles.AOT_ARRAY_OFFSET_BINDINGS.get(
                (section, field)
            )
            if array_binding is not None:
                array_name = array_binding[0]
                if array_name in emitted_arrays:
                    continue
                emitted_arrays.add(array_name)
                if array_name == "AOT_Code_entry_point_offset":
                    code = profile["code"]
                    values = [
                        code["entry_point"],
                        code["unchecked_entry_point"],
                        code["monomorphic_entry_point"],
                        code["monomorphic_unchecked_entry_point"],
                    ]
                else:
                    function = profile["function"]
                    values = [
                        function["entry_point"],
                        function["unchecked_entry_point"],
                    ]
                rendered = ", ".join(f"0x{int(value):x}" for value in values)
                lines.append(
                    "static constexpr dart::compiler::target::word "
                    f"{array_name}[] = {{{rendered}}};"
                )
                continue
            value = int(profile[section][field])
            lines.append(
                "static constexpr dart::compiler::target::word "
                f"{sdk_name} = 0x{value:x};"
            )
        lines.append("#endif")
        text = "\n".join(lines)
        generate_vm_profiles._verify_profile_against_aot_offsets(profile, text)

        drifted = text.replace(
            "AOT_Thread_heap_base_offset = 0x48;",
            "AOT_Thread_heap_base_offset = 0x50;",
        )
        with self.assertRaisesRegex(ValueError, "thread.heap_base"):
            generate_vm_profiles._verify_profile_against_aot_offsets(profile, drifted)

    def test_payload_contract_verifier_requires_dart_payload_start_semantics(self) -> None:
        object_header = """
bool HasMonomorphicEntry(const CodePtr code) {
  return code->untag()->entry_point_ != code->untag()->monomorphic_entry_point_;
}
uword PayloadStartOf(const CodePtr code) {
  const uword entry_offset = HasMonomorphicEntry(code)
      ? Instructions::kPolymorphicEntryOffsetAOT
      : 0;
  return EntryPointOf(code) - entry_offset;
}
"""
        app_snapshot = """
uword start = Code::PayloadStartOf(code);
code->untag()->instructions_length_ = previous_end - start;
"""
        generate_vm_profiles._verify_aot_payload_contract(
            object_header, app_snapshot, source_name="test"
        )
        with self.assertRaisesRegex(ValueError, "PayloadStartOf"):
            generate_vm_profiles._verify_aot_payload_contract(
                object_header.replace("entry_point_ !=", "entry_point_ =="),
                app_snapshot,
                source_name="test",
            )


if __name__ == "__main__":
    unittest.main()
