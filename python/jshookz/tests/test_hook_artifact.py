import struct

import pytest

from jshookz.hook_artifact import (
    ENVELOPE_VERSION,
    HEADER_SIZE,
    LEGACY_ENVELOPE_VERSION,
    MAGIC,
    MAX_PAYLOAD_SIZE,
    HookArtifactError,
    build_hook_artifact,
    identity_from_hex,
    parse_hook_artifact,
)
from jshookz.xfl_profile import XFLArithmeticProfile


ABI_ID = bytes(range(32))
PROFILE_ID = bytes(range(32, 64))


def artifact(
    payload: bytes = b"provider bytecode",
    profile: XFLArithmeticProfile = XFLArithmeticProfile.NONE,
) -> bytes:
    return build_hook_artifact(
        payload,
        hook_api_version=1,
        bytecode_abi_id=ABI_ID,
        runtime_profile_id=PROFILE_ID,
        profile=profile,
    )


def test_v2_wire_layout_is_exact_and_big_endian():
    encoded = artifact(b"abc")

    assert len(encoded) == HEADER_SIZE + 3
    assert encoded[:4] == MAGIC
    assert encoded[4:8] == bytes([2, 1, 0, HEADER_SIZE])
    assert encoded[8:12] == b"\x00\x01\x00\x00"
    assert encoded[12:16] == b"\x00\x00\x00\x03"
    assert encoded[16:48] == ABI_ID
    assert encoded[48:80] == PROFILE_ID
    assert encoded[80:] == b"abc"

    decoded = parse_hook_artifact(encoded)
    assert decoded.envelope_version == ENVELOPE_VERSION
    assert decoded.artifact_kind == 1
    assert decoded.hook_api_version == 1
    assert decoded.profile is XFLArithmeticProfile.NONE
    assert decoded.bytecode_abi_id == ABI_ID
    assert decoded.runtime_profile_id == PROFILE_ID
    assert decoded.payload == b"abc"


@pytest.mark.parametrize(
    ("profile", "wire"),
    [
        (XFLArithmeticProfile.NONE, b"\x00\x00"),
        (XFLArithmeticProfile.XAHAU_FLOAT_V1, b"\x00\x01"),
        (XFLArithmeticProfile.NEAREST_EVEN_V1, b"\x00\x02"),
    ],
)
def test_v2_profile_code_is_canonical_big_endian(
    profile: XFLArithmeticProfile, wire: bytes
):
    encoded = artifact(profile=profile)

    assert encoded[10:12] == wire
    assert parse_hook_artifact(encoded).profile is profile


def test_frozen_literal_v1_remains_readable_as_profile_none():
    frozen_v1 = bytes.fromhex(
        "58514a53010100500001000000000003"
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
        "616263"
    )

    decoded = parse_hook_artifact(frozen_v1)

    assert decoded.envelope_version == LEGACY_ENVELOPE_VERSION
    assert decoded.profile is XFLArithmeticProfile.NONE
    assert decoded.payload == b"abc"


@pytest.mark.parametrize(
    ("offset", "replacement", "match"),
    [
        (0, b"Q", "magic"),
        (4, b"\x03", "envelope version 3"),
        (5, b"\x02", "artifact kind 2"),
        (7, b"\x51", "header size 81"),
        (10, b"\x01", "profile code 256"),
        (11, b"\x03", "profile code 3"),
    ],
)
def test_parser_rejects_noncanonical_header_fields(offset, replacement, match):
    encoded = bytearray(artifact())
    encoded[offset : offset + len(replacement)] = replacement

    with pytest.raises(HookArtifactError, match=match):
        parse_hook_artifact(encoded)


def test_parser_rejects_truncation_trailing_bytes_and_empty_payload():
    encoded = artifact(b"abc")
    with pytest.raises(HookArtifactError, match="truncated"):
        parse_hook_artifact(encoded[:-1])
    with pytest.raises(HookArtifactError, match="trailing bytes"):
        parse_hook_artifact(encoded + b"x")

    empty = bytearray(encoded[:HEADER_SIZE])
    empty[12:16] = struct.pack(">I", 0)
    with pytest.raises(HookArtifactError, match="must not be empty"):
        parse_hook_artifact(empty)


def test_v1_still_requires_both_reserved_profile_bytes_zero():
    encoded = bytearray(artifact())
    encoded[4] = LEGACY_ENVELOPE_VERSION
    for offset in (10, 11):
        mutated = bytearray(encoded)
        mutated[offset] = 1
        with pytest.raises(HookArtifactError, match="v1 reserved flags"):
            parse_hook_artifact(mutated)


def test_explicit_downgrade_rewrite_parses_as_v1_none_without_changing_payload():
    encoded = bytearray(
        artifact(b"profiled module", XFLArithmeticProfile.XAHAU_FLOAT_V1)
    )
    encoded[4] = LEGACY_ENVELOPE_VERSION
    encoded[10:12] = b"\x00\x00"

    decoded = parse_hook_artifact(encoded)

    assert decoded.envelope_version == LEGACY_ENVELOPE_VERSION
    assert decoded.profile is XFLArithmeticProfile.NONE
    assert decoded.payload == b"profiled module"


def test_parser_rejects_raw_bytecode_and_zero_identities():
    with pytest.raises(HookArtifactError, match="truncated"):
        parse_hook_artifact(b"\x05raw-qjsc")

    encoded = bytearray(artifact())
    encoded[16:48] = bytes(32)
    with pytest.raises(HookArtifactError, match="bytecode ABI.*all-zero"):
        parse_hook_artifact(encoded)

    encoded = bytearray(artifact())
    encoded[48:80] = bytes(32)
    with pytest.raises(HookArtifactError, match="runtime profile.*all-zero"):
        parse_hook_artifact(encoded)


def test_builder_enforces_on_ledger_size_limit_and_identity_width():
    with pytest.raises(HookArtifactError, match="maximum"):
        artifact(bytes(MAX_PAYLOAD_SIZE + 1))
    with pytest.raises(HookArtifactError, match="exactly 32 bytes"):
        build_hook_artifact(
            b"x",
            hook_api_version=1,
            bytecode_abi_id=b"short",
            runtime_profile_id=PROFILE_ID,
            profile=XFLArithmeticProfile.NONE,
        )
    with pytest.raises(HookArtifactError, match="named profile"):
        build_hook_artifact(
            b"x",
            hook_api_version=1,
            bytecode_abi_id=ABI_ID,
            runtime_profile_id=PROFILE_ID,
            profile=1,  # type: ignore[arg-type]
        )


def test_identity_hex_decoder_is_strict():
    assert identity_from_hex(ABI_ID.hex(), "ABI") == ABI_ID
    with pytest.raises(HookArtifactError, match="not hexadecimal"):
        identity_from_hex("gg", "ABI")
    with pytest.raises(HookArtifactError, match="exactly 32 bytes"):
        identity_from_hex("00", "ABI")
