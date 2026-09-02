"""Export one sealed provider product as the directory xahaud consumes.

The bundle is the sealed wasm, its runtime-profile manifest, the native ABI
snapshot, the seven API artifacts, one flat receipt, and one preprojected C++
values file. xahaud reads the receipt with `file(STRINGS)`, verifies every
pinned digest with `file(SHA256)`, compiles the values file as it is, and
embeds the wasm bytes itself. Nothing on the consumer's build path parses
JSON or runs Python; every coherence check runs here, before the receipt is
derived, and fails closed.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import paths


RECEIPT_SCHEMA = "jshookz.provider-receipt.v1"
MANIFEST_SCHEMA = "xahau.quickjs.runtime-profile-lock.v1"
API_ARTIFACT_SCHEMA = "jshookz.api-artifacts.v1"

RECEIPT_FILE = "jshookz_provider.receipt"
VALUES_FILE = "jshookz_provider.values.cpp"
WASM_FILE = "jshookz_provider.wasm"
MANIFEST_FILE = "jshookz_provider.manifest.json"
NATIVE_ABI_FILE = "jshookz_provider.native-abi.json"
API_MANIFEST_FILE = "api-artifacts.json"
SEALED_FILES = (WASM_FILE, MANIFEST_FILE, NATIVE_ABI_FILE)
# Files the previous consumer contract carried; an export removes them from
# its destination so a pin directory never holds two generations.
RETIRED_FILES = ("jshookz_provider.lock.json", "jshookz_provider.manifest.cmake")

# bundle basename -> (tracked source, identity label)
API_ARTIFACTS: dict[str, tuple[Path, str]] = {
    API_MANIFEST_FILE: (paths.API_ARTIFACT_MANIFEST, "api_artifact_manifest"),
    "hooks-api.d.ts": (paths.CANONICAL_HOOKS_API_DECLARATIONS, "broad_declaration"),
    "xahau-quickjs-v1-consensus-entropy.d.ts": (
        paths.XAHAU_V1_CONSENSUS_ENTROPY_HOOKS_API_DECLARATIONS,
        "entropy_declaration",
    ),
    "xahau-quickjs-v1-consensus-entropy.surface.json": (
        paths.XAHAU_V1_CONSENSUS_ENTROPY_JAVASCRIPT_SURFACE,
        "entropy_surface",
    ),
    "xahau-quickjs-v1.d.ts": (
        paths.XAHAU_V1_HOOKS_API_DECLARATIONS,
        "exact_v1_declaration",
    ),
    "xahau-quickjs-v1.surface.json": (
        paths.XAHAU_V1_JAVASCRIPT_SURFACE,
        "selected_surface",
    ),
    "xfl-profile-ledger.ts": (paths.XAHAU_XFL_PROFILE_LEDGER, "xfl_profile_ledger"),
}
BUNDLE_FILES = frozenset({*SEALED_FILES, *API_ARTIFACTS, RECEIPT_FILE, VALUES_FILE})

WASM_VALTYPE = {"i32", "i64"}
WASM_BYTE = {"i32": "0x7f", "i64": "0x7e"}
WASM_PAGE_BYTES = 65536
IMPORT_KEYS = frozenset({"module", "name", "params", "results"})
FUNCTION_EXPORT_KEYS = frozenset({"kind", "name", "params", "results"})
MEMORY_EXPORT_KEYS = frozenset(
    {"kind", "name", "minimum_pages", "maximum_pages", "memory64", "shared"}
)
LIMIT_INTS = (
    "host_work_base_per_call",
    "host_work_budget",
    "host_work_per_addressed_byte",
    "quickjs_heap_bytes",
    "quickjs_stack_bytes",
    "serialized_object_max_bytes",
    "serialized_object_max_depth",
    "serialized_object_max_fields",
    "serialized_object_max_scopes",
    "wasmtime_fuel_per_initialization",
    "wasmtime_fuel_per_invocation",
)
MODULE_VALIDATION_KEYS = (
    "layout_version",
    "failure_sentinel",
    "main_bit",
    "callback_bit",
    "entry_mask",
    "reserved_mask",
    "profile_mask",
    "profile_shift",
    "version_mask",
    "version_shift",
)


class BundleError(ValueError):
    """A sealed product failed a fail-closed coherence check before export."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _hex_identity(value: object, label: str) -> str:
    text = str(value).lower()
    if len(text) != 64 or any(c not in "0123456789abcdef" for c in text):
        raise BundleError(f"{label} is not a SHA-256 hex digest: {value!r}")
    return text


