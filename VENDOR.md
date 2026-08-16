# Vendored code and pinned sources

This file records the source and licensing boundary of code shipped in the
repository. Generated artifacts record their own exact inputs where practical.

## QuickJS

`engine/quickjs/` preserves Bellard QuickJS history from fork point
`d7ae12ae71dfd6ab2997527d295014a8996fa0f9` (2026-03-23). Its MIT license is
retained at `engine/quickjs/LICENSE`. Local deterministic-runtime changes are
part of the jshookz product tree.

Run `scripts/check-upstream-quickjs.sh` to compare the subtree with Bellard's
current `master`; the script uses the dedicated public upstream URL and never
assumes that this repository's `origin` points at QuickJS.

## catl::xdata

`codec/xrpl/x-data/` originates from Nicholas Dudfield's `catalogue-tools`
codebase at commit `298b81fe` and has since been adapted for deterministic WASM
execution: explicit error policy, injected crypto operations, and generated
embedded definitions. Nicholas owns this code and distributes the copy in
this repository under the root MIT license.

`scripts/check-upstream-xdata.sh` compares it with an explicitly supplied
`CATALOGUE_TOOLS` checkout. Local adaptations are expected and must be
preserved deliberately during an update.

## Protocol definitions

The embedded definitions are content-pinned inputs from XRPL/Xahau
`server_definitions` output:

| File | SHA-256 | Embedded protocol hash |
|---|---|---|
| `codec/xrpl/x-data/definitions/xahau_definitions.json` | `9934e49481656ff0c8880c7b8ce7b85ac7d4f08ddbeeeeb2394e6223415e0c96` | `DCED6D8E6D66EA2AA216341482C30E2BA66C31088836B9C8CA27D181BA5B8B12` |
| `codec/xrpl/x-data/definitions/xrpl_definitions.json` | `96d73f79bf4d83b13ac22d9ff58744cfab90a557dbbb82328a62504a8a323dd8` | recorded in the JSON |

Xahau JSON refresh, 2026-08-16:

- repository: `https://github.com/Xahau/xahaud` (local checkout `xahaud-hookz-test-vectors`)
- commit: `f7e01c799e12baf4821a1da97301484873516241`
- command: `build/rippled --definitions`
- content SHA-256: `9934e49481656ff0c8880c7b8ce7b85ac7d4f08ddbeeeeb2394e6223415e0c96`
- generated header SHA-256: `6a055a756eb61b93442db2caefe6ff3c95a405d950eadc268a6b4788fc4b9290`
- generated header also records the JSON SHA-256 in its `// SHA-256:` line

## Xahau Hook ABI

`integrations/xahau/generated/raw-hook-abi.json` is projected from public Xahau
source at commit `bb244ef7729503a0317bcff0f8fdaa93ca5cb7d2`, path
`include/xrpl/hook/hook_api.macro`. hookz performs the Tree-sitter extraction;
`integrations/xahau/raw-hook-api-policy.json` selects the reviewed provider
slice. Generated files carry the same pin and are freshness-checked.

## External build and test dependencies

- Binaryen 128 supplies the canonical `wasm-opt -O3` provider post-link step.
  CI checksum-verifies its official release archive; the build rejects a
  missing or different `wasm-opt` version so provider bytes never depend on
  ambient `PATH` contents.
- Wasmtime C API 47.0.3 is downloaded through `conan/wasmtime/` or CI from
  checksummed official release archives; it is Apache-2.0 with LLVM exception.
- `wasmtime-py` is pinned to its separately published 47.0.1 package. Both
  embeddings are Wasmtime major 47; the projects do not publish identical
  patch numbers.
- hostem pins hookz to public commit
  `299a812d9badb5baf66da9fe73e7b1f740298822`.
- The differential test pins `ripple-binary-codec` 2.4.1 and overrides its
  transitive `ws` dependency to patched 8.21.0.
