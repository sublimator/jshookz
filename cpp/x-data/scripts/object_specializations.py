"""One product-owned model for typed serialized-object specialization.

The provider generator and declaration tooling both consume this module.  The
policy selects public families/leaves and exceptional semantics; canonical
field membership, requiredness, discriminator values, and codes stay in the
vendored definitions JSON.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from typing import Any

OPTIONALITY = {0: "required", 1: "optional", 2: "optional"}


@dataclass(frozen=True)
class FormatField:
    name: str
    presence: str


@dataclass(frozen=True)
class ObjectFormat:
    name: str
    type_code: int
    fields: tuple[FormatField, ...]

    @property
    def required(self) -> tuple[str, ...]:
        return tuple(field.name for field in self.fields if field.presence == "required")

    @property
    def allowed(self) -> tuple[str, ...]:
        return tuple(field.name for field in self.fields)


@dataclass(frozen=True)
class ObjectFamily:
    name: str
    discriminator: str
    common: tuple[FormatField, ...]
    formats: tuple[ObjectFormat, ...]
    leaves: tuple[str, ...]
    defaults: tuple[tuple[str, str], ...]

    def format(self, name: str) -> ObjectFormat:
        for value in self.formats:
            if value.name == name:
                return value
        raise KeyError(name)


@dataclass(frozen=True)
class SpecializationModel:
    families: tuple[ObjectFamily, ...]
    refinements: tuple[tuple[str, tuple[tuple[str, str], ...]], ...]
    digest: str

    def family(self, name: str) -> ObjectFamily:
        for value in self.families:
            if value.name == name:
                return value
        raise KeyError(name)

    def refinement_map(self) -> dict[str, dict[str, str]]:
        return {leaf: dict(fields) for leaf, fields in self.refinements}


def _format_fields(value: object, *, owner: str) -> tuple[FormatField, ...]:
    if not isinstance(value, list):
        raise ValueError(f"{owner} must be a field list")
    fields: list[FormatField] = []
    seen: set[str] = set()
    for item in value:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            raise ValueError(f"{owner} has an invalid field")
        name = item["name"]
        optionality = item.get("optionality")
        if optionality not in OPTIONALITY:
            raise ValueError(f"{owner}.{name} optionality must be 0, 1, or 2")
        if name in seen:
            raise ValueError(f"{owner} duplicates field {name}")
        seen.add(name)
        fields.append(FormatField(name, OPTIONALITY[optionality]))
    return tuple(fields)


def format_table(
    definitions: dict[str, Any], table_name: str
) -> tuple[tuple[FormatField, ...], dict[str, tuple[FormatField, ...]]]:
    table = definitions.get(table_name)
    if not isinstance(table, dict):
        raise ValueError(f"definitions have no {table_name} object")
    common = _format_fields(table.get("common", []), owner=f"{table_name}.common")
    formats = {
        name: _format_fields(fields, owner=f"{table_name}.{name}")
        for name, fields in table.items()
        if name != "common"
    }
    return common, formats


def _string_map(value: object, *, owner: str) -> dict[str, str]:
    if value is None:
        return {}
    if not isinstance(value, dict) or not all(
        isinstance(key, str) and isinstance(item, str)
        for key, item in value.items()
    ):
        raise ValueError(f"{owner} must be a string map")
    return dict(value)


def load_model(
    definitions: dict[str, Any], policy: dict[str, Any]
) -> SpecializationModel:
    selected = policy.get("object_specializations")
    if not isinstance(selected, dict) or not isinstance(selected.get("families"), list):
        raise ValueError("provider policy has no object_specializations.families")

    serialized_names = {
        row[0]
        for row in definitions.get("FIELDS", [])
        if isinstance(row, list)
        and len(row) == 2
        and isinstance(row[0], str)
        and isinstance(row[1], dict)
        and row[1].get("isSerialized") is True
    }
    families: list[ObjectFamily] = []
    selected_leaves: set[str] = set()
    canonical_families: list[dict[str, object]] = []
    for raw in selected["families"]:
        if not isinstance(raw, dict):
            raise ValueError("object specialization family must be an object")
        name = raw.get("name")
        discriminator = raw.get("discriminator")
        type_table_name = raw.get("type_table")
        format_table_name = raw.get("format_table")
        leaves = raw.get("leaves")
        if not all(
            isinstance(value, str)
            for value in (name, discriminator, type_table_name, format_table_name)
        ) or not isinstance(leaves, list) or not all(
            isinstance(leaf, str) for leaf in leaves
        ):
            raise ValueError("object specialization family shape is invalid")
        if any(existing.name == name for existing in families):
            raise ValueError(f"duplicate object specialization family {name}")
        if discriminator not in serialized_names:
            raise ValueError(f"{name} discriminator {discriminator} is not serialized")

        type_codes = definitions.get(type_table_name)
        if not isinstance(type_codes, dict):
            raise ValueError(f"definitions have no {type_table_name} object")
        common, specific = format_table(definitions, format_table_name)
        formats: list[ObjectFormat] = []
        for format_name, own_fields in specific.items():
            code = type_codes.get(format_name)
            if not isinstance(code, int) or not 0 <= code <= 0xFFFF:
                raise ValueError(
                    f"{format_table_name}.{format_name} has no uint16 type code"
                )
            fields = (*common, *own_fields)
            names = [field.name for field in fields]
            if len(names) != len(set(names)):
                raise ValueError(f"{format_table_name}.{format_name} duplicates fields")
            missing = sorted(set(names) - serialized_names)
            if missing:
                raise ValueError(
                    f"{format_table_name}.{format_name} has unadmitted fields: "
                    + ", ".join(missing)
                )
            formats.append(ObjectFormat(format_name, code, fields))
        formats.sort(key=lambda value: value.type_code)
        if len({value.type_code for value in formats}) != len(formats):
            raise ValueError(f"{name} formats duplicate a type code")
        missing_leaves = sorted(set(leaves) - set(specific))
        if missing_leaves:
            raise ValueError(f"{name} selects unknown leaves: {', '.join(missing_leaves)}")
        duplicate_leaves = selected_leaves & set(leaves)
        if duplicate_leaves:
            raise ValueError(f"leaves selected twice: {', '.join(sorted(duplicate_leaves))}")
        selected_leaves.update(leaves)

        defaults = _string_map(raw.get("defaults"), owner=f"{name}.defaults")
        common_names = {field.name for field in common}
        if not set(defaults) <= common_names:
            raise ValueError(f"{name} defaults must name common fields")
        family = ObjectFamily(
            name,
            discriminator,
            common,
            tuple(formats),
            tuple(leaves),
            tuple(sorted(defaults.items())),
        )
        families.append(family)
        canonical_families.append(
            {
                "name": name,
                "discriminator": discriminator,
                "common": [(field.name, field.presence) for field in common],
                "formats": [
                    {
                        "name": value.name,
                        "code": value.type_code,
                        "fields": [
                            (field.name, field.presence) for field in value.fields
                        ],
                    }
                    for value in formats
                ],
                "leaves": leaves,
                "defaults": sorted(defaults.items()),
            }
        )

    raw_refinements = selected.get("refinements", {})
    if not isinstance(raw_refinements, dict):
        raise ValueError("object specialization refinements must be an object")
    refinements: list[tuple[str, tuple[tuple[str, str], ...]]] = []
    for leaf, raw_fields in sorted(raw_refinements.items()):
        if leaf not in selected_leaves:
            raise ValueError(f"refinement names unselected leaf {leaf}")
        fields = _string_map(raw_fields, owner=f"refinements.{leaf}")
        owning = next(family for family in families if leaf in family.leaves)
        allowed = set(owning.format(leaf).allowed)
        if not set(fields) <= allowed:
            raise ValueError(f"{leaf} refinement names a field outside its format")
        refinements.append((leaf, tuple(sorted(fields.items()))))

    canonical = {
        "schema": 1,
        "families": canonical_families,
        "refinements": [(leaf, list(fields)) for leaf, fields in refinements],
    }
    digest = hashlib.sha256(
        json.dumps(canonical, separators=(",", ":"), sort_keys=True).encode()
    ).hexdigest()
    return SpecializationModel(tuple(families), tuple(refinements), digest)
