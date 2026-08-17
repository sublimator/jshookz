import struct

import pytest

from jshookz.hook_artifact import (
    HEADER_SIZE,
    MAGIC,
    MAX_PAYLOAD_SIZE,
    HookArtifactError,
    build_hook_artifact,
    identity_from_hex,
    parse_hook_artifact,
)


ABI_ID = bytes(range(32))
PROFILE_ID = bytes(range(32, 64))


def artifact(payload: bytes = b"provider bytecode") -> bytes:
    return build_hook_artifact(
        payload,
        hook_api_version=1,
        bytecode_abi_id=ABI_ID,
        runtime_profile_id=PROFILE_ID,
    )


def test_v1_wire_layout_is_exact_and_big_endian():
    encoded = artifact(b"abc")

    assert len(encoded) == HEADER_SIZE + 3
    assert encoded[:4] == MAGIC
    assert encoded[4:8] == bytes([1, 1, 0, HEADER_SIZE])
    assert encoded[8:12] == b"\x00\x01\x00\x00"
    assert encoded[12:16] == b"\x00\x00\x00\x03"
    assert encoded[16:48] == ABI_ID
    assert encoded[48:80] == PROFILE_ID
    assert encoded[80:] == b"abc"

    decoded = parse_hook_artifact(encoded)
    assert decoded.envelope_version == 1
    assert decoded.artifact_kind == 1
    assert decoded.hook_api_version == 1
    assert decoded.bytecode_abi_id == ABI_ID
    assert decoded.runtime_profile_id == PROFILE_ID
    assert decoded.payload == b"abc"


@pytest.mark.parametrize(
    ("offset", "replacement", "match"),
    [
        (0, b"Q", "magic"),
        (4, b"\x02", "envelope version 2"),
        (5, b"\x02", "artifact kind 2"),
        (7, b"\x51", "header size 81"),
        (11, b"\x01", "reserved flags"),
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
        )


def test_identity_hex_decoder_is_strict():
    assert identity_from_hex(ABI_ID.hex(), "ABI") == ABI_ID
    with pytest.raises(HookArtifactError, match="not hexadecimal"):
        identity_from_hex("gg", "ABI")
    with pytest.raises(HookArtifactError, match="exactly 32 bytes"):
        identity_from_hex("00", "ABI")
