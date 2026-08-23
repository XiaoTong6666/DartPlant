use anyhow::{Context, Result, bail};
use flutterdec_adapter::{AdapterInput, ProgramModel, run_adapter};
use flutterdec_loader::{load_snapshot_bundle, read_apk_entry};
use goblin::elf::Elf;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, Serialize)]
struct DartPlantMetadata {
    format: u32,
    producer: &'static str,
    adapter_kind: String,
    dart_version: String,
    snapshot_hash: String,
    architecture: String,
    snapshot_kind: &'static str,
    source: SourceInfo,
    module: ModuleInfo,
    methods: Vec<MethodInfo>,
}

#[derive(Debug, Serialize)]
struct SourceInfo {
    input_path: String,
    libapp_path: String,
}

#[derive(Debug, Serialize)]
struct ModuleInfo {
    soname: &'static str,
    build_id: Option<String>,
    sha256: String,
}

#[derive(Debug, Serialize)]
struct MethodInfo {
    library_uri: String,
    #[serde(rename = "class")]
    class_name: String,
    function_name: String,
    name: String,
    signature: String,
    entry_kind: u32,
    address_kind: u32,
    code_section_va: u64,
    code_offset: String,
    entry_va: String,
    code_size: u64,
    fingerprint: String,
    fingerprint_algo: &'static str,
    name_kind: String,
}

#[derive(Debug)]
struct ElfFile {
    bytes: Vec<u8>,
    build_id: Option<String>,
    executable_base_va: u64,
    sections: Vec<(u64, u64, u64)>,
}

impl ElfFile {
    fn open(path: &Path) -> Result<Self> {
        let bytes = fs::read(path).with_context(|| format!("read {}", path.display()))?;
        Self::from_bytes(bytes)
    }

    fn from_bytes(bytes: Vec<u8>) -> Result<Self> {
        let elf = Elf::parse(&bytes).context("parse libapp ELF")?;
        if elf.header.e_machine != goblin::elf::header::EM_AARCH64 {
            bail!("DartPlant metadata requires ARM64 libapp.so");
        }
        let executable_base_va = elf
            .program_headers
            .iter()
            .find(|header| {
                header.p_type == goblin::elf::program_header::PT_LOAD
                    && header.p_flags & goblin::elf::program_header::PF_X != 0
            })
            .map(|header| header.p_vaddr)
            .unwrap_or(0);
        let sections = elf
            .program_headers
            .iter()
            .filter(|header| header.p_type == goblin::elf::program_header::PT_LOAD)
            .map(|header| (header.p_vaddr, header.p_offset, header.p_filesz))
            .collect();
        Ok(Self {
            build_id: extract_build_id(&elf, &bytes),
            bytes,
            executable_base_va,
            sections,
        })
    }

    fn read_va(&self, va: u64, size: u64) -> Result<&[u8]> {
        for (segment_va, file_offset, file_size) in &self.sections {
            let end = segment_va.saturating_add(*file_size);
            if va >= *segment_va && va.saturating_add(size) <= end {
                let offset = file_offset.saturating_add(va - segment_va) as usize;
                let end = offset.saturating_add(size as usize);
                return self
                    .bytes
                    .get(offset..end)
                    .context("ELF VA range outside file");
            }
        }
        bail!("cannot map ELF VA 0x{va:x} size {size}");
    }
}

fn extract_build_id(elf: &Elf<'_>, bytes: &[u8]) -> Option<String> {
    for header in &elf.program_headers {
        if header.p_type != goblin::elf::program_header::PT_NOTE {
            continue;
        }
        let start = header.p_offset as usize;
        let end = start
            .saturating_add(header.p_filesz as usize)
            .min(bytes.len());
        let mut cursor = start;
        while cursor + 12 <= end {
            let namesz = u32::from_le_bytes(bytes[cursor..cursor + 4].try_into().ok()?) as usize;
            let descsz =
                u32::from_le_bytes(bytes[cursor + 4..cursor + 8].try_into().ok()?) as usize;
            let note_type = u32::from_le_bytes(bytes[cursor + 8..cursor + 12].try_into().ok()?);
            cursor += 12;
            let name_end = cursor.saturating_add((namesz + 3) & !3);
            let desc_end = name_end.saturating_add((descsz + 3) & !3);
            if desc_end > end {
                break;
            }
            if note_type == 3 && namesz >= 3 && bytes.get(cursor..cursor + 3) == Some(b"GNU") {
                return Some(
                    bytes[name_end..name_end + descsz]
                        .iter()
                        .map(|byte| format!("{byte:02x}"))
                        .collect(),
                );
            }
            cursor = desc_end;
        }
    }
    None
}

fn fingerprint(bytes: &[u8]) -> String {
    let mut value = 14695981039346656037u64;
    for byte in bytes {
        value ^= u64::from(*byte);
        value = value.wrapping_mul(1099511628211);
    }
    format!("{value:016x}")
}

fn sha256(bytes: &[u8]) -> String {
    let mut digest = Sha256::new();
    digest.update(bytes);
    format!("{:x}", digest.finalize())
}