def _dict(value: object, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise BundleError(f"{label} is missing or not an object")
    return value


def _int(value: object, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BundleError(f"{label} is not an integer: {value!r}")
    return value


def _token(value: object, label: str) -> str:
    """A receipt value: non-empty, no whitespace, no newline."""
    text = str(value)
    if not text or any(c.isspace() for c in text):
        raise BundleError(f"{label} is not a single receipt token: {value!r}")
    return text


def _join_types(values: object, label: str) -> str:
    if not isinstance(values, list) or not all(isinstance(v, str) for v in values):
        raise BundleError(f"{label} is not a list of wasm value types: {values!r}")
    for item in values:
        if item not in WASM_VALTYPE:
            raise BundleError(f"{label} has unsupported wasm value type {item!r}")
    return ",".join(values)


def _wasm_signature(params: list[str], results: list[str]) -> list[str]:
    return [WASM_BYTE[v] for v in results] + [WASM_BYTE[v] for v in params]


def _typed_imports(rows: object, label: str) -> list[dict[str, Any]]:
    if not isinstance(rows, list) or not all(isinstance(r, dict) for r in rows):
        raise BundleError(f"{label} must be a list of typed import rows")
    names: list[str] = []
    for row in rows:
        if set(row) != IMPORT_KEYS or not isinstance(row.get("name"), str):
            raise BundleError(f"{label} row has extra or missing keys: {row!r}")
        if row["module"] != "env":
            raise BundleError(f"{label} {row['name']} is not an env import")
        _join_types(row["params"], f"{label} {row['name']} params")
        _join_types(row["results"], f"{label} {row['name']} results")
        names.append(row["name"])
    if len(names) != len(set(names)):
        raise BundleError(f"{label} contains duplicate import names")
    return rows


def _typed_exports(rows: object, label: str) -> list[dict[str, Any]]:
    if not isinstance(rows, list) or not all(isinstance(r, dict) for r in rows):
        raise BundleError(f"{label} must be a list of typed export rows")
    names: list[str] = []
    memories = 0
    for row in rows:
        kind, name = row.get("kind"), row.get("name")
        if not isinstance(name, str) or not name:
            raise BundleError(f"{label} row is missing its name: {row!r}")
        if kind == "function":
            if set(row) != FUNCTION_EXPORT_KEYS:
                raise BundleError(f"{label} function {name} has extra or missing keys")
            _join_types(row["params"], f"{label} {name} params")
            _join_types(row["results"], f"{label} {name} results")
        elif kind == "memory":
            if set(row) != MEMORY_EXPORT_KEYS:
                raise BundleError(f"{label} memory row has extra or missing keys")
            _int(row["minimum_pages"], f"{label} memory minimum_pages")
            _int(row["maximum_pages"], f"{label} memory maximum_pages")
            if row["memory64"] is not False or row["shared"] is not False:
                raise BundleError(f"{label} memory must be memory64=false shared=false")
            if name != "memory":
                raise BundleError(f"{label} memory export must be named memory")
            memories += 1
        else:
            raise BundleError(f"{label} has unsupported export kind {kind!r}")
        names.append(name)
    if len(names) != len(set(names)) or names != sorted(names):
        raise BundleError(f"{label} must be unique and sorted by export name")
    if memories != 1:
        raise BundleError(f"{label} must export exactly one memory")
    return rows


@dataclass(frozen=True)
class Projection:
    """Everything the receipt and the values file are rendered from."""

    product: str
    manifest: dict[str, Any]
    native: dict[str, Any]
    imports: list[dict[str, Any]]
    exports: list[dict[str, Any]]
    native_imports: list[dict[str, Any]]
    memory: dict[str, Any]
    identities: dict[str, str]  # API artifact identity label -> sha256
    manifest_sha256: str
    native_abi_sha256: str
    provider_sha256: str
    provider_size: int


def validate(
    manifest: dict[str, Any],
    manifest_bytes: bytes,
    native: dict[str, Any],
    native_bytes: bytes,
    wasm: bytes,
    artifacts: dict[str, bytes],
    product: str,
) -> Projection:
    """Prove the sealed JSON, the wasm, and the API artifacts agree."""
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise BundleError(f"unsupported manifest schema: {manifest.get('schema')!r}")
    source = _dict(manifest.get("source"), "manifest.source")
    if source.get("product") != product:
        raise BundleError(
            f"sealed manifest names product {source.get('product')!r}, not {product!r}"
        )
    provider = _dict(manifest.get("provider"), "manifest.provider")
    source_provider = _dict(source.get("provider"), "manifest.source.provider")

    provider_sha256 = _hex_identity(provider.get("sha256"), "provider sha256")
    provider_size = _int(provider.get("size"), "provider size")
    if _sha256(wasm) != provider_sha256 or len(wasm) != provider_size:
        raise BundleError("sealed provider does not match its manifest; rebuild")
    _hex_identity(manifest.get("bytecode_abi_id"), "bytecode ABI id")
    _hex_identity(manifest.get("runtime_profile_id"), "runtime profile id")

    imports = _typed_imports(provider.get("imports"), "provider.imports")
    if imports != _typed_imports(source_provider.get("imports"), "source imports"):
        raise BundleError("provider import signatures disagree between manifest copies")
    exports = _typed_exports(provider.get("exports"), "provider.exports")
    if exports != _typed_exports(
        source_provider.get("allowed_exports"), "allowed exports"
    ):
        raise BundleError("provider export signatures disagree between manifest copies")
    memory = next(row for row in exports if row["kind"] == "memory")

    build = _dict(provider.get("build"), "provider.build")
    source_build = _dict(source_provider.get("build"), "source.provider.build")
    # The source copy also records compiler flags; only the two consumer-facing
    # numbers must agree between the measured and the declared build.
    for key in ("wasm_stack_bytes", "wasm_memory_max_bytes"):
        if _int(build.get(key), key) != _int(source_build.get(key), f"source {key}"):
            raise BundleError(f"{key} disagrees between manifest copies")
    if build.get("wasm_memory_max_bytes") != memory["maximum_pages"] * WASM_PAGE_BYTES:
        raise BundleError("wasm_memory_max_bytes disagrees with the memory export")
    if source_provider.get("forbidden_import_modules") != ["wasi_snapshot_preview1"]:
        raise BundleError("provider must forbid exactly wasi_snapshot_preview1")

    products = _dict(native.get("products"), "native ABI products")
    native_imports = products.get(product)
    if not isinstance(native_imports, list) or native.get("selected") != native_imports:
        raise BundleError(f"native ABI does not select product {product!r}")
    native_by_name = {row.get("name"): row for row in native_imports}
    if set(native_by_name) != {row["name"] for row in imports} or len(
        native_by_name
    ) != len(imports):
        raise BundleError("provider imports and native ABI imports differ")
    for row in imports:
        expected = _wasm_signature(row["params"], row["results"])
        if native_by_name[row["name"]].get("wasm_signature") != expected:
            raise BundleError(
                f"import {row['name']} signature disagrees with native ABI"
            )
    native_source = _dict(native.get("source"), "native ABI source")
    for key in ("repository", "commit", "path"):
        if not isinstance(native_source.get(key), str):
            raise BundleError(f"native ABI source is missing {key}")
    _int(native_source.get("macro_function_count"), "macro_function_count")

    limits = _dict(source.get("limits"), "manifest.source.limits")
    for key in LIMIT_INTS:
        _int(limits.get(key), f"limits.{key}")
    _token(limits.get("host_work_meter"), "host_work_meter")
    execution = _dict(source.get("execution"), "manifest.source.execution")
    _token(execution.get("host_adapter_policy"), "host_adapter_policy")
    engine = _dict(source.get("engine"), "manifest.source.engine")
    _token(engine.get("version"), "engine.version")
    artifact = _dict(source.get("artifact"), "manifest.source.artifact")
    _int(artifact.get("envelope_version"), "envelope_version")
    _int(artifact.get("hook_api_version"), "hook_api_version")
    codes = _dict(artifact.get("xfl_arithmetic_profile_codes"), "profile codes")
    for key in ("none", "xahauFloatV1", "nearestEvenV1"):
        _int(codes.get(key), f"xfl_arithmetic_profile_codes.{key}")
    validation = _dict(
        source_provider.get("module_validation_result"), "module_validation_result"
    )
    for key in MODULE_VALIDATION_KEYS:
        _int(validation.get(key), f"module_validation_result.{key}")

    api_manifest = json.loads(artifacts[API_MANIFEST_FILE])
    if (
        not isinstance(api_manifest, dict)
        or set(api_manifest) != {"artifacts", "schema"}
        or api_manifest.get("schema") != API_ARTIFACT_SCHEMA
    ):
        raise BundleError("API artifact manifest has an unexpected shape")
    declared = _dict(api_manifest.get("artifacts"), "API artifacts")
    identities: dict[str, str] = {}
    expected_keys: set[str] = set()
    for name, (source_path, label) in API_ARTIFACTS.items():
        digest = _sha256(artifacts[name])
        identities[label] = digest
        if name == API_MANIFEST_FILE:
            continue
        relative = source_path.relative_to(paths.REPO_ROOT).as_posix()
        expected_keys.add(relative)
        if declared.get(relative) != digest:
            raise BundleError(f"API artifact {name} disagrees with its manifest digest")
    if set(declared) != expected_keys:
        raise BundleError("API artifact manifest file set disagrees with the bundle")

    surface = _dict(manifest.get("javascript_surface"), "javascript_surface")
    nested = _dict(source.get("javascript_surface"), "source.javascript_surface")
    if surface.get("declaration_sha256") != identities["exact_v1_declaration"]:
        raise BundleError("exact-v1 declaration digest disagrees with the manifest")
    if surface.get("sha256") != identities["selected_surface"]:
        raise BundleError("selected surface digest disagrees with the manifest")
    for key in ("declaration", "manifest", "schema"):
        if surface.get(key) != nested.get(key):
            raise BundleError(f"javascript_surface.{key} disagrees between copies")

    return Projection(
        product=product,
        manifest=manifest,
        native=native,
        imports=imports,
        exports=exports,
        native_imports=native_imports,
        memory=memory,
        identities=identities,
        manifest_sha256=_sha256(manifest_bytes),
        native_abi_sha256=_sha256(native_bytes),
        provider_sha256=provider_sha256,
        provider_size=provider_size,
    )


def _quote(value: str) -> str:
    if any(c in value for c in '\n\r"'):
        raise BundleError(f"invalid C++ string value: {value!r}")
    return json.dumps(value)


def _hex_bytes(value: str) -> str:
    return ", ".join(f"0x{value[i : i + 2]}" for i in range(0, 64, 2))


def _cpp_bool(value: object) -> str:
    return "true" if value is True else "false"


def _export_row(row: dict[str, Any]) -> str:
    if row["kind"] == "memory":
        fields = (
            _quote("memory"),
            _quote(row["name"]),
            _quote(""),
            _quote(""),
            f"{int(row['minimum_pages'])}U",
            f"{int(row['maximum_pages'])}U",
            _cpp_bool(row["memory64"]),
            _cpp_bool(row["shared"]),
        )
    else:
        fields = (
            _quote("function"),
            _quote(row["name"]),
            _quote(_join_types(row["params"], "export params")),
            _quote(_join_types(row["results"], "export results")),
            "0U",
            "0U",
            "false",
            "false",
        )
    return "{" + ", ".join(fields) + "}"


def render_values_cpp(p: Projection) -> str:
    """The translation unit xahaud compiles; it defines every extern in
    xahaud's QuickJSProviderProfile.h except the embedded wasm bytes."""
    manifest = p.manifest
    source = manifest["source"]
    limits = source["limits"]
    artifact = source["artifact"]
    codes = artifact["xfl_arithmetic_profile_codes"]
    validation = source["provider"]["module_validation_result"]
    native_source = p.native["source"]
    import_rows = ",\n    ".join(
        "{"
        + ", ".join(
            (
                _quote(row["module"]),
                _quote(row["name"]),
                _quote(_join_types(row["params"], "import params")),
                _quote(_join_types(row["results"], "import results")),
            )
        )
        + "}"
        for row in p.imports
    )
    native_rows = ",\n    ".join(
        "{"
        + ", ".join(
            (
                _quote(row["name"]),
                _quote(row["return_type"]),
                _quote(",".join(row["param_types"])),
                _quote(row["amendment"] or ""),
            )
        )
        + "}"
        for row in p.native_imports
    )
    export_rows = ",\n    ".join(_export_row(row) for row in p.exports)
    return f"""// Generated by jshookz build; do not edit.
// Provider: {WASM_FILE} sha256={p.provider_sha256} size={p.provider_size}
#include <xrpld/app/hook/QuickJSHookRuntime.h>
#include <xrpld/app/hook/detail/QuickJSProviderProfile.h>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace hook::artifact::generated {{

std::array<std::uint8_t, 32> const providerSHA256 = {{
    {_hex_bytes(p.provider_sha256)}}};
std::array<std::uint8_t, 32> const bytecodeABI = {{
    {_hex_bytes(manifest["bytecode_abi_id"])}}};
std::array<std::uint8_t, 32> const runtimeProfile = {{
    {_hex_bytes(manifest["runtime_profile_id"])}}};
std::string_view const providerProduct = {_quote(p.product)};
std::string_view const providerManifestSHA256 = {_quote(p.manifest_sha256)};
std::size_t const providerSize = {p.provider_size};
std::uint16_t const hookApiVersion = {artifact["hook_api_version"]};
std::uint64_t const initializationFuel = {limits["wasmtime_fuel_per_initialization"]}ULL;
std::uint64_t const invocationFuel = {limits["wasmtime_fuel_per_invocation"]}ULL;
std::string_view const hostWorkMeter = {_quote(limits["host_work_meter"])};
std::uint64_t const hostWorkBudget = {limits["host_work_budget"]}ULL;
std::uint64_t const hostWorkBasePerCall = {limits["host_work_base_per_call"]}ULL;
std::uint64_t const hostWorkPerAddressedByte =
    {limits["host_work_per_addressed_byte"]}ULL;
std::string_view const hostAdapterPolicy = {_quote(source["execution"]["host_adapter_policy"])};
std::uint32_t const heapBytes = {limits["quickjs_heap_bytes"]}U;
std::uint32_t const stackBytes = {limits["quickjs_stack_bytes"]}U;
std::uint32_t const wasmStackBytes = {manifest["provider"]["build"]["wasm_stack_bytes"]}U;
std::uint32_t const serializedObjectMaxBytes =
    {limits["serialized_object_max_bytes"]}U;
std::uint32_t const serializedObjectMaxFields =
    {limits["serialized_object_max_fields"]}U;
std::uint32_t const serializedObjectMaxScopes =
    {limits["serialized_object_max_scopes"]}U;
std::uint32_t const serializedObjectMaxDepth =
    {limits["serialized_object_max_depth"]}U;
std::uint32_t const providerMemoryMinimumPages = {int(p.memory["minimum_pages"])}U;
std::uint32_t const providerMemoryMaximumPages = {int(p.memory["maximum_pages"])}U;
bool const providerMemory64 = false;
bool const providerMemoryShared = false;
std::string_view const javascriptBroadDeclarationSHA256 =
    {_quote(p.identities["broad_declaration"])};
std::string_view const javascriptExactV1DeclarationSHA256 =
    {_quote(p.identities["exact_v1_declaration"])};
std::string_view const javascriptSurfaceSHA256 = {_quote(p.identities["selected_surface"])};
std::string_view const javascriptXFLProfileLedgerSHA256 =
    {_quote(p.identities["xfl_profile_ledger"])};
std::string_view const javascriptAPIArtifactManifestSHA256 =
    {_quote(p.identities["api_artifact_manifest"])};
std::uint8_t const xqjsEnvelopeVersion = {artifact["envelope_version"]}U;
std::uint16_t const xflArithmeticProfileNone =
    {codes["none"]}U;
std::uint16_t const xflArithmeticProfileXahauFloatV1 =
    {codes["xahauFloatV1"]}U;
std::uint16_t const xflArithmeticProfileNearestEvenV1 =
    {codes["nearestEvenV1"]}U;
std::uint32_t const moduleValidationLayoutVersion =
    {validation["layout_version"]}U;
std::int32_t const moduleValidationFailureSentinel =
    {validation["failure_sentinel"]};
std::uint32_t const moduleValidationMainBit =
    {validation["main_bit"]}U;
std::uint32_t const moduleValidationCallbackBit =
    {validation["callback_bit"]}U;
std::uint32_t const moduleValidationEntryMask =
    {validation["entry_mask"]}U;
std::uint32_t const moduleValidationReservedMask =
    {validation["reserved_mask"]}U;
std::uint32_t const moduleValidationProfileMask =
    {validation["profile_mask"]}U;
std::uint32_t const moduleValidationProfileShift =
    {validation["profile_shift"]}U;
std::uint32_t const moduleValidationVersionMask =
    {validation["version_mask"]}U;
std::uint32_t const moduleValidationVersionShift =
    {validation["version_shift"]}U;

namespace {{

std::string_view const providerImportNames[] = {{
    {", ".join(_quote(row["name"]) for row in p.imports)}}};
std::string_view const providerExportNames[] = {{
    {", ".join(_quote(row["name"]) for row in p.exports)}}};
ProviderImportSignature const providerImportSignatureData[] = {{
    {import_rows}}};
ProviderExportSignature const providerExportSignatureData[] = {{
    {export_rows}}};
NativeImportSignature const nativeImportSignatureData[] = {{
    {native_rows}}};

}}  // namespace

std::span<std::string_view const> const providerImports{{providerImportNames}};
std::span<std::string_view const> const providerExports{{providerExportNames}};
std::span<ProviderImportSignature const> const providerImportSignatures{{
    providerImportSignatureData}};
std::span<ProviderExportSignature const> const providerExportSignatures{{
    providerExportSignatureData}};
std::span<NativeImportSignature const> const nativeImportSignatures{{
    nativeImportSignatureData}};
std::string_view const nativeABISourceRepository = {_quote(native_source["repository"])};
std::string_view const nativeABISourceCommit = {_quote(native_source["commit"])};
std::string_view const nativeABISourcePath = {_quote(native_source["path"])};
std::string_view const nativeABISHA256 = {_quote(p.native_abi_sha256)};
std::size_t const nativeABICatalogueCount = {native_source["macro_function_count"]};

}}  // namespace hook::artifact::generated
"""


def receipt_values(p: Projection, values_sha256: str) -> dict[str, str]:
    """Every pin xahaud needs to admit and run the provider, nothing else."""
    manifest = p.manifest
    source = manifest["source"]
    limits = source["limits"]
    values = {
        "api_artifacts_file": API_MANIFEST_FILE,
        "api_artifacts_sha256": p.identities["api_artifact_manifest"],
        "broad_declaration_sha256": p.identities["broad_declaration"],
        "bytecode_abi_id": manifest["bytecode_abi_id"],
        "exact_v1_declaration_sha256": p.identities["exact_v1_declaration"],
        "heap_bytes": limits["quickjs_heap_bytes"],
        "hook_api_version": source["artifact"]["hook_api_version"],
        "host_adapter_policy": source["execution"]["host_adapter_policy"],
        "host_work_base_per_call": limits["host_work_base_per_call"],
        "host_work_budget": limits["host_work_budget"],
        "host_work_meter": limits["host_work_meter"],
        "host_work_per_addressed_byte": limits["host_work_per_addressed_byte"],
        "initialization_fuel": limits["wasmtime_fuel_per_initialization"],
        "invocation_fuel": limits["wasmtime_fuel_per_invocation"],
        "manifest_file": MANIFEST_FILE,
        "manifest_schema": manifest["schema"],
        "manifest_sha256": p.manifest_sha256,
        "native_abi_file": NATIVE_ABI_FILE,
        "native_abi_sha256": p.native_abi_sha256,
        "product": p.product,
        "provider_export_count": len(p.exports),
        "provider_file": WASM_FILE,
        "provider_import_count": len(p.imports),
        "provider_memory_maximum_pages": p.memory["maximum_pages"],
        "provider_memory_minimum_pages": p.memory["minimum_pages"],
        "provider_sha256": p.provider_sha256,
        "provider_size": p.provider_size,
        "runtime_profile_id": manifest["runtime_profile_id"],
        "schema": RECEIPT_SCHEMA,
        "selected_surface_sha256": p.identities["selected_surface"],
        "serialized_object_max_bytes": limits["serialized_object_max_bytes"],
        "serialized_object_max_depth": limits["serialized_object_max_depth"],
        "serialized_object_max_fields": limits["serialized_object_max_fields"],
        "serialized_object_max_scopes": limits["serialized_object_max_scopes"],
        "stack_bytes": limits["quickjs_stack_bytes"],
        "values_file": VALUES_FILE,
        "values_sha256": values_sha256,
        "wasm_stack_bytes": manifest["provider"]["build"]["wasm_stack_bytes"],
        "wasmtime_version": source["engine"]["version"],
        "xfl_profile_ledger_sha256": p.identities["xfl_profile_ledger"],
    }
    return {key: _token(value, key) for key, value in values.items()}


def render_receipt(values: dict[str, str]) -> str:
    """One `key value` per line, keys sorted, nothing else."""
    lines = []
    for key in sorted(values):
        if not RECEIPT_KEY.match(key) or not RECEIPT_VALUE.match(values[key]):
            raise BundleError(f"invalid receipt line: {key!r} {values[key]!r}")
        lines.append(f"{key} {values[key]}")
    return "\n".join(lines) + "\n"


# One grammar governs both sides: a key is [a-z0-9_]+, a value is one run of
# non-whitespace, a line is exactly `key value`, the file is newline-terminated
# with no blank lines, no comments, no quoting, and no repeated keys.
RECEIPT_KEY = re.compile(r"[a-z0-9_]+\Z")
RECEIPT_VALUE = re.compile(r"\S+\Z")


def parse_receipt(text: str) -> dict[str, str]:
    """The consumer's reading of the receipt, kept here so tests prove it."""
    values: dict[str, str] = {}
    if not text.endswith("\n") or "\n\n" in text:
        raise BundleError("receipt must be newline-terminated lines without blanks")
    for number, line in enumerate(text.split("\n")[:-1], start=1):
        key, sep, value = line.partition(" ")
        if not sep or not RECEIPT_KEY.match(key) or not RECEIPT_VALUE.match(value):
            raise BundleError(f"receipt line {number} is not `key value`: {line!r}")
        if key in values:
            raise BundleError(f"receipt repeats key {key!r}")
        values[key] = value
    return values


def export(product: str, destination: Path | None = None) -> Path:
    """Validate one sealed product and write its consumer bundle.

    Nothing is rebuilt or resealed; a stale or incoherent seal fails closed
    before any file is written.
    """
    from .build import PRODUCTS

    try:
        selected = PRODUCTS[product]
    except KeyError as error:
        raise ValueError(f"unknown provider product: {product!r}") from error
    target = Path(destination).resolve() if destination else selected.bundle_dir
    sources: dict[str, Path] = {
        name: selected.build_dir / name for name in SEALED_FILES
    }
    sources.update({name: path for name, (path, _) in API_ARTIFACTS.items()})
    missing = [str(path) for path in sources.values() if not path.is_file()]
    if missing:
        raise RuntimeError(
            f"incomplete {product} build; run `jshookz build {product}` first. "
            f"Missing: {', '.join(missing)}"
        )
    contents = {name: path.read_bytes() for name, path in sources.items()}
    try:
        projection = validate(
            manifest=json.loads(contents[MANIFEST_FILE]),
            manifest_bytes=contents[MANIFEST_FILE],
            native=json.loads(contents[NATIVE_ABI_FILE]),
            native_bytes=contents[NATIVE_ABI_FILE],
            wasm=contents[WASM_FILE],
            artifacts={name: contents[name] for name in API_ARTIFACTS},
            product=product,
        )
    except (BundleError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot export {product}: {error}") from error
    values_cpp = render_values_cpp(projection).encode()
    receipt = render_receipt(receipt_values(projection, _sha256(values_cpp)))

    # The destination is exactly the bundle. Anything else there is either a
    # retired contract file, which is removed, or a stranger, which is refused
    # so a stale or mistyped file can never ride along into a pin or a release.
    if target.exists():
        if not target.is_dir():
            raise RuntimeError(f"bundle destination is not a directory: {target}")
        strangers = sorted(
            entry.name
            for entry in target.iterdir()
            if entry.name not in BUNDLE_FILES and entry.name not in RETIRED_FILES
        )
        if strangers:
            raise RuntimeError(
                f"bundle destination {target} holds files outside the bundle: "
                f"{', '.join(strangers)}; remove them or export elsewhere"
            )
    target.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        shutil.copyfile(source, target / name)
    (target / VALUES_FILE).write_bytes(values_cpp)
    (target / RECEIPT_FILE).write_text(receipt)
    for name in RETIRED_FILES:
        (target / name).unlink(missing_ok=True)
    print(f"✓ Exported consumer bundle {target}")
    return target
