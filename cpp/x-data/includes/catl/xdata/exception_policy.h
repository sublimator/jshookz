#pragma once

#include <utility>

namespace catl::xdata {

template <typename E>
[[noreturn]] inline void
throw_or_terminate(E&& e)
{
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    throw std::forward<E>(e);
#else
    (void)e;
    __builtin_trap();
    __builtin_unreachable();
#endif
}

}  // namespace catl::xdata

#define CATL_XDATA_THROW(exception_expr) \
    ::catl::xdata::throw_or_terminate((exception_expr))
