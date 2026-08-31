"""Observe A-prime runtime type objects from an executing provider WASM."""

from __future__ import annotations

import json
from pathlib import Path
from typing import TypedDict

from .host import WasmHost

SCHEMA = "jshookz.runtime-type-observation.v1"


class RuntimeGlobal(TypedDict):
    name: str
    kind: str
    frozen: bool
    extensible: bool
    ordinary_object: bool
    own_has_instance: bool
    inherited_has_instance: bool
    has_instance_callable: bool
    has_instance_writable: bool | None
    has_instance_enumerable: bool | None
    has_instance_configurable: bool | None
    own_prototype: bool
    constructible: bool
    construction_throws: bool | None
    prototype_parent: str | None
    constructor_parent: str | None


class RuntimeEnumNamespace(TypedDict):
    name: str
    kind: str
    frozen: bool
    extensible: bool
    ordinary_object: bool
    own_has_instance: bool
    inherited_has_instance: bool
    own_prototype: bool
    constructible: bool
    descriptors_exact: bool
    own_keys: list[str]
    values: dict[str, int]


class RuntimeTypeObservation(TypedDict):
    schema: str
    globals: list[RuntimeGlobal]
    enum_namespaces: list[RuntimeEnumNamespace]


_OBSERVE_GLOBALS = r"""
JSON.stringify({
  schema: "jshookz.runtime-type-observation.v1",
  globals: Object.getOwnPropertyNames(globalThis).sort().map(name => {
    const prototypeNouns = [
      "STObject", "Transaction", "Payment", "LedgerEntry", "AccountRoot",
      "URIToken", "HookLedger", "HookDefinition"
    ];
    const value = globalThis[name];
    const kind = value === null ? "null" : typeof value;
    const objectLike = (kind === "object" || kind === "function");
    const descriptor = objectLike
      ? Object.getOwnPropertyDescriptor(value, Symbol.hasInstance)
      : undefined;
    let constructible = false;
    if (objectLike) {
      try {
        // Use the observed value only as newTarget. Calling it would execute
        // provider or language constructors (Date reaches the ledger clock).
        Reflect.construct(Object, [], value);
        constructible = true;
      } catch (_) {}
    }
    let constructionThrows = null;
    let prototypeParent = null;
    let constructorParent = null;
    if (prototypeNouns.includes(name)) {
      try { Reflect.construct(value, []); constructionThrows = false; }
      catch (error) { constructionThrows = error instanceof TypeError; }
      const parentPrototype = Object.getPrototypeOf(value.prototype);
      prototypeParent = parentPrototype === Object.prototype
        ? "Object"
        : prototypeNouns.find(candidate =>
            globalThis[candidate].prototype === parentPrototype) ?? null;
      const parentConstructor = Object.getPrototypeOf(value);
      constructorParent = parentConstructor === Function.prototype
        ? "Function"
        : prototypeNouns.find(candidate =>
            globalThis[candidate] === parentConstructor) ?? null;
    }
    return {
      name,
      kind,
      frozen: objectLike && Object.isFrozen(value),
      extensible: objectLike && Object.isExtensible(value),
      ordinary_object: kind === "object" &&
        Object.getPrototypeOf(value) === Object.prototype,
      own_has_instance: descriptor !== undefined,
      inherited_has_instance: objectLike && value[Symbol.hasInstance] !== undefined,
      has_instance_callable: objectLike &&
        typeof value[Symbol.hasInstance] === "function",
      has_instance_writable: descriptor === undefined ? null : descriptor.writable,
      has_instance_enumerable: descriptor === undefined ? null : descriptor.enumerable,
      has_instance_configurable: descriptor === undefined ? null : descriptor.configurable,
      own_prototype: objectLike && Object.hasOwn(value, "prototype"),
      constructible,
      construction_throws: constructionThrows,
      prototype_parent: prototypeParent,
      constructor_parent: constructorParent,
    };
  }),
  enum_namespaces: [
    "EntropyTier", "TransactionType", "LedgerEntryType", "TransactionResult",
    "HookReturnCode"
  ].filter(name => globalThis[name] !== undefined).map(name => {
    const value = globalThis[name];
    const kind = value === null ? "null" : typeof value;
    const objectLike = (kind === "object" || kind === "function");
    const ownKeys = objectLike ? Object.getOwnPropertyNames(value) : [];
    const descriptors = objectLike ? Object.getOwnPropertyDescriptors(value) : {};
    let constructible = false;
    if (objectLike) {
      try { Reflect.construct(Object, [], value); constructible = true; } catch (_) {}
    }
    return {
      name,
      kind,
      frozen: objectLike && Object.isFrozen(value),
      extensible: objectLike && Object.isExtensible(value),
      ordinary_object: kind === "object" &&
        Object.getPrototypeOf(value) === Object.prototype,
      own_has_instance: objectLike && Object.hasOwn(value, Symbol.hasInstance),
      inherited_has_instance: objectLike && value[Symbol.hasInstance] !== undefined,
      own_prototype: objectLike && Object.hasOwn(value, "prototype"),
      constructible,
      descriptors_exact: ownKeys.every(key => {
        const descriptor = descriptors[key];
        return descriptor.enumerable && !descriptor.writable &&
          !descriptor.configurable && Object.hasOwn(descriptor, "value");
      }),
      own_keys: ownKeys,
      values: Object.fromEntries(ownKeys.map(key => [key, value[key]])),
    };
  }),
})
""".strip()


def observe_runtime_types(wasm_path: str | Path) -> RuntimeTypeObservation:
    """Inspect one built provider; the declaration and policy are not inputs."""
    host = WasmHost(wasm_path=Path(wasm_path))
    host.init()
    try:
        result = host.eval(_OBSERVE_GLOBALS)
    finally:
        host.destroy()
    if not result.ok or result.result_value is None:
        raise RuntimeError(result.error or "provider runtime observation failed")
    value = json.loads(result.result_value)
    if not isinstance(value, dict) or value.get("schema") != SCHEMA:
        raise ValueError("provider returned an invalid runtime-type observation")
    rows = value.get("globals")
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
        raise ValueError("provider returned invalid runtime global rows")
    enum_rows = value.get("enum_namespaces")
    if not isinstance(enum_rows, list) or any(
        not isinstance(row, dict) for row in enum_rows
    ):
        raise ValueError("provider returned invalid runtime enum-namespace rows")
    return value
