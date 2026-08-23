#include "pathset_quickjs_fixture.h"

#include "catl/xdata/amount-view.h"
#include "catl/xdata/pathset-rules.h"
#include "catl/xdata/pathset-view.h"
#include "catl/xdata/protocol.h"

#include <jshookz/qjs.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace catl::xdata {

// This exact qualified name is the sole private integration seam friended by
// CertifiedObject. It is intentionally defined only in this native test TU.
class CertifiedObjectValue {
public:
  CertifiedObjectValue(CertifiedObject &&object,
                       test::PathSetQuickJSCounters *counters) noexcept
      : object_(std::move(object)), counters_(counters) {}

  std::optional<PathSetView> pathset_view(std::size_t ordinal) const noexcept {
    return object_.bind_view<PathSetView>(ordinal);
  }

  test::PathSetQuickJSCounters *counters() const noexcept { return counters_; }

private:
  CertifiedObject object_;
  test::PathSetQuickJSCounters *counters_ = nullptr;
};

} // namespace catl::xdata

namespace catl::xdata::test {
namespace {

namespace qjs = jshookz::qjs;

JSClassID g_owner_class_id = 0;
JSClassID g_facade_class_id = 0;
JSClassID g_pathset_class_id = 0;
JSClassID g_path_class_id = 0;
JSClassID g_pathhop_class_id = 0;

struct alignas(std::max_align_t) TestAllocationHeader {
  std::size_t size = 0;
};

bool allocator_would_exceed(JSMallocState const *state, std::size_t old_size,
                            std::size_t new_size) noexcept {
  if (state->malloc_size < old_size)
    return true;
  std::size_t const retained = state->malloc_size - old_size;
  return new_size > state->malloc_limit ||
         retained > state->malloc_limit - new_size;
}

void *test_malloc(JSMallocState *state, std::size_t size) {
  auto *control = static_cast<PathSetQuickJSAllocatorControl *>(state->opaque);
  if (control != nullptr && control->reject_exact_size &&
      size == control->exact_size) {
    control->reject_exact_size = false;
    ++control->exact_rejections;
    return nullptr;
  }
  if (size == 0 ||
      size > std::numeric_limits<std::size_t>::max() -
                 sizeof(TestAllocationHeader) ||
      allocator_would_exceed(state, 0, size))
    return nullptr;
  auto *header = static_cast<TestAllocationHeader *>(
      std::malloc(sizeof(TestAllocationHeader) + size));
  if (header == nullptr)
    return nullptr;
  header->size = size;
  ++state->malloc_count;
  state->malloc_size += size;
  return header + 1;
}

void test_free(JSMallocState *state, void *pointer) {
  if (pointer == nullptr)
    return;
  auto *header = static_cast<TestAllocationHeader *>(pointer) - 1;
  --state->malloc_count;
  state->malloc_size -= header->size;
  std::free(header);
}

void *test_realloc(JSMallocState *state, void *pointer, std::size_t size) {
  if (pointer == nullptr)
    return size == 0 ? nullptr : test_malloc(state, size);
  auto *header = static_cast<TestAllocationHeader *>(pointer) - 1;
  std::size_t const old_size = header->size;
  if (size == 0) {
    test_free(state, pointer);
    return nullptr;
  }

  auto *control = static_cast<PathSetQuickJSAllocatorControl *>(state->opaque);
  if (control != nullptr && control->reject_exact_size &&
      size == control->exact_size) {
    control->reject_exact_size = false;
    ++control->exact_rejections;
    return nullptr;
  }
  if (size > std::numeric_limits<std::size_t>::max() -
                 sizeof(TestAllocationHeader) ||
      allocator_would_exceed(state, old_size, size))
    return nullptr;
  auto *replacement = static_cast<TestAllocationHeader *>(
      std::realloc(header, sizeof(TestAllocationHeader) + size));
  if (replacement == nullptr)
    return nullptr;
  replacement->size = size;
  state->malloc_size = state->malloc_size - old_size + size;
  return replacement + 1;
}

std::size_t test_malloc_usable_size(void const *pointer) {
  if (pointer == nullptr)
    return 0;
  return (static_cast<TestAllocationHeader const *>(pointer) - 1)->size;
}

JSMallocFunctions const g_test_allocator = {
    .js_malloc = test_malloc,
    .js_free = test_free,
    .js_realloc = test_realloc,
    .js_malloc_usable_size = test_malloc_usable_size,
};

struct FacadeState {
  JSValue owner = JS_UNDEFINED;
  JSValue cached_pathset = JS_UNDEFINED;
  PathSetQuickJSCounters *counters = nullptr;
  PathSetQuickJSAllocatorControl *allocator_control = nullptr;
  std::size_t pathset_ordinal = 0;
  PathSetMarkEdge disabled_mark = PathSetMarkEdge::none;
  bool mark_enabled = true;
};

struct PathSetState {
  JSValue owner = JS_UNDEFINED;
  Slice payload{};
  std::uint32_t *directory = nullptr;
  PathSetQuickJSCounters *counters = nullptr;
  std::size_t path_count = 0;
  std::size_t hop_count = 0;
  std::size_t directory_bytes = 0;
  PathSetMarkEdge disabled_mark = PathSetMarkEdge::none;
  bool mark_enabled = true;
};

struct PathState {
  JSValue parent = JS_UNDEFINED;
  PathSetQuickJSCounters *counters = nullptr;
  std::uint32_t path_index = 0;
  PathSetMarkEdge disabled_mark = PathSetMarkEdge::none;
  bool mark_enabled = true;
};

struct PathHopState {
  JSValue parent = JS_UNDEFINED;
  PathSetQuickJSCounters *counters = nullptr;
  std::array<std::uint8_t, 20> account{};
  std::array<std::uint8_t, 20> currency{};
  std::array<std::uint8_t, 20> issuer{};
  std::uint32_t hop_ordinal = 0;
  PathSetMarkEdge disabled_mark = PathSetMarkEdge::none;
  std::uint8_t cached_components = 0;
  bool mark_enabled = true;
};

static_assert(std::is_trivially_copyable_v<PathState>);
static_assert(std::is_trivially_copyable_v<PathHopState>);
static_assert(sizeof(PathState) <= sizeof(JSValue) + sizeof(void *) + 16);
static_assert(sizeof(PathHopState) <= sizeof(JSValue) + sizeof(void *) + 80);

constexpr std::uint8_t kAccountCached = 1u << 0;
constexpr std::uint8_t kCurrencyCached = 1u << 1;
constexpr std::uint8_t kIssuerCached = 1u << 2;

class LocalValue {
public:
  LocalValue(JSContext *context, JSValue value) noexcept
      : context_(context), value_(value) {}

