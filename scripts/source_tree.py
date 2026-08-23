from __future__ import annotations

from pathlib import Path

from util import ROOT_DIR


RUST_EXTENSIONS = {".rs"}
CPP_EXTENSIONS = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
CPP_SOURCE_EXTENSIONS = {".c", ".cc", ".cpp"}
TEXT_EXTENSIONS = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".h",
    ".hh",
    ".hpp",
    ".json",
    ".md",
    ".prop",
    ".py",
    ".rs",
    ".sh",
    ".toml",
    ".txt",
    ".xml",
    ".yml",
    ".yaml",
}
TEXT_FILENAMES = {
    "CMakeLists.txt",
}
IGNORED_PARTS = {
    ".git",
    ".idea",
    ".cxx",
    "build",
    "out",
    "third_party",
    "tools/dartplant-indexer/target",
    "tests/flutter_fixture/build",
    "tests/flutter_fixture/.dart_tool",
    "tests/flutter_fixture/android/.gradle",
}


def is_ignored(path: Path) -> bool:
    relative_path = path.relative_to(ROOT_DIR)
    relative = relative_path.as_posix()
    if ".cxx" in relative_path.parts:
        return True
    if relative in IGNORED_PARTS:
        return True
    return any(relative == part or relative.startswith(part + "/") for part in IGNORED_PARTS)


def iter_files(extensions: set[str]) -> list[Path]:
    files: list[Path] = []
    for path in ROOT_DIR.rglob("*"):
        if not path.is_file() or is_ignored(path):
            continue
        if path.suffix.lower() in extensions:
            files.append(path)
    return sorted(files)


def iter_text_files() -> list[Path]:
    files: list[Path] = []
    for path in ROOT_DIR.rglob("*"):
        if not path.is_file() or is_ignored(path):
            continue
        if path.name in TEXT_FILENAMES or path.suffix.lower() in TEXT_EXTENSIONS:
            files.append(path)
    return sorted(files)
