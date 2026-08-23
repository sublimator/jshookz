#pragma once

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace catl::xdata::test {

enum class PathSetMarkEdge : std::uint8_t {
  none,
  facade,
  pathset,
  path,
  pathhop,
};

char const *pathset_mark_edge_name(PathSetMarkEdge edge) noexcept;

bool parse_pathset_mark_edge(char const *name, PathSetMarkEdge &edge) noexcept;

struct PathSetQuickJSCounters {
  std::uint64_t owners_created = 0;
  std::uint64_t owners_finalized = 0;
  std::uint64_t facades_created = 0;
  std::uint64_t facades_finalized = 0;
  std::uint64_t pathsets_created = 0;
  std::uint64_t pathsets_finalized = 0;
  std::uint64_t paths_created = 0;
  std::uint64_t paths_finalized = 0;
  std::uint64_t pathhops_created = 0;
  std::uint64_t pathhops_finalized = 0;

  std::uint64_t directory_measure_calls = 0;
  std::uint64_t directory_allocate_attempts = 0;
  std::uint64_t directory_allocations = 0;
  std::uint64_t directory_frees = 0;
  std::uint64_t directory_bytes_allocated = 0;
  std::uint64_t directory_bytes_freed = 0;
  std::uint64_t directory_fill_calls = 0;
  std::uint64_t directory_fill_successes = 0;
  std::uint64_t forced_directory_ooms = 0;
  std::uint64_t pathset_cache_stores = 0;
  std::uint64_t pathset_cache_hits = 0;

  std::uint64_t account_component_copies = 0;
  std::uint64_t currency_component_copies = 0;
  std::uint64_t issuer_component_copies = 0;
};

// Opaque-to-QuickJS allocator control. The facade arms reject_exact_size only
// immediately around its directory js_malloc call.
struct PathSetQuickJSAllocatorControl {
  bool fail_next_directory = false;
  bool reject_exact_size = false;
  std::size_t exact_size = 0;
  std::uint64_t exact_rejections = 0;
};

// Native-only test fixture for the frozen private PathSet facade. It registers
// no global constructor and exposes no provider surface.
class PathSetQuickJSFixture {
public:
  explicit PathSetQuickJSFixture(
      PathSetMarkEdge disabled_mark = PathSetMarkEdge::none) noexcept;
  ~PathSetQuickJSFixture();

  PathSetQuickJSFixture(PathSetQuickJSFixture const &) = delete;
  PathSetQuickJSFixture &operator=(PathSetQuickJSFixture const &) = delete;

  bool ready() const noexcept;

  char const *init_error() const noexcept;

  JSContext *context() const noexcept;

  JSRuntime *runtime() const noexcept;

  PathSetQuickJSCounters const &counters() const noexcept;

  bool v1_sandbox_builtins_absent() const noexcept;

  // The bytes are copied once into the sole CertifiedRoot before a facade is
  // returned. The default ordinal is the first field in the certified object.
  JSValue make_facade(std::span<std::uint8_t const> object_bytes,
                      std::size_t pathset_ordinal = 0);

  JSValue paths(JSValueConst facade);

  JSValue at(JSValueConst parent, std::uint32_t index);

  JSValue property(JSValueConst object, char const *name);

  bool facade_has_cached_pathset(JSValueConst facade) const noexcept;

  bool directory_snapshot(JSValueConst pathset, std::span<std::uint32_t> output,
                          std::size_t &path_count,
                          std::size_t &hop_count) const noexcept;

  void fail_next_directory_allocation() noexcept;

  std::string take_exception_message();

  void run_gc() noexcept;

  // Plants one ordinary-property back edge around the selected hidden edge.
  // The caller must then release its facade local before running the GC.
  bool plant_cycle(PathSetMarkEdge edge, JSValueConst facade);

  bool cycle_collected(PathSetMarkEdge edge) const noexcept;

  // Re-enables a deliberately disabled mark edge through a non-owning test
  // handle and runs GC, so a red-control process can tear its runtime down.
  bool cleanup_red_cycle() noexcept;

private:
  bool register_classes() noexcept;

  JSRuntime *runtime_ = nullptr;
  JSContext *context_ = nullptr;
  PathSetQuickJSCounters counters_{};
  PathSetQuickJSAllocatorControl allocator_control_{};
  PathSetMarkEdge disabled_mark_ = PathSetMarkEdge::none;
  char const *init_error_ = nullptr;

  // Borrowed only. Set exclusively for a deliberately retained red-control
  // cycle, then cleared before the cleanup GC.
  JSValue cleanup_source_ = JS_UNDEFINED;
  PathSetMarkEdge cleanup_edge_ = PathSetMarkEdge::none;
};

} // namespace catl::xdata::test
