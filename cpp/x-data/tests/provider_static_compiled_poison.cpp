#include "catl/xdata/static_protocol.h"

#include <cstddef>
#include <cstdint>

#if defined(JSHOOKZ_POISON_DYNAMIC_PROTOCOL)
#include "catl/xdata/protocol.h"
#elif defined(JSHOOKZ_POISON_FIELD_TYPES)
#include "catl/xdata/types.h"
#elif defined(JSHOOKZ_POISON_VECTOR)
#include <vector>
#elif defined(JSHOOKZ_POISON_STRING)
#include <string>
#elif defined(JSHOOKZ_POISON_BAD_ALLOC) || defined(JSHOOKZ_POISON_ALIGNED_NEW)
#include <new>
#elif defined(JSHOOKZ_POISON_MALLOC)
#include <cstdlib>
#endif

namespace {

#if defined(JSHOOKZ_POISON_MALLOC)
void *pre_main_allocation = std::malloc(1);
#endif

[[nodiscard]] std::uintptr_t exercise_poison() {
#if defined(JSHOOKZ_POISON_DYNAMIC_PROTOCOL)
  catl::xdata::Protocol protocol;
  return protocol.fields().size();
#elif defined(JSHOOKZ_POISON_FIELD_TYPES)
  return catl::xdata::FieldTypes::ALL.size();
#elif defined(JSHOOKZ_POISON_NEW)
  auto *value = new std::uint64_t{1};
  auto const result = *value;
  delete value;
  return result;
#elif defined(JSHOOKZ_POISON_ALIGNED_NEW)
  void *value = ::operator new(64, std::align_val_t{64});
  ::operator delete(value, std::align_val_t{64});
  return value != nullptr;
#elif defined(JSHOOKZ_POISON_VECTOR)
  std::vector<std::uint64_t> values(32, 1);
  return values.size();
#elif defined(JSHOOKZ_POISON_STRING)
  std::string value(128, 'x');
  return value.size();
#elif defined(JSHOOKZ_POISON_BAD_ALLOC)
  // Keep the real route linked without invoking its terminating no-exception
  // implementation when the poison executable is started.
  auto *route = &std::__throw_bad_alloc;
  return reinterpret_cast<std::uintptr_t>(route) != 0;
#elif defined(JSHOOKZ_POISON_MALLOC)
  auto const result = pre_main_allocation != nullptr;
  std::free(pre_main_allocation);
  pre_main_allocation = nullptr;
  return result;
#else
#error "A provider-static poison must be selected"
#endif
}

} // namespace

int main() {
  auto const &protocol = catl::xdata::xahau_static_protocol();
  if (protocol.field_count != 327)
    return 2;
  volatile auto observed = exercise_poison();
  (void)observed;
  return 0;
}
