# Vendored code and pinned sources

This file records the source and licensing boundary of code shipped in the
repository. Generated artifacts record their own exact inputs where practical.

## QuickJS

`cpp/quickjs/` preserves Bellard QuickJS history from fork point
`d7ae12ae71dfd6ab2997527d295014a8996fa0f9` (2026-03-23). Its MIT license is
retained at `cpp/quickjs/LICENSE`. Local deterministic-runtime changes are
part of the jshookz product tree.

Run `vendor/check-quickjs.sh` to compare the subtree with Bellard's
current `master`; the script uses the dedicated public upstream URL and never
assumes that this repository's `origin` points at QuickJS.

## catl::xdata

`cpp/x-data/` originates from Nicholas Dudfield's `catalogue-tools`
codebase at commit `298b81fe` and has since been adapted for deterministic WASM
execution: explicit error policy, injected crypto operations, and generated
embedded definitions. Nicholas owns this code and distributes the copy in
this repository under the root MIT license.

`vendor/check-xdata.sh` compares it with an explicitly supplied
`CATALOGUE_TOOLS` checkout. Local adaptations are expected and must be
preserved deliberately during an update.

### Selective back-ports, 2026-08-18

The vendor point stays at `298b81f`. These are individual upstream commits
pulled forward, not a re-vendor, so `check-xdata.sh` still reports the rest of
the tree against the original point:

| Upstream | What | How it landed |
|---|---|---|
| `e39b942` | `acq_rel` on the intrusive refcount RMW (TSan-clean) | `core/src/types.cpp` taken verbatim; now in sync with upstream HEAD |
| `e76de19` | optional `SliceVisitor` callbacks via `detail::call_*` | `includes/catl/xdata/slice-visitor.h` taken verbatim; now in sync |
| `d5d11ba` | `kMaxParseDepth = 256` threaded through the parser | `includes/catl/xdata/parser.h` taken from upstream HEAD, then every `throw` re-wrapped as `CATL_XDATA_THROW` |
| `2f08afe` | `kMaxBase58Len = 1024` bound on quadratic base58 | hand-applied to `base58/src/base58.cpp`, which keeps the injected `sha256_oneshot` and the exception policy |

Deliberately **not** taken:

- `870ff8c` / `6e3c99e` canonical protocol definitions. Upstream's
  `xahau_definitions.json` carries `EntropyCount`, `RandomData`,
  `RandomDigests`, `Validator` and the `Entropy`/`Shuffle` transaction types,
  which are Xahau **feature-branch** fields. Our copy is the later pin with
  recorded provenance (see below) and taking upstream's would regress it.
- `1119fdd` duplicate field-code fix — already present here.
- `64f4bb8`, `ad3b4b8`, `1e12e7a` — stats visitor, CMake codegen and logger
  diagnostics, none of which this tree builds.

The exception policy is the standing adaptation to re-apply on any future
pull: `cpp/provider/CMakeLists.txt` builds `-fno-exceptions`, and
`exception_policy.h` degrades a throw to `__builtin_trap()`. An in-module C++
throw cannot be caught here at all, so the depth cap aborts the instance
rather than raising — still strictly better than the stack-exhaustion SIGSEGV
it replaces, but not a recoverable error.

## Protocol definitions

The embedded definitions are content-pinned inputs from XRPL/Xahau
`server_definitions` output:

| File | SHA-256 | Embedded protocol hash |
|---|---|---|
| `cpp/x-data/definitions/xahau_definitions.json` | `9934e49481656ff0c8880c7b8ce7b85ac7d4f08ddbeeeeb2394e6223415e0c96` | `DCED6D8E6D66EA2AA216341482C30E2BA66C31088836B9C8CA27D181BA5B8B12` |
| `cpp/x-data/definitions/xrpl_definitions.json` | `96d73f79bf4d83b13ac22d9ff58744cfab90a557dbbb82328a62504a8a323dd8` | recorded in the JSON |

Xahau JSON refresh, 2026-08-16:

- repository: `https://github.com/Xahau/xahaud` (local checkout `xahaud-hookz-test-vectors`)
- commit: `f7e01c799e12baf4821a1da97301484873516241`
- command: `build/rippled --definitions`
- content SHA-256: `9934e49481656ff0c8880c7b8ce7b85ac7d4f08ddbeeeeb2394e6223415e0c96`
- generated header SHA-256: `e4d79c92cbabe292cc14b9606cad9b5e0b99ee720246605ae89882224a804d7d`
- generated header also records the JSON SHA-256 in its `// SHA-256:` line
- 2026-08-17: header is native Protocol tables, not an embedded JSON blob

## Xahau Hook ABI

`xahau/generated/raw-hook-abi.json` is projected from public Xahau
source at commit `bb244ef7729503a0317bcff0f8fdaa93ca5cb7d2`, path
`include/xrpl/hook/hook_api.macro`. hookz performs the Tree-sitter extraction;
`xahau/raw-hook-api-policy.json` selects the reviewed provider
slice. Generated files carry the same pin and are freshness-checked.

## External build and test dependencies

- Binaryen 128 supplies the canonical `wasm-opt -O3` provider post-link step.
  CI checksum-verifies its official release archive; the build rejects a
  missing or different `wasm-opt` version so provider bytes never depend on
  ambient `PATH` contents.
- `wasmtime-py` is pinned to 47.0.1 as the Python host oracle. The sealed
  provider does not embed Wasmtime.
- hostem pins hookz to public commit
  `299a812d9badb5baf66da9fe73e7b1f740298822`.
