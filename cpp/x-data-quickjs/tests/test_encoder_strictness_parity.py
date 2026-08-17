"""Encoder strictness parity (issue 0010).

The JS/wasm encoder and the boost::json (CLI/diagnostic) encoder disagreed on
malformed UInt64 input, because they parsed it with two different helpers:

    try_parse_hex_uint64  (errors-as-values)  checked ec AND ptr == end, and
                                              capped at 16 hex chars
    parse_hex_uint64      (throwing)          checked ec only

`std::from_chars` stops at the first invalid character and reports success, so
the throwing twin accepted input the errors-as-values twin rejected. Measured
before the fix:

    "1234GG"             expected REJECT   throwing twin ACCEPT 0x1234
    "00000000000000001"  expected REJECT   throwing twin ACCEPT 0x1   (17 chars)

The second case was not in the issue as written; it turned up while
reproducing the first.

Fixed on two levels, because either alone leaves a way back in:

  1. `UInt64Codec::encode(Serializer&, boost::json::value const&)` delegates to
     `encode_hex` -> `encode_hex_expected`, so there is one parsing path.
  2. The throwing helpers themselves now require full consumption (and the hex
     one caps at 16 chars), so calling one directly cannot resurrect the
     asymmetry either.

These are source-level properties, so this test reads the source. That is a
deliberate choice, not a shortcut: a behavioural test of the boost::json
encoder means building `catl_xdata_cli`, which needs boost natively and cannot
run in this suite. The helper-level behaviour was verified out-of-band with a
compiled probe over the five inputs above; what has to hold *continuously* is
that the two paths stay unified, which is exactly what is asserted here.
"""

from __future__ import annotations

import re

from conftest import REPO


XDATA = REPO / "cpp" / "x-data" / "includes" / "catl" / "xdata"
CODEC_ERROR_H = XDATA / "codec-error.h"
UINT_H = XDATA / "codecs" / "uint.h"

# Every from_chars-based parse helper, throwing and errors-as-values alike.
# Both families must be strict; that is the whole point of the issue.
PARSE_HELPERS = (
    "parse_int64",
    "parse_uint64",
    "parse_hex_uint64",
    "try_parse_int64",
    "try_parse_hex_uint64",
)


def _helper_body(name: str) -> str:
    """The source of one helper, from its definition to its closing brace."""
    src = CODEC_ERROR_H.read_text()
    # Definitions sit at the start of a line, return type on the line before.
    m = re.search(rf"^{re.escape(name)}\(", src, re.MULTILINE)
    assert m, f"helper {name} not found in {CODEC_ERROR_H.name}"
    rest = src[m.start():]
    end = rest.find("\n}\n")
    assert end != -1, f"could not find end of {name}"
    return rest[: end + 3]


def test_all_parse_helpers_require_full_consumption():
    """from_chars stops at the first bad char and calls that success.

    Any helper that checks only `ec` silently truncates malformed input —
    "1234GG" becomes 0x1234. Both twin families must check `ptr != end`.
    """
    missing = [n for n in PARSE_HELPERS if "ptr != end" not in _helper_body(n)]
    assert not missing, (
        "these parse helpers accept trailing garbage (issue 0010): "
        + ", ".join(missing)
    )


def test_hex_uint64_twins_agree_on_the_length_cap():
    """A u64 is at most 16 hex chars; both twins must reject 17, value-fits."""
    for name in ("parse_hex_uint64", "try_parse_hex_uint64"):
        assert "> 16" in _helper_body(name), (
            f"{name} lost its 16-hex-char cap; a 17-char "
            '"00000000000000001" would silently encode as 0x1'
        )


def test_json_uint64_encode_delegates_to_the_strict_path():
    """The boost::json overload must not parse for itself.

    Anchored inside UInt64Codec: every codec struct in this header declares an
    identically-shaped `encode(Serializer<Sink>&, boost::json::value const&)`,
    so an unanchored search silently matches UInt8Codec instead.
    """
    src = UINT_H.read_text()
    start = src.find("struct UInt64Codec")
    assert start != -1, "UInt64Codec not found"
    struct_src = src[start:]

    marker = "encode(Serializer<Sink>& s, boost::json::value const& v)"
    idx = struct_src.find(marker)
    assert idx != -1, "UInt64Codec json encode overload not found"
    body = struct_src[idx + len(marker):]
    body = body[: body.find("\n    }") + 6]

    assert "encode_hex(" in body, (
        "the json UInt64 encoder should delegate to encode_hex (the "
        f"errors-as-values path), got:\n{body}"
    )
    assert "parse_hex_uint64" not in body, (
        "the json UInt64 encoder parses for itself again — that is exactly the "
        "asymmetry issue 0010 removed"
    )
