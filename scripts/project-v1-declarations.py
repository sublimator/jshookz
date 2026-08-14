#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["tree-sitter>=0.23", "tree-sitter-typescript>=0.23"]
# ///
"""Project the exact xahau-quickjs-v1 declarations from the canonical API.

The broad public declaration is the source of names, enum values, and every
signature that v1 implements without alteration.  PROFILE below records only
selection, runtime names, and the handful of intentional v1 refinements.  The
generated file is package data and the TypeScript compiler's safe default.

Usage:
    ./scripts/project-v1-declarations.py          # update generated output
    ./scripts/project-v1-declarations.py --check  # fail if output is stale
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import tree_sitter_typescript
from tree_sitter import Language, Node, Parser

ROOT = Path(__file__).resolve().parents[1]
CANONICAL = ROOT / "packages/jshookz/src/jshookz/types/hooks-api.d.ts"
OUTPUT = ROOT / "packages/jshookz/src/jshookz/types/xahau-quickjs-v1.d.ts"

HEADER = """/**
 * Generated from hooks-api.d.ts by scripts/project-v1-declarations.py.
 *
 * Exact JavaScript surface implemented by the sealed xahau-quickjs-v1
 * provider. This deliberately narrow declaration is the compiler default.
 * Edit the canonical declaration or PROFILE in the generator, not this file.
 */
"""

# The provider accepts concrete typed arrays, ArrayBuffer, and strings.  The
# canonical future API deliberately accepts a broader BytesLike vocabulary.
PROFILE_PREAMBLE = """
type HookTypedArray =
  | Int8Array
  | Uint8Array
  | Uint8ClampedArray
  | Int16Array
  | Uint16Array
  | Int32Array
  | Uint32Array
  | Float32Array
  | Float64Array
  | BigInt64Array
  | BigUint64Array;

type BytesLike =
  | HookTypedArray
  | ArrayBuffer
  | string;

