from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[2]
REGISTRY_PATH = ROOT_DIR / "scripts" / "data" / "vm_capabilities.json"


@dataclass(frozen=True)
class Capability:
    key: str
    cpp_symbol: str
    diagnostic_name: str
    bit: int
    cold_required: bool
    verified_after_create: bool
    ci_required_event: bool


def _load() -> tuple[Capability, ...]:
    document = json.loads(REGISTRY_PATH.read_text())
    if document.get("schema_version") != 1:
        raise ValueError("unsupported VM capability registry schema")
    raw = document.get("capabilities")
    if not isinstance(raw, list) or not raw:
        raise ValueError("VM capability registry is empty")
    capabilities = tuple(Capability(**entry) for entry in raw)
    bits = [capability.bit for capability in capabilities]
    names = [capability.diagnostic_name for capability in capabilities]
    keys = [capability.key for capability in capabilities]
    if len(set(bits)) != len(bits) or any(bit <= 0 or bit & (bit - 1) for bit in bits):
        raise ValueError("VM capability registry bits must be unique powers of two")
    if len(set(names)) != len(names) or len(set(keys)) != len(keys):
        raise ValueError("VM capability registry keys/names must be unique")
    return capabilities


CAPABILITIES = _load()
BY_NAME = {capability.diagnostic_name: capability for capability in CAPABILITIES}
BY_KEY = {capability.key: capability for capability in CAPABILITIES}


def mask(*, cold_required: bool = False, verified_after_create: bool = False) -> int:
    result = 0
    for capability in CAPABILITIES:
        if cold_required and not capability.cold_required:
            continue
        if verified_after_create and not capability.verified_after_create:
            continue
        result |= capability.bit
    return result


def required_event_capabilities() -> tuple[Capability, ...]:
    return tuple(capability for capability in CAPABILITIES if capability.ci_required_event)
