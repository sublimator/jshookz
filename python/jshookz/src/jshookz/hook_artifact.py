"""Canonical on-ledger envelope for compiled QuickJS Hooks.

The bytecode emitted by QuickJS is an internal compiler artifact.  Xahau stores
this envelope in ``sfCreateCode`` so artifact dispatch, decoder compatibility,
and runtime policy do not depend on ``HookApiVersion`` or filename conventions.

Versions 1 and 2 are deliberately fixed-width, big-endian wire formats.  Do
not serialize a native-language structure for consensus data.  New builders
emit only v2; v1 remains an exact parser compatibility surface.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from . import _runtime_profile_constants as generated
from .xfl_profile import (
    XFLArithmeticProfile,
    xfl_profile_code,
    xfl_profile_from_code,
)


MAGIC = b"XQJS"
LEGACY_ENVELOPE_VERSION = 1
ENVELOPE_VERSION = generated.XQJS_ENVELOPE_VERSION
SUPPORTED_ENVELOPE_VERSIONS = frozenset(
    {LEGACY_ENVELOPE_VERSION, ENVELOPE_VERSION}
)
QUICKJS_BYTECODE_KIND = 1
HEADER_SIZE = 80
MAX_CREATE_CODE_SIZE = 65_535
MAX_PAYLOAD_SIZE = MAX_CREATE_CODE_SIZE - HEADER_SIZE
IDENTITY_SIZE = 32

_HEADER = struct.Struct(">4sBBHHHI32s32s")


class HookArtifactError(ValueError):
    """The supplied bytes are not a canonical QuickJS Hook artifact."""


@dataclass(frozen=True)
class HookArtifact:
    envelope_version: int
    artifact_kind: int
    hook_api_version: int
    profile: XFLArithmeticProfile
    bytecode_abi_id: bytes
    runtime_profile_id: bytes
    payload: bytes


def _identity(value: bytes, name: str) -> bytes:
    value = bytes(value)
    if len(value) != IDENTITY_SIZE:
        raise HookArtifactError(
            f"{name} must be exactly {IDENTITY_SIZE} bytes, got {len(value)}"
        )
    if not any(value):
        raise HookArtifactError(f"{name} must not be the all-zero identity")
    return value


def build_hook_artifact(
    payload: bytes,
    *,
    hook_api_version: int,
    bytecode_abi_id: bytes,
    runtime_profile_id: bytes,
    profile: XFLArithmeticProfile,
) -> bytes:
    """Package provider bytecode into the canonical v2 deployment envelope."""
    payload = bytes(payload)
    if not payload:
        raise HookArtifactError("QuickJS Hook payload must not be empty")
    if len(payload) > MAX_PAYLOAD_SIZE:
        raise HookArtifactError(
            f"QuickJS Hook payload is {len(payload)} bytes; maximum is "
            f"{MAX_PAYLOAD_SIZE}"
        )
    if not 0 <= hook_api_version <= 0xFFFF:
        raise HookArtifactError("Hook API version must fit an unsigned 16-bit field")
    if not isinstance(profile, XFLArithmeticProfile):
        raise HookArtifactError("XFL arithmetic profile must be a named profile")

    bytecode_abi_id = _identity(bytecode_abi_id, "bytecode ABI identity")
    runtime_profile_id = _identity(runtime_profile_id, "runtime profile identity")
    header = _HEADER.pack(
        MAGIC,
        ENVELOPE_VERSION,
        QUICKJS_BYTECODE_KIND,
        HEADER_SIZE,
        hook_api_version,
        xfl_profile_code(profile),
        len(payload),
        bytecode_abi_id,
        runtime_profile_id,
    )
    assert len(header) == HEADER_SIZE
    return header + payload


def parse_hook_artifact(data: bytes) -> HookArtifact:
    """Parse and canonically validate a v1 or v2 QuickJS Hook artifact."""
    data = bytes(data)
    if len(data) > MAX_CREATE_CODE_SIZE:
        raise HookArtifactError(
            f"Hook artifact is {len(data)} bytes; maximum is {MAX_CREATE_CODE_SIZE}"
        )
    if len(data) < HEADER_SIZE:
        raise HookArtifactError(
            f"QuickJS Hook artifact is truncated: {len(data)} bytes, need at least "
            f"{HEADER_SIZE}"
        )

    (
        magic,
        envelope_version,
        artifact_kind,
        header_size,
        hook_api_version,
        profile_field,
        payload_size,
        bytecode_abi_id,
        runtime_profile_id,
    ) = _HEADER.unpack_from(data)

    if magic != MAGIC:
        raise HookArtifactError("not a QuickJS Hook artifact (XQJS magic missing)")
    if envelope_version not in SUPPORTED_ENVELOPE_VERSIONS:
        raise HookArtifactError(
            f"unsupported QuickJS Hook envelope version {envelope_version}"
        )
    if artifact_kind != QUICKJS_BYTECODE_KIND:
        raise HookArtifactError(
            f"unsupported QuickJS Hook artifact kind {artifact_kind}"
        )
    if header_size != HEADER_SIZE:
        raise HookArtifactError(
            f"non-canonical v{envelope_version} header size {header_size}; "
            f"expected {HEADER_SIZE}"
        )
    if envelope_version == LEGACY_ENVELOPE_VERSION:
        if profile_field != 0:
            raise HookArtifactError("QuickJS Hook v1 reserved flags must be zero")
        profile = XFLArithmeticProfile.NONE
    else:
        try:
            profile = xfl_profile_from_code(profile_field)
        except ValueError as error:
            raise HookArtifactError(
                f"unknown QuickJS Hook v2 XFL arithmetic profile code {profile_field}"
            ) from error
    if payload_size == 0:
        raise HookArtifactError("QuickJS Hook payload must not be empty")

    expected_size = HEADER_SIZE + payload_size
    if len(data) != expected_size:
        relation = "truncated" if len(data) < expected_size else "has trailing bytes"
        raise HookArtifactError(
            f"QuickJS Hook artifact {relation}: header declares {payload_size} "
            f"payload bytes, total is {len(data)}"
        )

    return HookArtifact(
        envelope_version=envelope_version,
        artifact_kind=artifact_kind,
        hook_api_version=hook_api_version,
        profile=profile,
        bytecode_abi_id=_identity(bytecode_abi_id, "bytecode ABI identity"),
        runtime_profile_id=_identity(
            runtime_profile_id, "runtime profile identity"
        ),
        payload=data[HEADER_SIZE:],
    )


def identity_from_hex(value: str, name: str) -> bytes:
    """Decode an exact 32-byte identity used at CLI/configuration boundaries."""
    try:
        decoded = bytes.fromhex(value)
    except ValueError as exc:
        raise HookArtifactError(f"{name} is not hexadecimal") from exc
    return _identity(decoded, name)