  ~LocalValue() { JS_FreeValue(context_, value_); }

  LocalValue(LocalValue const &) = delete;
  LocalValue &operator=(LocalValue const &) = delete;

  JSValueConst get() const noexcept { return value_; }

  bool is_exception() const noexcept { return JS_IsException(value_); }

private:
  JSContext *context_ = nullptr;
  JSValue value_ = JS_UNDEFINED;
};

bool apply_v1_sandbox_subset(JSContext *context) {
  LocalValue global(context, JS_GetGlobalObject(context));
  if (global.is_exception())
    return false;
  LocalValue math(context, JS_GetPropertyStr(context, global.get(), "Math"));
  if (math.is_exception())
    return false;

  JSAtom random = JS_NewAtom(context, "random");
  if (random == JS_ATOM_NULL)
    return false;
  int const random_deleted =
      JS_DeleteProperty(context, math.get(), random, 0);
  JS_FreeAtom(context, random);
  if (random_deleted < 0)
    return false;

  for (char const *name : {"SharedArrayBuffer", "Atomics"}) {
    JSAtom atom = JS_NewAtom(context, name);
    if (atom == JS_ATOM_NULL)
      return false;
    int const deleted = JS_DeleteProperty(context, global.get(), atom, 0);
    JS_FreeAtom(context, atom);
    if (deleted < 0)
      return false;
  }
  return !JS_HasException(context);
}

template <class T, class... Args>
T *new_state(JSContext *context, Args &&...args) noexcept {
  void *storage = js_malloc(context, sizeof(T));
  if (storage == nullptr)
    return nullptr;
  return new (storage) T(std::forward<Args>(args)...);
}

template <class T> void free_state(JSRuntime *runtime, T *state) noexcept {
  state->~T();
  js_free_rt(runtime, state);
}

void owner_finalizer(JSRuntime *runtime, JSValue value) {
  auto *owner = static_cast<CertifiedObjectValue *>(
      JS_GetOpaque(value, g_owner_class_id));
  if (owner == nullptr)
    return;
  auto *counters = owner->counters();
  owner->~CertifiedObjectValue();
  if (counters != nullptr)
    ++counters->owners_finalized;
  js_free_rt(runtime, owner);
}

void facade_finalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<FacadeState *>(JS_GetOpaque(value, g_facade_class_id));
  if (state == nullptr)
    return;
  auto *counters = state->counters;
  JS_FreeValueRT(runtime, state->cached_pathset);
  JS_FreeValueRT(runtime, state->owner);
  if (counters != nullptr)
    ++counters->facades_finalized;
  free_state(runtime, state);
}

void facade_mark(JSRuntime *runtime, JSValueConst value,
                 JS_MarkFunc *mark_func) {
  auto *state =
      static_cast<FacadeState *>(JS_GetOpaque(value, g_facade_class_id));
  if (state == nullptr || !state->mark_enabled)
    return;
  JS_MarkValue(runtime, state->owner, mark_func);
  JS_MarkValue(runtime, state->cached_pathset, mark_func);
}

void pathset_finalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<PathSetState *>(JS_GetOpaque(value, g_pathset_class_id));
  if (state == nullptr)
    return;
  auto *counters = state->counters;

  // Deliberate teardown order: free the exact directory word block, then
  // release the owner. Never inspect the borrowed payload here.
  if (state->directory != nullptr) {
    js_free_rt(runtime, state->directory);
    if (counters != nullptr) {
      ++counters->directory_frees;
      counters->directory_bytes_freed += state->directory_bytes;
    }
  }
  JS_FreeValueRT(runtime, state->owner);
  if (counters != nullptr)
    ++counters->pathsets_finalized;
  free_state(runtime, state);
}

void pathset_mark(JSRuntime *runtime, JSValueConst value,
                  JS_MarkFunc *mark_func) {
  auto *state =
      static_cast<PathSetState *>(JS_GetOpaque(value, g_pathset_class_id));
  if (state == nullptr || !state->mark_enabled)
    return;
  JS_MarkValue(runtime, state->owner, mark_func);
}

