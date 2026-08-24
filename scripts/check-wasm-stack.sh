#!/usr/bin/env bash
set -euo pipefail

if [ -n "${EXPECTED_STACK_POINTER:-}" ]; then
    expected="${EXPECTED_STACK_POINTER}"
else
    script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
    expected="$(python3 - "$script_dir/../xahau/profiles/xahau-quickjs-v1.source.json" <<'PY'
import json
import pathlib
import sys

source = json.loads(pathlib.Path(sys.argv[1]).read_text())
print(source["provider"]["build"]["wasm_stack_bytes"])
PY
)"
fi

if [ "$#" -eq 0 ]; then
    set -- \
        "build/xahau-provider/jshookz_provider.wasm"
fi

python3 - "$expected" "$@" <<'PY'
import pathlib
import sys

expected = int(sys.argv[1], 0)
paths = [pathlib.Path(arg) for arg in sys.argv[2:]]
ok = True


def read_u32(data, pos):
    value = 0
    shift = 0
    while True:
        if pos >= len(data):
            raise ValueError("truncated LEB128")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, pos
        shift += 7
        if shift >= 35:
            raise ValueError("LEB128 too large")


def find_stack_pointer_initial(data):
    if data[:4] != b"\0asm" or data[4:8] != b"\x01\0\0\0":
        raise ValueError("not a wasm32 binary")

    pos = 8
    while pos < len(data):
        section_id = data[pos]
        pos += 1
        section_size, pos = read_u32(data, pos)
        section_end = pos + section_size
        if section_end > len(data):
            raise ValueError("truncated section")

        if section_id == 6:
            count, gpos = read_u32(data, pos)
            for _ in range(count):
                value_type = data[gpos]
                mutable = data[gpos + 1]
                gpos += 2
                opcode = data[gpos]
                gpos += 1
                if opcode != 0x41:
                    raise ValueError("global initializer is not i32.const")
                init_value, gpos = read_u32(data, gpos)
                if gpos >= section_end or data[gpos] != 0x0b:
                    raise ValueError("global initializer missing end opcode")
                gpos += 1
                if value_type == 0x7f and mutable == 1:
                    return init_value
            raise ValueError("no mutable i32 global found")

        pos = section_end

    raise ValueError("no global section found")


for path in paths:
    if not path.exists():
        print(f"{path}: required artifact is missing", file=sys.stderr)
        ok = False
        continue
    try:
        initial = find_stack_pointer_initial(path.read_bytes())
    except Exception as exc:
        print(f"{path}: could not read __stack_pointer initial value: {exc}", file=sys.stderr)
        ok = False
        continue
    if initial != expected:
        print(
            f"{path}: __stack_pointer initial value is {initial}, expected {expected}",
            file=sys.stderr,
        )
        ok = False
    else:
        print(f"{path}: __stack_pointer initial value {initial}")

if not ok:
    sys.exit(1)
PY
