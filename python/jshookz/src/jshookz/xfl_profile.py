"""Typed XFL arithmetic-profile and provider-validation metadata."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from . import _runtime_profile_constants as generated


class XFLArithmeticProfile(str, Enum):
    NONE = "none"
    XAHAU_FLOAT_V1 = "xahauFloatV1"
    NEAREST_EVEN_V1 = "nearestEvenV1"


XQJS_ARITHMETIC_PROFILE_NONE = generated.XFL_ARITHMETIC_PROFILE_NONE
XQJS_ARITHMETIC_PROFILE_XAHAU_FLOAT_V1 = (
    generated.XFL_ARITHMETIC_PROFILE_XAHAU_FLOAT_V1
)
XQJS_ARITHMETIC_PROFILE_NEAREST_EVEN_V1 = (
    generated.XFL_ARITHMETIC_PROFILE_NEAREST_EVEN_V1
)

_PROFILE_BY_CODE = {
    XQJS_ARITHMETIC_PROFILE_NONE: XFLArithmeticProfile.NONE,
    XQJS_ARITHMETIC_PROFILE_XAHAU_FLOAT_V1: XFLArithmeticProfile.XAHAU_FLOAT_V1,
    XQJS_ARITHMETIC_PROFILE_NEAREST_EVEN_V1: (
        XFLArithmeticProfile.NEAREST_EVEN_V1
    ),
}
_CODE_BY_PROFILE = {profile: code for code, profile in _PROFILE_BY_CODE.items()}


def xfl_profile_code(profile: XFLArithmeticProfile) -> int:
    """Return the generated wire/provider code for a named profile."""
    try:
        return _CODE_BY_PROFILE[profile]
    except KeyError as error:
        raise ValueError(f"unsupported XFL arithmetic profile: {profile!r}") from error


def xfl_profile_from_code(code: int) -> XFLArithmeticProfile:
    """Decode one generated wire/provider code, rejecting unknown values."""
    try:
        return _PROFILE_BY_CODE[code]
    except KeyError as error:
        raise ValueError(f"unknown XFL arithmetic profile code {code}") from error


@dataclass(frozen=True)
class ModuleValidationMetadata:
    """Decoded positive result from ``qjs_validate_hook_module``."""

    has_callback: bool
    profile: XFLArithmeticProfile


def decode_module_validation_result(word: int) -> ModuleValidationMetadata:
    """Decode the exact generated module-validation result ABI v1."""
    if not isinstance(word, int) or isinstance(word, bool):
        raise ValueError("module-validation result must be an i32")
    if word <= 0 or word > 0x7FFFFFFF:
        raise ValueError(f"invalid module-validation success word {word}")
    if word & generated.MODULE_VALIDATION_RESERVED_MASK:
        raise ValueError(f"module-validation result has reserved bits: 0x{word:08x}")

    version = (
        word & generated.MODULE_VALIDATION_VERSION_MASK
    ) >> generated.MODULE_VALIDATION_VERSION_SHIFT
    if version != generated.MODULE_VALIDATION_LAYOUT_VERSION:
        raise ValueError(f"unsupported module-validation layout version {version}")

    entries = word & generated.MODULE_VALIDATION_ENTRY_MASK
    if not entries & generated.MODULE_VALIDATION_MAIN_BIT:
        raise ValueError("module-validation result has no callable main entry")
    if entries not in {
        generated.MODULE_VALIDATION_MAIN_BIT,
        generated.MODULE_VALIDATION_MAIN_BIT
        | generated.MODULE_VALIDATION_CALLBACK_BIT,
    }:
        raise ValueError(f"invalid module-validation entry bits {entries}")

    code = (
        word & generated.MODULE_VALIDATION_PROFILE_MASK
    ) >> generated.MODULE_VALIDATION_PROFILE_SHIFT
    return ModuleValidationMetadata(
        has_callback=bool(entries & generated.MODULE_VALIDATION_CALLBACK_BIT),
        profile=xfl_profile_from_code(code),
    )