void path_finalizer(JSRuntime *runtime, JSValue value) {
  auto *state = static_cast<PathState *>(JS_GetOpaque(value, g_path_class_id));
  if (state == nullptr)
    return;
  auto *counters = state->counters;
  JS_FreeValueRT(runtime, state->parent);
  if (counters != nullptr)
    ++counters->paths_finalized;
  free_state(runtime, state);
}

void path_mark(JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark_func) {
  auto *state = static_cast<PathState *>(JS_GetOpaque(value, g_path_class_id));
  if (state == nullptr || !state->mark_enabled)
    return;
  JS_MarkValue(runtime, state->parent, mark_func);
}

void pathhop_finalizer(JSRuntime *runtime, JSValue value) {
  auto *state =
      static_cast<PathHopState *>(JS_GetOpaque(value, g_pathhop_class_id));
  if (state == nullptr)
    return;
  auto *counters = state->counters;
  JS_FreeValueRT(runtime, state->parent);
  if (counters != nullptr)
    ++counters->pathhops_finalized;
  free_state(runtime, state);
}

void pathhop_mark(JSRuntime *runtime, JSValueConst value,
                  JS_MarkFunc *mark_func) {
  auto *state =
      static_cast<PathHopState *>(JS_GetOpaque(value, g_pathhop_class_id));
  if (state == nullptr || !state->mark_enabled)
    return;
  JS_MarkValue(runtime, state->parent, mark_func);
}

JSClassDef const g_owner_class = {
    .class_name = "CertifiedObjectValue",
    .finalizer = owner_finalizer,
};

JSClassDef const g_facade_class = {
    .class_name = "PrivateSTObjectFacade",
    .finalizer = facade_finalizer,
    .gc_mark = facade_mark,
};

JSClassDef const g_pathset_class = {
    .class_name = "PrivatePathSet",
    .finalizer = pathset_finalizer,
    .gc_mark = pathset_mark,
};

JSClassDef const g_path_class = {
    .class_name = "PrivatePath",
    .finalizer = path_finalizer,
    .gc_mark = path_mark,
};

JSClassDef const g_pathhop_class = {
    .class_name = "PrivatePathHop",
    .finalizer = pathhop_finalizer,
    .gc_mark = pathhop_mark,
};

JSValue new_owner(JSContext *context, CertifiedObject &&object,
                  PathSetQuickJSCounters *counters) {
  JSValue value = JS_NewObjectClass(context, g_owner_class_id);
  if (JS_IsException(value))
    return value;
  auto *state =
      new_state<CertifiedObjectValue>(context, std::move(object), counters);
  if (state == nullptr) {
    JS_FreeValue(context, value);
    return JS_EXCEPTION;
  }
  JS_SetOpaque(value, state);
  ++counters->owners_created;
  return value;
}

JSValue new_facade(JSContext *context, JSValueConst owner, std::size_t ordinal,
                   PathSetMarkEdge disabled_mark,
                   PathSetQuickJSAllocatorControl *allocator_control,
                   PathSetQuickJSCounters *counters) {
  JSValue value = JS_NewObjectClass(context, g_facade_class_id);
  if (JS_IsException(value))
    return value;
  auto *state = new_state<FacadeState>(context);
  if (state == nullptr) {
    JS_FreeValue(context, value);
    return JS_EXCEPTION;
  }
  state->owner = JS_DupValue(context, owner);
  state->counters = counters;
  state->allocator_control = allocator_control;
  state->pathset_ordinal = ordinal;
  state->disabled_mark = disabled_mark;
  state->mark_enabled = disabled_mark != PathSetMarkEdge::facade;
  JS_SetOpaque(value, state);
  ++counters->facades_created;
  if (JS_PreventExtensions(context, value) < 0) {
    JS_FreeValue(context, value);
    return JS_EXCEPTION;
  }
  return value;
}

JSValue new_pathset(JSContext *context, JSValueConst owner, Slice payload,
                    PathSetDirectoryShape const &shape,
                    std::uint32_t *directory, PathSetMarkEdge disabled_mark,
                    PathSetQuickJSCounters *counters) {
  JSValue value = JS_NewObjectClass(context, g_pathset_class_id);
  if (JS_IsException(value))
    return value;
  auto *state = new_state<PathSetState>(context);
  if (state == nullptr) {
    JS_FreeValue(context, value);
    return JS_EXCEPTION;
  }
  state->owner = JS_DupValue(context, owner);
  state->payload = payload;
  state->directory = directory;
  state->counters = counters;
  state->path_count = shape.paths;
  state->hop_count = shape.hops;
  state->directory_bytes = shape.bytes;
  state->disabled_mark = disabled_mark;
  state->mark_enabled = disabled_mark != PathSetMarkEdge::pathset;
  JS_SetOpaque(value, state);
  ++counters->pathsets_created;
  return value;
}

JSValue new_path(JSContext *context, JSValueConst pathset,
                 std::uint32_t path_index, PathSetMarkEdge disabled_mark,
                 PathSetQuickJSCounters *counters) {
  JSValue value = JS_NewObjectClass(context, g_path_class_id);
  if (JS_IsException(value))
    return value;
  auto *state = new_state<PathState>(context);
  if (state == nullptr) {
    JS_FreeValue(context, value);
    return JS_EXCEPTION;
  }
  state->parent = JS_DupValue(context, pathset);
  state->counters = counters;
  state->path_index = path_index;
  state->disabled_mark = disabled_mark;
  state->mark_enabled = disabled_mark != PathSetMarkEdge::path;
  JS_SetOpaque(value, state);
  ++counters->paths_created;
  return value;
}

