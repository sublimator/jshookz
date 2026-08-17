# x-data-quickjs

JS face of `cpp/x-data`. Provider-blind: Slice + FieldDef in, JSValue /
bytes / `std::expected` out. Tests live here because they prove this
library, even though the runner is Python.

The standalone wasm reactor (`standalone_wasm_bindings.c`, target
`jshookz_xrpl_codec`) is a test/oracle face, not the public API.
