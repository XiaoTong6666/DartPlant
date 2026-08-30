from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parent.parent
INCLUDE_DIR = ROOT_DIR / "include"


def public_headers() -> list[str]:
    return sorted(
        path.relative_to(INCLUDE_DIR).as_posix()
        for path in (INCLUDE_DIR / "dartplant").rglob("*.h")
    )


def main() -> None:
    compiler = shutil.which("clang")
    if compiler is None:
        raise SystemExit("clang is required for the public C header check")

    headers = public_headers()
    if not headers:
        raise SystemExit("no public DartPlant headers found")

    failures: list[str] = []
    for header in headers:
        source = f'#include "{header}"\nint main(void) {{ return 0; }}\n'
        result = subprocess.run(
            [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{INCLUDE_DIR}",
                "-x",
                "c",
                "-fsyntax-only",
                "-",
            ],
            input=source,
            text=True,
            cwd=ROOT_DIR,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            failures.append(f"=== {header} ===\n{result.stderr.rstrip()}")

    if failures:
        raise SystemExit("\n\n".join(failures))
    print(f"public C headers verified: {len(headers)}")


if __name__ == "__main__":
    main()