JSValue new_pathhop(JSContext *context, JSValueConst pathset,
                    std::uint32_t hop_ordinal, PathSetMarkEdge disabled_mark,
                    PathSetQuickJSCounters *counters) {
  JSValue value = JS_NewObjectClass(context, g_pathhop_class_id);
  if (JS_IsException(value))
    return value;
  auto *state = new_state<PathHopState>(context);
  if (state == nullptr) {
    JS_FreeValue(context, value);
    return JS_EXCEPTION;
  }
  state->parent = JS_DupValue(context, pathset);
  state->counters = counters;
  state->hop_ordinal = hop_ordinal;
  state->disabled_mark = disabled_mark;
  state->mark_enabled = disabled_mark != PathSetMarkEdge::pathhop;
  JS_SetOpaque(value, state);
  ++counters->pathhops_created;
  return value;
}

bool read_index(JSContext *context, int argc, JSValueConst *argv,
                std::uint32_t &index) {
  if (argc < 1) {
    JS_ThrowTypeError(context, "at() requires an index");
    return false;
  }
  std::uint64_t wide = 0;
  if (JS_ToIndex(context, &wide, argv[0]) < 0)
    return false;
  if (wide > std::numeric_limits<std::uint32_t>::max()) {
    JS_ThrowRangeError(context, "index out of range");
    return false;
  }
  index = static_cast<std::uint32_t>(wide);
  return true;
}

JSValue facade_paths_getter(JSContext *context, JSValueConst this_value) {
  auto *facade = static_cast<FacadeState *>(
      JS_GetOpaque2(context, this_value, g_facade_class_id));
  if (facade == nullptr)
    return JS_EXCEPTION;
  if (!JS_IsUndefined(facade->cached_pathset)) {
    ++facade->counters->pathset_cache_hits;
    return JS_DupValue(context, facade->cached_pathset);
  }

  auto *owner = static_cast<CertifiedObjectValue *>(
      JS_GetOpaque2(context, facade->owner, g_owner_class_id));
  if (owner == nullptr)
    return JS_EXCEPTION;
  auto view = owner->pathset_view(facade->pathset_ordinal);
  if (!view)
    return JS_ThrowTypeError(context, "Paths field is not bindable");

  ++facade->counters->directory_measure_calls;
  auto shape = PathSetRules::measure_directory(view->payload());
  if (!shape)
    return JS_ThrowInternalError(context, "PathSet directory overflow");

  ++facade->counters->directory_allocate_attempts;
  bool const force_oom = facade->allocator_control->fail_next_directory;
  std::uint64_t const rejected_before =
      facade->allocator_control->exact_rejections;
  if (force_oom) {
    facade->allocator_control->fail_next_directory = false;
    facade->allocator_control->reject_exact_size = true;
    facade->allocator_control->exact_size = shape->bytes;
  }

  auto *directory =
      static_cast<std::uint32_t *>(js_malloc(context, shape->bytes));
  facade->allocator_control->reject_exact_size = false;
  facade->allocator_control->exact_size = 0;
  bool const forced_rejection =
      facade->allocator_control->exact_rejections == rejected_before + 1;
  if (forced_rejection)
    ++facade->counters->forced_directory_ooms;
  if (directory == nullptr)
    return JS_EXCEPTION;
  if (force_oom) {
    js_free(context, directory);
    return JS_ThrowInternalError(context,
                                 "directory allocator red control missed");
  }
  ++facade->counters->directory_allocations;
  facade->counters->directory_bytes_allocated += shape->bytes;

  ++facade->counters->directory_fill_calls;
  if (!PathSetRules::fill_directory(
          view->payload(), *shape,
          std::span<std::uint32_t>{directory, shape->words})) {
    js_free(context, directory);
    ++facade->counters->directory_frees;
    facade->counters->directory_bytes_freed += shape->bytes;
    return JS_ThrowInternalError(context, "PathSet directory fill failed");
  }
  ++facade->counters->directory_fill_successes;

  JSValue pathset =
      new_pathset(context, facade->owner, view->payload(), *shape, directory,
                  facade->disabled_mark, facade->counters);
  if (JS_IsException(pathset)) {
    js_free(context, directory);
    ++facade->counters->directory_frees;
    facade->counters->directory_bytes_freed += shape->bytes;
    return pathset;
  }

  facade->cached_pathset = JS_DupValue(context, pathset);
  ++facade->counters->pathset_cache_stores;
  return pathset;
}

JSValue pathset_length_getter(JSContext *context, JSValueConst this_value) {
  auto *state = static_cast<PathSetState *>(
      JS_GetOpaque2(context, this_value, g_pathset_class_id));
  if (state == nullptr)
    return JS_EXCEPTION;
  return JS_NewUint32(context, static_cast<std::uint32_t>(state->path_count));
}

JSValue pathset_at(JSContext *context, JSValueConst this_value, int argc,
                   JSValueConst *argv) {
  auto *state = static_cast<PathSetState *>(
      JS_GetOpaque2(context, this_value, g_pathset_class_id));
  if (state == nullptr)
    return JS_EXCEPTION;
  std::uint32_t index = 0;
  if (!read_index(context, argc, argv, index))
    return JS_EXCEPTION;
  if (index >= state->path_count)
    return JS_ThrowRangeError(context, "PathSet index out of range");
  return new_path(context, this_value, index, state->disabled_mark,
                  state->counters);
}