type HookReturnCode = number;
"""


@dataclass(frozen=True)
class ProfileOverride:
    canonical_sha256: str
    profile: str


@dataclass(frozen=True)
class ContainerProjection:
    source: str
    output: str
    members: tuple[str, ...]
    overrides: dict[str, ProfileOverride] = field(default_factory=dict)
    kind: str = "class"


PROFILE = (
    ContainerProjection(
        "STBlob",
        "STBlob",
        ("byteLength", "from", "byteAt", "toBytes", "toHex", "equals"),
    ),
    ContainerProjection(
        "STHash",
        "Hash256",
        ("from", "toHex", "toBytes", "isZero", "equals"),
        overrides={
            "equals": ProfileOverride(
                "1f408ec4301e13611e226182d446fc8dea4d74ea6faa023df4d8ea35d1d3e233",
                "equals(other: Hash256): boolean;",
            )
        },
    ),
    ContainerProjection(
        "STAddress",
        "AccountID",
        ("from", "toHex", "toBytes"),
        overrides={
            "from": ProfileOverride(
                "820400f272131c3f8daf6e4ab2aa50b5ba557d18be53aeed79517bb87127f0b3",
                "static from(value: BytesLike): AccountID;",
            )
        },
    ),
    ContainerProjection(
        "XFL",
        "XFL",
        ("raw", "fromRaw", "mantissa", "exponent", "isNegative", "isZero"),
        overrides={
            "fromRaw": ProfileOverride(
                "9c4cffcd1d3857c18a95bdb09be7063bb0c7c7352828a7547536db47d32da363",
                "static fromRaw(raw: bigint | number): XFL;",
            ),
            "mantissa": ProfileOverride(
                "289e93464c0f190aba9c34a5947d42583b5ea601e6c3d44fbf28da001ced6895",
                "mantissa(): number;",
            ),
        },
    ),
    ContainerProjection(
        "lifecycle",
        "lifecycle",
        ("account",),
        kind="namespace",
    ),
    ContainerProjection(
        "ledger",
        "ledger",
        ("sequence", "lastTime", "lastHash"),
        kind="namespace",
    ),
    ContainerProjection("otxn", "otxn", ("type",), kind="namespace"),
    ContainerProjection(
        "state",
        "state",
        ("get", "set"),
        overrides={
            "get": ProfileOverride(
                "8d137fe7efe37dbd93f2e6a92ee091d01f84efe1c3122e1ee68c3ccf7b9bd3b5",
                "function get(key: BytesLike | STBlob | Hash256 | AccountID): HostResult<STBlob | undefined>;",
            ),
            "set": ProfileOverride(
                "a2e1bd4a85de52e6d01741f2b6e36a037de2f060f05aafe07cd10a074286c2fc",
                """function set(
  key: BytesLike | STBlob | Hash256 | AccountID,
  value: BytesLike | STBlob | Hash256 | AccountID,
): HostResult<void>;""",
            ),
        },
        kind="namespace",
    ),
    ContainerProjection(
        "emit",
        "emit",
        ("reserve", "prepare", "tx"),
        overrides={
            "prepare": ProfileOverride(
                "e1ee8d216002cb0be08af85683c67c9f985f2a9213c2bb3169cf21c426f8e477",
                "function prepare(transaction: BytesLike | STBlob): HostResult<STBlob>;",
            ),
            "tx": ProfileOverride(
                "8162619549d58dbdf5f6bcc6ff34a1732bad41a8bd0e6a6885d0b89be778b0be",
                "function tx(transaction: BytesLike | STBlob): HostResult<Hash256>;",
            ),
        },
        kind="namespace",
    ),
    ContainerProjection(
        "rollback",
        "rollback",
        ("onHostFailure",),
        kind="namespace",
    ),
)

TOP_LEVEL = ("HostSuccess", "HostFailure", "HostResult", "TransactionType")
GLOBAL_FUNCTIONS = ("accept", "rollback", "trace")
GLOBAL_OVERRIDES = {
    "accept": ProfileOverride(
        "2b0d4e0e626ee0f53a682a593a8ced5afdb4763e56d6cfc2fa6e4b03ebfaba1e",
        """declare function accept(
  message?: string | Uint8Array | ArrayBuffer,
  code?: number,
): never;""",
    ),
    "rollback": ProfileOverride(
        "a60dbf9df833cff68c19e185e1af3fc4bd70a1105b84938630420b3d319734cc",
        """declare function rollback(
  message?: string | Uint8Array | ArrayBuffer,
  code?: number,
): never;""",
    ),
}

PROFILE_ROOT_LOCKS = {
    "BytesLike": "785c42b0e7d524dceeb2dd77535c890d14ac5a127f9799cc0e6cae51dabc37a1",
    "HookReturnCode": "03e0b4e56bc1d8c638f261d2274dd6a9d9247688166d02136da138b78602a35a",
}


class CanonicalModel:
    def __init__(self, source: str):
        self.source = source
        self.raw = source.encode()
        parser = Parser(Language(tree_sitter_typescript.language_typescript()))
        self.tree = parser.parse(self.raw)
        if self.tree.root_node.has_error:
            raise ValueError(f"{CANONICAL}: TypeScript parse error")
        self.declarations: dict[str, Node] = {}
        self.functions: dict[str, Node] = {}
        for child in self.tree.root_node.named_children:
            inner = self._unwrap(child)
            name = self._name(inner)
            if name is None:
                continue
            if inner.type in {"function_declaration", "function_signature"}:
                self.functions[name] = inner
            else:
                self.declarations[name] = inner

    def text(self, node: Node) -> str:
        return self.raw[node.start_byte : node.end_byte].decode()

    @staticmethod
    def _unwrap(node: Node) -> Node:
        while node.type in {"ambient_declaration", "export_statement"}:
            nested = [child for child in node.named_children if child.type != "comment"]
            if not nested:
                break
            node = nested[-1]
        return node

    def _name(self, node: Node) -> str | None:
        name = node.child_by_field_name("name")
        if name is not None:
            return self.text(name)
        for child in node.named_children:
            if child.type == "variable_declarator":
                name = child.child_by_field_name("name")
                if name is not None:
                    return self.text(name)
        return None

    def declaration(self, name: str) -> str:
        try:
            return self.text(self.declarations[name])
        except KeyError as exc:
            raise ValueError(f"canonical declaration {name!r} is missing") from exc

    def function(self, name: str) -> str:
        try:
            return self.text(self.functions[name])
        except KeyError as exc:
            raise ValueError(f"canonical function {name!r} is missing") from exc

    def members(self, name: str) -> dict[str, str]:
        try:
            declaration = self.declarations[name]
        except KeyError as exc:
            raise ValueError(f"canonical container {name!r} is missing") from exc
        body = declaration.child_by_field_name("body")
        if body is None:
            body = next(
                (child for child in declaration.named_children if child.type in {"class_body", "interface_body", "statement_block"}),
                None,
            )
        if body is None:
            raise ValueError(f"canonical container {name!r} has no body")
        result: dict[str, str] = {}
        for child in body.named_children:
            inner = self._unwrap(child)
            member_name = self._name(inner)
            if member_name is not None:
                result[member_name] = self.text(inner)
        return result


def _replace_profile_names(text: str) -> str:
    text = re.sub(r"\bSTHash(?:<[^>]+>)?", "Hash256", text)
    text = re.sub(r"\bSTAddress\b", "AccountID", text)
    text = re.sub(r"\bHexString\b", "string", text)
    text = re.sub(r"\bLedgerSequence\b", "number", text)
    text = re.sub(r"\bRippleTime\b", "number", text)
    return text


def _indent(text: str) -> str:
    return "\n".join("  " + line if line else line for line in text.splitlines())


def _terminated(text: str) -> str:
    text = text.rstrip()
    return text if text.endswith((";", "}")) else text + ";"


def _assert_lock(label: str, canonical: str, expected: str) -> None:
    actual = hashlib.sha256(canonical.encode()).hexdigest()
    if actual != expected:
        raise ValueError(
            f"{label}: canonical declaration changed ({actual}); review the "
            "profile refinement before updating its lock"
        )


def _render_container(model: CanonicalModel, projection: ContainerProjection) -> str:
    available = model.members(projection.source)
    requested = set(projection.members)
    unknown_overrides = set(projection.overrides) - requested
    if unknown_overrides:
        raise ValueError(
            f"{projection.output}: overrides for unselected members "
            f"{sorted(unknown_overrides)}"
        )
    rendered: list[str] = []
    for member in projection.members:
        if member not in available:
            raise ValueError(
                f"{projection.source}.{member} is absent from canonical declaration"
            )
        value = available[member]
        override = projection.overrides.get(member)
        if override is not None:
            _assert_lock(
                f"{projection.source}.{member}",
                _terminated(value),
                override.canonical_sha256,
            )
            value = override.profile
        rendered.append(_indent(_terminated(_replace_profile_names(value))))
    return (
        f"declare {projection.kind} {projection.output} {{\n"
        + "\n".join(rendered)
        + "\n}"
    )


def render() -> str:
    model = CanonicalModel(CANONICAL.read_text())
    for name, expected in PROFILE_ROOT_LOCKS.items():
        _assert_lock(name, model.declaration(name), expected)
    parts = [HEADER.rstrip(), PROFILE_PREAMBLE.strip()]
    for name in TOP_LEVEL:
        value = _replace_profile_names(model.declaration(name))
        if value.startswith("const enum "):
            value = "declare " + value
        parts.append(value)
    for projection in PROFILE:
        parts.append(_render_container(model, projection))
    parts.append("/** Core Hook terminals and tracing. */")
    for name in GLOBAL_FUNCTIONS:
        value = model.function(name)
        override = GLOBAL_OVERRIDES.get(name)
        if override is not None:
            _assert_lock(
                f"global {name}",
                _terminated(value),
                override.canonical_sha256,
            )
            value = override.profile
        value = _replace_profile_names(value)
        if not value.startswith("declare "):
            value = "declare " + value
        parts.append(value)
    return "\n\n".join(parts).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    expected = render()
    current = OUTPUT.read_text() if OUTPUT.exists() else ""
    if args.check:
        if current == expected:
            print(f"{OUTPUT.relative_to(ROOT)}: current")
            return 0
        sys.stderr.writelines(
            difflib.unified_diff(
                current.splitlines(keepends=True),
                expected.splitlines(keepends=True),
                fromfile=str(OUTPUT.relative_to(ROOT)),
                tofile="generated",
            )
        )
        return 1
    if current != expected:
        OUTPUT.write_text(expected)
        print(f"updated {OUTPUT.relative_to(ROOT)}")
    else:
        print(f"{OUTPUT.relative_to(ROOT)}: already current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
