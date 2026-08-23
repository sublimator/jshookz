# Regenerate the embedded Xahau definitions header from its JSON.
#
# Why this exists: the headers in `generated/` are committed, and until
# 2026-07-26 nothing regenerated them. There was a rule in
# `x-data/CMakeLists.txt#28ab88f` (vendored from catalogue-tools), but no
# experiment used that file -- both compile x-data's sources directly -- and it
# invoked `${CMAKE_SOURCE_DIR}/scripts/generate-definitions.py`, an upstream
# path that does not exist here. The local generator is
# `scripts/generate_definitions.py` (underscore), next to this file. That dead
# rule, plus its two equally-dead siblings under base58/ and core/, are deleted
# as of the commit that added this file -- they were reachable from no build
# and read as though the definitions were already wired up.
#
# The consequence was silent: refreshing definitions/xahau_definitions.json had
# no effect on any build. The wasm kept the old embed, so a newly-added field
# (DomainID) failed to encode while the rest of the suite stayed green.
#
# Do not point this at the catalogue-tools generator. That one emits
# `inline const std::string` JSON chunks, which allocates during static
# init -- wrong for wasm, where _initialize must not fragment the heap -- and
# it skips clean_definitions(), which drops duplicate `hash`/`index` fields and
# strips the ~86-entry `features` block the codec never reads. The local
# generator emits native Protocol tables (issue 0064), not a JSON blob.
#
# Output goes to the source tree, not the binary dir, because `generated/` is
# already on the include path of both consumers and the header is committed.
# That is only safe because the generator is deterministic: it stamps the
# input's SHA-256 rather than a timestamp, so an unchanged JSON regenerates
# byte-identical output and copy_if_different makes the build a no-op.
#
#
# Usage: include() this, then
#   add_dependencies(<target-compiling-embedded_protocol.cpp> generate_xdata_definitions)

if(TARGET generate_xdata_definitions)
  return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_xdata_dir ${CMAKE_CURRENT_LIST_DIR})
set(_xdata_gen ${_xdata_dir}/scripts/generate_definitions.py)

# The .tmp lives in the binary dir so separately-configured experiments cannot
# tear each other's intermediate write into the shared source-tree output.
function(_xdata_embed network namespace)
  set(_json ${_xdata_dir}/definitions/${network}_definitions.json)
  set(_hdr ${_xdata_dir}/generated/embedded_${network}_definitions.h)
  set(_tmp ${CMAKE_CURRENT_BINARY_DIR}/embedded_${network}_definitions.h.tmp)
  add_custom_command(
    OUTPUT ${_hdr}
    COMMAND ${Python3_EXECUTABLE} ${_xdata_gen} --input ${_json} --output
            ${_tmp} --namespace ${namespace}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_tmp} ${_hdr}
    DEPENDS ${_json} ${_xdata_gen}
    COMMENT "Embedding ${network} definitions")
  set(_xdata_headers ${_xdata_headers} ${_hdr} PARENT_SCOPE)
endfunction()

_xdata_embed(xahau catl::xdata::xahau)
_xdata_embed(xrpl catl::xdata::xrpl)

set(_xdata_static_policy
    ${_xdata_dir}/definitions/provider_static_policy.json)
set(_xdata_static_hdr ${_xdata_dir}/generated/static_xahau_protocol.h)
set(_xdata_static_tmp
    ${CMAKE_CURRENT_BINARY_DIR}/static_xahau_protocol.h.tmp)
add_custom_command(
  OUTPUT ${_xdata_static_hdr}
  COMMAND ${Python3_EXECUTABLE} ${_xdata_gen}
          --input ${_xdata_dir}/definitions/xahau_definitions.json
          --output ${_xdata_static_tmp}
          --namespace catl::xdata::xahau_static_data
          --provider-static
          --materializer-policy ${_xdata_static_policy}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${_xdata_static_tmp} ${_xdata_static_hdr}
  DEPENDS ${_xdata_dir}/definitions/xahau_definitions.json
          ${_xdata_gen} ${_xdata_static_policy}
  COMMENT "Generating allocation-free Xahau provider protocol")
list(APPEND _xdata_headers ${_xdata_static_hdr})

add_custom_target(generate_xdata_definitions DEPENDS ${_xdata_headers})