PathSetState *path_parent(JSContext *context, PathState const *path) {
  return static_cast<PathSetState *>(
      JS_GetOpaque2(context, path->parent, g_pathset_class_id));
}

JSValue path_length_getter(JSContext *context, JSValueConst this_value) {
  auto *path = static_cast<PathState *>(
      JS_GetOpaque2(context, this_value, g_path_class_id));
  if (path == nullptr)
    return JS_EXCEPTION;
  auto *parent = path_parent(context, path);
  if (parent == nullptr)
    return JS_EXCEPTION;
  if (path->path_index >= parent->path_count)
    return JS_ThrowInternalError(context, "Path directory index invalid");
  std::uint32_t const *starts = parent->directory;
  std::uint32_t const length =
      starts[path->path_index + 1] - starts[path->path_index];
  return JS_NewUint32(context, length);
}

JSValue path_at(JSContext *context, JSValueConst this_value, int argc,
                JSValueConst *argv) {
  auto *path = static_cast<PathState *>(
      JS_GetOpaque2(context, this_value, g_path_class_id));
  if (path == nullptr)
    return JS_EXCEPTION;
  auto *parent = path_parent(context, path);
  if (parent == nullptr)
    return JS_EXCEPTION;
  std::uint32_t local_index = 0;
  if (!read_index(context, argc, argv, local_index))
    return JS_EXCEPTION;
  std::uint32_t const begin = parent->directory[path->path_index];
  std::uint32_t const end = parent->directory[path->path_index + 1];
  if (local_index >= end - begin)
    return JS_ThrowRangeError(context, "Path index out of range");
  return new_pathhop(context, path->parent, begin + local_index,
                     parent->disabled_mark, parent->counters);
}

struct HopComponents {
  std::uint8_t type = 0;
  std::uint8_t const *account = nullptr;
  std::uint8_t const *currency = nullptr;
  std::uint8_t const *issuer = nullptr;
};

bool resolve_hop(PathSetState const &parent, std::uint32_t ordinal,
                 HopComponents &out) {
  if (ordinal >= parent.hop_count)
    return false;
  std::size_t const path_words = parent.path_count + 1;
  std::size_t const offset = parent.directory[path_words + ordinal];
  if (offset >= parent.payload.size())
    return false;
  auto const *bytes = parent.payload.data();
  out.type = bytes[offset];
  std::size_t cursor = offset + 1;

  auto take = [&](std::uint8_t const *&component) {
    if (parent.payload.size() - cursor < 20)
      return false;
    component = bytes + cursor;
    cursor += 20;
    return true;
  };

  if ((out.type & PathSet::TYPE_ACCOUNT) != 0 && !take(out.account))
    return false;
  if ((out.type & PathSet::TYPE_CURRENCY) != 0 && !take(out.currency))
    return false;
  if ((out.type & PathSet::TYPE_ISSUER) != 0 && !take(out.issuer))
    return false;
  return true;
}

PathSetState *pathhop_parent(JSContext *context, PathHopState const *hop) {
  return static_cast<PathSetState *>(
      JS_GetOpaque2(context, hop->parent, g_pathset_class_id));
}

JSValue pathhop_type_getter(JSContext *context, JSValueConst this_value) {
  auto *hop = static_cast<PathHopState *>(
      JS_GetOpaque2(context, this_value, g_pathhop_class_id));
  if (hop == nullptr)
    return JS_EXCEPTION;
  auto *parent = pathhop_parent(context, hop);
  if (parent == nullptr)
    return JS_EXCEPTION;
  HopComponents components;
  if (!resolve_hop(*parent, hop->hop_ordinal, components))
    return JS_ThrowInternalError(context, "PathHop directory invalid");
  return JS_NewUint32(context, components.type);
}

enum class Component : std::uint8_t {
  account,
  currency,
  issuer,
};

JSValue pathhop_component(JSContext *context, JSValueConst this_value,
                          Component component) {
  auto *hop = static_cast<PathHopState *>(
      JS_GetOpaque2(context, this_value, g_pathhop_class_id));
  if (hop == nullptr)
    return JS_EXCEPTION;
  auto *parent = pathhop_parent(context, hop);
  if (parent == nullptr)
    return JS_EXCEPTION;
  HopComponents resolved;
  if (!resolve_hop(*parent, hop->hop_ordinal, resolved))
    return JS_ThrowInternalError(context, "PathHop directory invalid");

  std::uint8_t const *source = nullptr;
  std::array<std::uint8_t, 20> *cache = nullptr;
  std::uint8_t cache_bit = 0;
  std::uint64_t *copy_count = nullptr;
  switch (component) {
  case Component::account:
    source = resolved.account;
    cache = &hop->account;
    cache_bit = kAccountCached;
    copy_count = &hop->counters->account_component_copies;
    break;
  case Component::currency:
    source = resolved.currency;
    cache = &hop->currency;
    cache_bit = kCurrencyCached;
    copy_count = &hop->counters->currency_component_copies;
    break;
  case Component::issuer:
    source = resolved.issuer;
    cache = &hop->issuer;
    cache_bit = kIssuerCached;
    copy_count = &hop->counters->issuer_component_copies;
    break;
  }
  if (source == nullptr)
    return JS_UNDEFINED;
  if ((hop->cached_components & cache_bit) == 0) {
    std::memcpy(cache->data(), source, cache->size());
    hop->cached_components |= cache_bit;
    ++*copy_count;
  }
  return qjs::uint8Array(
      context, std::span<std::uint8_t const>{cache->data(), cache->size()});
}