fn cached_adapter(repo: &Path, hash: &str) -> Result<PathBuf> {
    let cache = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target/dartplant-adapters")
        .join(format!("dart_adapter_{hash}"));
    if cache.exists() {
        return Ok(cache);
    }
    let template = repo.join("adapters/python/adapter_template.py");
    if !template.exists() {
        bail!("vendored flutterdec adapter template is missing");
    }
    if let Some(parent) = cache.parent() {
        fs::create_dir_all(parent)?;
    }
    let script = format!(
        "#!/usr/bin/env python3\nfrom pathlib import Path\nimport sys\nroot = Path({repo:?})\nsys.path.insert(0, str(root / 'adapters' / 'python'))\nimport adapter_template\nif __name__ == '__main__':\n    raise SystemExit(adapter_template.entrypoint(default_snapshot_hash={hash:?}, default_version='unknown'))\n",
        repo = repo,
        hash = hash,
    );
    fs::write(&cache, script)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut permissions = fs::metadata(&cache)?.permissions();
        permissions.set_mode(0o755);
        fs::set_permissions(&cache, permissions)?;
    }
    Ok(cache)
}

fn class_library_map(model: &ProgramModel) -> HashMap<String, Option<String>> {
    let mut map: HashMap<String, Vec<String>> = HashMap::new();
    for class in &model.classes {
        map.entry(class.name.clone())
            .or_default()
            .push(class.library_uri.clone());
    }
    map.into_iter()
        .map(|(name, mut libraries)| {
            libraries.sort();
            libraries.dedup();
            let library = if libraries.len() == 1 {
                libraries.into_iter().next()
            } else {
                None
            };
            (name, library)
        })
        .collect()
}

fn convert_model(
    input: &Path,
    bundle: &flutterdec_loader::SnapshotBundle,
    model: &ProgramModel,
) -> Result<DartPlantMetadata> {
    if model.arch != "arm64" {
        bail!("flutterdec model is not ARM64");
    }
    if model.functions.is_empty() {
        bail!("flutterdec model contains no functions");
    }
    let libapp = if input.extension().and_then(|value| value.to_str()) == Some("apk") {
        ElfFile::from_bytes(read_apk_entry(input, "lib/arm64-v8a/libapp.so")?)?
    } else {
        ElfFile::open(&bundle.libapp_path)?
    };
    let class_libraries = class_library_map(model);
    let mut methods = Vec::new();
    let mut emitted_names = HashSet::new();
    for function in &model.functions {
        let Some(library_uri) = class_libraries
            .get(&function.owner_class)
            .cloned()
            .flatten()
        else {
            continue;
        };
        let name_kind = function
            .name_kind
            .clone()
            .unwrap_or_else(|| "unknown".to_string());
        if !matches!(name_kind.as_str(), "exact" | "external" | "heuristic") {
            continue;
        }
        if function.entry_va == 0 || function.size == 0 {
            continue;
        }
        // blutter may expose the same logical Dart function together with its
        // forwarding/checking wrapper. The first record is the Code entry
        // recovered from the Function object; keep it and reject later
        // same-name wrappers so method lookup is deterministic.
        let logical_name = (
            library_uri.clone(),
            function.owner_class.clone(),
            function.name.clone(),
        );
        if !emitted_names.insert(logical_name) {
            continue;
        }
        let section_va = if function.code_section_va == 0 {
            libapp.executable_base_va
        } else {
            function.code_section_va
        };
        let code_offset = function
            .entry_va
            .checked_sub(section_va)
            .context("function entry is before instruction section")?;
        let code = libapp.read_va(function.entry_va, function.size)?;
        methods.push(MethodInfo {
            library_uri,
            class_name: function.owner_class.clone(),
            function_name: function.name.clone(),
            name: function.name.clone(),
            signature: String::new(),
            entry_kind: 0,
            address_kind: 3,
            code_section_va: section_va,
            code_offset: format!("0x{code_offset:x}"),
            entry_va: format!("0x{:x}", function.entry_va),
            code_size: function.size,
            fingerprint: fingerprint(code),
            fingerprint_algo: "fnv1a64",
            name_kind,
        });
    }
    if methods.is_empty() {
        bail!("flutterdec model contains no usable named methods");
    }
    Ok(DartPlantMetadata {
        format: 1,
        producer: "dartplant-indexer",
        adapter_kind: model.adapter_kind.clone(),
        dart_version: model.dart_version.clone(),
        snapshot_hash: if model.snapshot_hash.is_empty() || model.snapshot_hash == "unknown" {
            bundle.snapshot_hash.clone()
        } else {
            model.snapshot_hash.clone()
        },
        architecture: model.arch.clone(),
        snapshot_kind: "full-aot",
        source: SourceInfo {
            input_path: input
                .file_name()
                .and_then(|value| value.to_str())
                .unwrap_or("input")
                .to_string(),
            libapp_path: "libapp.so".to_string(),
        },
        module: ModuleInfo {
            soname: "libapp.so",
            build_id: libapp.build_id,
            sha256: sha256(&libapp.bytes),
        },
        methods,
    })
}

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let input = PathBuf::from(
        args.next()
            .context("usage: dartplant-indexer <APK|libapp.so> <out>")?,
    );
    let output = PathBuf::from(
        args.next()
            .context("usage: dartplant-indexer <APK|libapp.so> <out>")?,
    );
    let mut libflutter = None;
    while let Some(arg) = args.next() {
        if arg == "--libflutter" {
            libflutter = Some(PathBuf::from(
                args.next().context("--libflutter requires a path")?,
            ));
        } else {
            bail!("unknown argument: {arg}");
        }
    }
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    normalize_compiler_environment();
    let bundle = load_snapshot_bundle(&input)?;
    let flutterdec = repo.join("../../third_party/flutterdec");
    let blutter = repo.join("../../third_party/blutter/blutter.py");
    if std::env::var_os("FLUTTERDEC_BLUTTER_CMD").is_none() && blutter.exists() {
        // Supplying the flag in the command is important because the adapter
        // identifies a python command as a custom runner and otherwise would
        // omit blutter's --no-analysis argument.
        std::env::set_var(
            "FLUTTERDEC_BLUTTER_CMD",
            format!("python3 {} --no-analysis", blutter.display()),
        );
    }
    let adapter = cached_adapter(&flutterdec, &bundle.snapshot_hash)?;
    let input_dir;
    let adapter_input = if input.extension().and_then(|value| value.to_str()) == Some("apk") {
        input.as_path()
    } else if let Some(libflutter) = libflutter.as_ref() {
        input_dir = tempfile::tempdir()?;
        fs::copy(&input, input_dir.path().join("libapp.so"))?;
        fs::copy(libflutter, input_dir.path().join("libflutter.so"))?;
        input_dir.path()
    } else {
        input.as_path()
    };
    let backend = std::env::var("DARTPLANT_ADAPTER_BACKEND").unwrap_or_else(|_| "auto".to_string());
    let model = run_adapter(
        &adapter,
        &AdapterInput {
            input_path: Some(adapter_input),
            libapp_path: Some(&bundle.libapp_path),
            vm_data: &bundle.vm_data,
            isolate_data: &bundle.isolate_data,
            vm_instr: &bundle.vm_instr,
            isolate_instr: &bundle.isolate_instr,
            vm_instr_va: bundle.vm_instr_va,
            isolate_instr_va: bundle.isolate_instr_va,
            backend: Some(&backend),
        },
    )?;
    if let Some(debug_path) = std::env::var_os("DARTPLANT_DUMP_MODEL") {
        fs::write(debug_path, serde_json::to_vec_pretty(&model)?)?;
    }
    let metadata = convert_model(&input, &bundle, &model)?;
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(output, serde_json::to_vec_pretty(&metadata)?)?;
    Ok(())
}

