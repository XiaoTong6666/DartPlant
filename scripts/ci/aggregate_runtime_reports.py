#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path

from common import RUNTIME_SCENARIOS


def main() -> int:
    parser = argparse.ArgumentParser(description="Aggregate DartPlant runtime report JSON files")
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--title", default="DartPlant Flutter runtime matrix")
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    for path in sorted(args.reports.glob("*.json")):
        document = json.loads(path.read_text())
        checks = {
            check["name"]: bool(check["passed"])
            for check in document.get("checks", [])
            if isinstance(check, dict) and "name" in check
        }
        rows.append(
            {
                "flutter": document.get("flutter", "?"),
                "dart": document.get("dart", "?"),
                "result": document.get("result", "FAIL"),
                "test": document.get("test", "all"),
                "checks": checks,
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        f"## {args.title}",
        "",
        "| Flutter | Dart | "
        + " | ".join(RUNTIME_SCENARIOS)
        + " | Result |",
        "| --- | --- | " + " | ".join("---" for _ in RUNTIME_SCENARIOS) + " | --- |",
    ]
    for row in rows:
        checks = row["checks"]
        assert isinstance(checks, dict)
        expected_test = row["test"]
        scenario_cells = []
        for scenario in RUNTIME_SCENARIOS:
            if expected_test != "all" and expected_test != scenario:
                scenario_cells.append("—")
            else:
                scenario_cells.append("✅" if checks.get(f"Scenario {scenario}") else "❌")
        lines.append(
            f"| {row['flutter']} | {row['dart']} | "
            + " | ".join(scenario_cells)
            + f" | {row['result']} |"
        )
    if not rows:
        lines.extend(["", "No runtime reports were produced."])
    args.output.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