JSValue pathhop_account_getter(JSContext *context, JSValueConst this_value) {
  return pathhop_component(context, this_value, Component::account);
}

JSValue pathhop_currency_getter(JSContext *context, JSValueConst this_value) {
  return pathhop_component(context, this_value, Component::currency);
}

JSValue pathhop_issuer_getter(JSContext *context, JSValueConst this_value) {
  return pathhop_component(context, this_value, Component::issuer);
}

JSCFunctionListEntry const g_facade_proto[] = {
    JS_CGETSET_DEF("Paths", facade_paths_getter, nullptr),
};

JSCFunctionListEntry const g_pathset_proto[] = {
    JS_CGETSET_DEF("length", pathset_length_getter, nullptr),
    JS_CFUNC_DEF("at", 1, pathset_at),
};

JSCFunctionListEntry const g_path_proto[] = {
    JS_CGETSET_DEF("length", path_length_getter, nullptr),
    JS_CFUNC_DEF("at", 1, path_at),
};

JSCFunctionListEntry const g_pathhop_proto[] = {
    JS_CGETSET_DEF("type", pathhop_type_getter, nullptr),
    JS_CGETSET_DEF("account", pathhop_account_getter, nullptr),
    JS_CGETSET_DEF("currency", pathhop_currency_getter, nullptr),
    JS_CGETSET_DEF("issuer", pathhop_issuer_getter, nullptr),
};

bool install_empty_prototype(JSContext *context, JSClassID class_id) {
  return qjs::installPrototype(context, class_id,
                               std::span<JSCFunctionListEntry const>{});
}

std::uint64_t source_finalizers(PathSetQuickJSCounters const &counters,
                                PathSetMarkEdge edge) noexcept {
  switch (edge) {
  case PathSetMarkEdge::facade:
    return counters.facades_finalized;
  case PathSetMarkEdge::pathset:
    return counters.pathsets_finalized;
  case PathSetMarkEdge::path:
    return counters.paths_finalized;
  case PathSetMarkEdge::pathhop:
    return counters.pathhops_finalized;
  case PathSetMarkEdge::none:
    return 0;
  }
  return 0;
}

} // namespace

char const *pathset_mark_edge_name(PathSetMarkEdge edge) noexcept {
  switch (edge) {
  case PathSetMarkEdge::none:
    return "none";
  case PathSetMarkEdge::facade:
    return "facade";
  case PathSetMarkEdge::pathset:
    return "pathset";
  case PathSetMarkEdge::path:
    return "path";
  case PathSetMarkEdge::pathhop:
    return "pathhop";
  }
  return "unknown";
}

bool parse_pathset_mark_edge(char const *name, PathSetMarkEdge &edge) noexcept {
  if (name == nullptr)
    return false;
  std::string_view const value{name};
  for (PathSetMarkEdge const candidate :
       {PathSetMarkEdge::facade, PathSetMarkEdge::pathset,
        PathSetMarkEdge::path, PathSetMarkEdge::pathhop}) {
    if (value == pathset_mark_edge_name(candidate)) {
      edge = candidate;
      return true;
    }
  }
  return false;
}

PathSetQuickJSFixture::PathSetQuickJSFixture(
    PathSetMarkEdge disabled_mark) noexcept
    : disabled_mark_(disabled_mark) {
  runtime_ = JS_NewRuntime2(&g_test_allocator, &allocator_control_);
  if (runtime_ == nullptr) {
    init_error_ = "JS_NewRuntime2 failed";
    return;
  }
  context_ = JS_NewContextRaw(runtime_);
  if (context_ == nullptr) {
    init_error_ = "JS_NewContextRaw failed";
    return;
  }

  // Exact v1 provider subset. Deliberately omit JS_AddIntrinsicWeakRef.
  if (JS_AddIntrinsicBaseObjects(context_) || JS_AddIntrinsicDate(context_) ||
      JS_AddIntrinsicEval(context_) ||
      JS_AddIntrinsicStringNormalize(context_) ||
      JS_AddIntrinsicRegExp(context_) || JS_AddIntrinsicJSON(context_) ||
      JS_AddIntrinsicProxy(context_) || JS_AddIntrinsicMapSet(context_) ||
      JS_AddIntrinsicTypedArrays(context_) ||
      JS_AddIntrinsicPromise(context_)) {
    init_error_ = "v1 intrinsic initialization failed";
    return;
  }
  if (!apply_v1_sandbox_subset(context_)) {
    init_error_ = "v1 sandbox initialization failed";
    return;
  }
  if (!register_classes()) {
    init_error_ = "private PathSet class registration failed";
    return;
  }
  if (!v1_sandbox_builtins_absent()) {
    init_error_ = "v1 context unexpectedly exposes disabled builtins";
    return;
  }
}

PathSetQuickJSFixture::~PathSetQuickJSFixture() {
  if (context_ != nullptr && cleanup_edge_ != PathSetMarkEdge::none)
    cleanup_red_cycle();
  if (context_ != nullptr)
    JS_FreeContext(context_);
  if (runtime_ != nullptr)
    JS_FreeRuntime(runtime_);
}