fn normalize_compiler_environment() {
    for (variable, fallback) in [("CC", "gcc"), ("CXX", "g++")] {
        let valid = std::env::var(variable)
            .ok()
            .and_then(|value| value.split_whitespace().next().map(PathBuf::from))
            .map(|path| path.is_absolute() && path.is_file())
            .unwrap_or(false);
        if !valid {
            std::env::set_var(variable, fallback);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn class_library_map_keeps_unique_owners() {
        let model = ProgramModel {
            schema_version: 3,
            adapter_kind: "test".to_string(),
            dart_version: "test".to_string(),
            snapshot_hash: "test".to_string(),
            arch: "arm64".to_string(),
            libraries: vec![flutterdec_adapter::LibraryInfo {
                id: 0,
                uri: "package:fixture/main.dart".to_string(),
                name_display: "package:fixture/main.dart".to_string(),
            }],
            classes: vec![flutterdec_adapter::ClassInfo {
                id: 0,
                name: "Fixture".to_string(),
                super_name: "Object".to_string(),
                library_uri: "package:fixture/main.dart".to_string(),
            }],
            functions: Vec::new(),
            object_pool: vec![],
            pool_geometry: None,
        };
        let map = class_library_map(&model);
        assert_eq!(
            map.get("Fixture").and_then(|value| value.as_deref()),
            Some("package:fixture/main.dart")
        );
    }

    #[test]
    fn class_library_map_rejects_ambiguous_owners() {
        let mut model = ProgramModel {
            schema_version: 3,
            adapter_kind: "test".to_string(),
            dart_version: "test".to_string(),
            snapshot_hash: "test".to_string(),
            arch: "arm64".to_string(),
            libraries: Vec::new(),
            classes: Vec::new(),
            functions: Vec::new(),
            object_pool: vec![],
            pool_geometry: None,
        };
        for (id, uri) in ["package:first/a.dart", "package:second/b.dart"]
            .into_iter()
            .enumerate()
        {
            model.classes.push(flutterdec_adapter::ClassInfo {
                id: id as u64,
                name: "Duplicate".to_string(),
                super_name: "Object".to_string(),
                library_uri: uri.to_string(),
            });
        }
        let map = class_library_map(&model);
        assert_eq!(map.get("Duplicate"), Some(&None));
    }

    #[test]
    fn fnv_fingerprint_matches_runtime() {
        assert_eq!(fingerprint(&[1, 2, 3, 4]), "be7a5e775165785d");
    }
}
