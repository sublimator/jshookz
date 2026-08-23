#include "catl/xdata/static_protocol.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

std::size_t allocation_calls = 0;

[[noreturn]] void allocation_failed() noexcept { std::abort(); }

} // namespace

void *operator new(std::size_t size) {
  ++allocation_calls;
  if (void *value = std::malloc(size == 0 ? 1 : size))
    return value;
  allocation_failed();
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void *operator new(std::size_t size, std::align_val_t alignment) {
  ++allocation_calls;
  void *value = nullptr;
  auto const width = static_cast<std::size_t>(alignment);
  if (posix_memalign(&value, width, size == 0 ? width : size) == 0)
    return value;
  allocation_failed();
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

void operator delete(void *value) noexcept { std::free(value); }

void operator delete(void *value, std::size_t) noexcept { std::free(value); }

void operator delete(void *value, std::align_val_t) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t, std::align_val_t) noexcept {
  std::free(value);
}

void operator delete[](void *value) noexcept { std::free(value); }

void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

void operator delete[](void *value, std::align_val_t) noexcept {
  std::free(value);
}

void operator delete[](void *value, std::size_t, std::align_val_t) noexcept {
  std::free(value);
}

int main() {
  if (allocation_calls != 0)
    return 1;
  auto const &protocol = catl::xdata::xahau_static_protocol();
  if (protocol.field_count != 327)
    return 2;
  return allocation_calls == 0 ? 0 : 3;
}