bool PathSetQuickJSFixture::register_classes() noexcept {
  if (!qjs::defineClass(runtime_, &g_owner_class_id, &g_owner_class) ||
      !qjs::defineClass(runtime_, &g_facade_class_id, &g_facade_class) ||
      !qjs::defineClass(runtime_, &g_pathset_class_id, &g_pathset_class) ||
      !qjs::defineClass(runtime_, &g_path_class_id, &g_path_class) ||
      !qjs::defineClass(runtime_, &g_pathhop_class_id, &g_pathhop_class))
    return false;

  return install_empty_prototype(context_, g_owner_class_id) &&
         qjs::installPrototype(
             context_, g_facade_class_id,
             std::span<JSCFunctionListEntry const>{g_facade_proto}) &&
         qjs::installPrototype(
             context_, g_pathset_class_id,
             std::span<JSCFunctionListEntry const>{g_pathset_proto}) &&
         qjs::installPrototype(
             context_, g_path_class_id,
             std::span<JSCFunctionListEntry const>{g_path_proto}) &&
         qjs::installPrototype(
             context_, g_pathhop_class_id,
             std::span<JSCFunctionListEntry const>{g_pathhop_proto});
}

bool PathSetQuickJSFixture::ready() const noexcept {
  return init_error_ == nullptr && runtime_ != nullptr && context_ != nullptr;
}

char const *PathSetQuickJSFixture::init_error() const noexcept {
  return init_error_ == nullptr ? "" : init_error_;
}

JSContext *PathSetQuickJSFixture::context() const noexcept { return context_; }

JSRuntime *PathSetQuickJSFixture::runtime() const noexcept { return runtime_; }

PathSetQuickJSCounters const &PathSetQuickJSFixture::counters() const noexcept {
  return counters_;
}

bool PathSetQuickJSFixture::v1_sandbox_builtins_absent() const noexcept {
  if (context_ == nullptr)
    return false;
  LocalValue global(context_, JS_GetGlobalObject(context_));
  if (global.is_exception())
    return false;
  LocalValue weak_ref(context_,
                      JS_GetPropertyStr(context_, global.get(), "WeakRef"));
  LocalValue finalization_registry(
      context_,
      JS_GetPropertyStr(context_, global.get(), "FinalizationRegistry"));
  LocalValue shared_array_buffer(
      context_, JS_GetPropertyStr(context_, global.get(), "SharedArrayBuffer"));
  LocalValue atomics(context_,
                     JS_GetPropertyStr(context_, global.get(), "Atomics"));
  LocalValue math(context_, JS_GetPropertyStr(context_, global.get(), "Math"));
  if (math.is_exception())
    return false;
  LocalValue random(context_,
                    JS_GetPropertyStr(context_, math.get(), "random"));
  return !weak_ref.is_exception() && !finalization_registry.is_exception() &&
         !shared_array_buffer.is_exception() && !atomics.is_exception() &&
         !random.is_exception() && JS_IsUndefined(weak_ref.get()) &&
         JS_IsUndefined(finalization_registry.get()) &&
         JS_IsUndefined(shared_array_buffer.get()) &&
         JS_IsUndefined(atomics.get()) && JS_IsUndefined(random.get());
}

JSValue
PathSetQuickJSFixture::make_facade(std::span<std::uint8_t const> object_bytes,
                                   std::size_t pathset_ordinal) {
  if (!ready())
    return JS_EXCEPTION;
  auto const protocol = Protocol::load_embedded_xahau_protocol();
  auto root = CertifiedRoot::copy_and_certify(
      Slice{object_bytes.data(), object_bytes.size()}, 0, protocol);
  if (!root)
    return JS_ThrowTypeError(context_, "object certification failed: %s",
                             root.error().message.c_str());

  JSValue owner =
      new_owner(context_, CertifiedObject{std::move(*root)}, &counters_);
  if (JS_IsException(owner))
    return owner;
  JSValue facade = new_facade(context_, owner, pathset_ordinal, disabled_mark_,
                              &allocator_control_, &counters_);
  JS_FreeValue(context_, owner);
  return facade;
}

JSValue PathSetQuickJSFixture::paths(JSValueConst facade) {
  return JS_GetPropertyStr(context_, facade, "Paths");
}

JSValue PathSetQuickJSFixture::at(JSValueConst parent, std::uint32_t index) {
  LocalValue method(context_, JS_GetPropertyStr(context_, parent, "at"));
  if (method.is_exception())
    return JS_EXCEPTION;
  JSValue argument = JS_NewUint32(context_, index);
  return JS_Call(context_, method.get(), parent, 1, &argument);
}

JSValue PathSetQuickJSFixture::property(JSValueConst object, char const *name) {
  return JS_GetPropertyStr(context_, object, name);
}

bool PathSetQuickJSFixture::facade_has_cached_pathset(
    JSValueConst facade) const noexcept {
  auto const *state =
      static_cast<FacadeState const *>(JS_GetOpaque(facade, g_facade_class_id));
  return state != nullptr && !JS_IsUndefined(state->cached_pathset);
}

bool PathSetQuickJSFixture::directory_snapshot(
    JSValueConst pathset, std::span<std::uint32_t> output,
    std::size_t &path_count, std::size_t &hop_count) const noexcept {
  auto const *state = static_cast<PathSetState const *>(
      JS_GetOpaque(pathset, g_pathset_class_id));
  if (state == nullptr ||
      output.size() != state->path_count + 1 + state->hop_count)
    return false;
  path_count = state->path_count;
  hop_count = state->hop_count;
  std::memcpy(output.data(), state->directory, output.size_bytes());
  return true;
}

void PathSetQuickJSFixture::fail_next_directory_allocation() noexcept {
  allocator_control_.fail_next_directory = true;
}

std::string PathSetQuickJSFixture::take_exception_message() {
  if (context_ == nullptr || !JS_HasException(context_))
    return {};
  LocalValue exception(context_, JS_GetException(context_));
  LocalValue string(context_, JS_ToString(context_, exception.get()));
  if (string.is_exception())
    return "<exception rendering failed>";
  std::size_t length = 0;
  char const *chars = JS_ToCStringLen(context_, &length, string.get());
  if (chars == nullptr)
    return "<exception string unavailable>";
  std::string result(chars, length);
  JS_FreeCString(context_, chars);
  return result;
}

void PathSetQuickJSFixture::run_gc() noexcept {
  if (runtime_ != nullptr)
    JS_RunGC(runtime_);
}

bool PathSetQuickJSFixture::plant_cycle(PathSetMarkEdge edge,
                                        JSValueConst facade) {
  if (edge == PathSetMarkEdge::none || context_ == nullptr)
    return false;
  LocalValue pathset(context_, paths(facade));
  if (pathset.is_exception())
    return false;

  JSValueConst source = JS_UNDEFINED;
  JSValueConst property_owner = JS_UNDEFINED;
  JSValueConst property_value = JS_UNDEFINED;
  std::optional<LocalValue> path;
  std::optional<LocalValue> hop;

  switch (edge) {
  case PathSetMarkEdge::facade:
    source = facade;
    property_owner = pathset.get();
    property_value = facade;
    break;
  case PathSetMarkEdge::pathset: {
    auto *state = static_cast<PathSetState *>(
        JS_GetOpaque(pathset.get(), g_pathset_class_id));
    if (state == nullptr)
      return false;
    source = pathset.get();
    property_owner = state->owner;
    property_value = pathset.get();
    break;
  }
  case PathSetMarkEdge::path:
    path.emplace(context_, at(pathset.get(), 0));
    if (path->is_exception())
      return false;
    source = path->get();
    property_owner = pathset.get();
    property_value = path->get();
    break;
  case PathSetMarkEdge::pathhop:
    path.emplace(context_, at(pathset.get(), 0));
    if (path->is_exception())
      return false;
    hop.emplace(context_, at(path->get(), 0));
    if (hop->is_exception())
      return false;
    source = hop->get();
    property_owner = pathset.get();
    property_value = hop->get();
    break;
  case PathSetMarkEdge::none:
    return false;
  }

  if (JS_SetPropertyStr(context_, property_owner, "__pathset_gc_cycle",
                        JS_DupValue(context_, property_value)) < 0)
    return false;

  if (disabled_mark_ == edge) {
    cleanup_source_ = source;
    cleanup_edge_ = edge;
  }
  return true;
}

bool PathSetQuickJSFixture::cycle_collected(
    PathSetMarkEdge edge) const noexcept {
  if (edge == PathSetMarkEdge::none || counters_.owners_created == 0)
    return false;
  return counters_.owners_created == counters_.owners_finalized &&
         counters_.facades_created == counters_.facades_finalized &&
         counters_.pathsets_created == counters_.pathsets_finalized &&
         counters_.paths_created == counters_.paths_finalized &&
         counters_.pathhops_created == counters_.pathhops_finalized &&
         counters_.directory_allocations == counters_.directory_frees &&
         counters_.directory_bytes_allocated == counters_.directory_bytes_freed;
}

bool PathSetQuickJSFixture::cleanup_red_cycle() noexcept {
  if (cleanup_edge_ == PathSetMarkEdge::none || context_ == nullptr)
    return true;
  PathSetMarkEdge const edge = cleanup_edge_;

  // If the deliberately broken edge did not retain its source, the borrowed
  // handle is stale and must not be touched.
  if (source_finalizers(counters_, edge) != 0) {
    cleanup_source_ = JS_UNDEFINED;
    cleanup_edge_ = PathSetMarkEdge::none;
    return cycle_collected(edge);
  }

  switch (edge) {
  case PathSetMarkEdge::facade: {
    auto *state = static_cast<FacadeState *>(
        JS_GetOpaque(cleanup_source_, g_facade_class_id));
    if (state == nullptr)
      return false;
    state->mark_enabled = true;
    break;
  }
  case PathSetMarkEdge::pathset: {
    auto *state = static_cast<PathSetState *>(
        JS_GetOpaque(cleanup_source_, g_pathset_class_id));
    if (state == nullptr)
      return false;
    state->mark_enabled = true;
    break;
  }
  case PathSetMarkEdge::path: {
    auto *state = static_cast<PathState *>(
        JS_GetOpaque(cleanup_source_, g_path_class_id));
    if (state == nullptr)
      return false;
    state->mark_enabled = true;
    break;
  }
  case PathSetMarkEdge::pathhop: {
    auto *state = static_cast<PathHopState *>(
        JS_GetOpaque(cleanup_source_, g_pathhop_class_id));
    if (state == nullptr)
      return false;
    state->mark_enabled = true;
    break;
  }
  case PathSetMarkEdge::none:
    return true;
  }

  cleanup_source_ = JS_UNDEFINED;
  cleanup_edge_ = PathSetMarkEdge::none;
  JS_RunGC(runtime_);
  return cycle_collected(edge);
}

} // namespace catl::xdata::test
